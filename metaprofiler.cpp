// ============================================================
//  MetaProfiler - Persistent Player Profile Engine for AMX Mod X
//  Version: 1.1.0
//
//  Every player gets one .ini profile file:
//      addons/amxmodx/data/metaprofiler/<key>.ini
//
//  The module is the storage engine + API. A small collector
//  plugin (metaprofiler.sma) feeds it events (connect, chat, kills).
//
//  Features
//   - One profile per player (key = name / steamid / ip, selectable)
//   - Safe file names (sanitised, lower-cased, hash suffix on collisions)
//   - Generic key/value API: any plugin can read/write any section
//   - Typed values: string, int, float, add_int (counters)
//   - Compare / check / diff / copy between two profiles
//   - Log sections: History, Chat, Gameplay, Names, IPs (size-capped)
//   - Offline profiles can be loaded and read too (LRU cache)
//   - Atomic saves (write .tmp then replace), dirty tracking
//   - Directory scan (list every profile on disk)
//
//  In moduleconfig.h enable BOTH (needed for the forwards, recommended anyway):
//     #define FN_AMXX_PLUGINSLOADED   OnPluginsLoaded
//     #define FN_AMXX_PLUGINSUNLOADED OnPluginsUnloaded
// ============================================================

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "amxxmodule.h"

#ifdef _WIN32
	#include <windows.h>
	#include <direct.h>
	#define MP_MKDIR(p) _mkdir(p)
#else
	#include <dirent.h>
	#include <sys/stat.h>
	#include <sys/types.h>
	#define MP_MKDIR(p) mkdir(p, 0755)
#endif

#define MP_VERSION    "1.1.0"
#define MP_VERSION_NUM  110
#define MP_OUT        MF_PrintSrvConsole
#define MP_DATA_DIR   "addons/amxmodx/data/metaprofiler"

#define MP_MAX_PROFILES   255
#define MP_MAX_PLAYERS    32
#define MP_MAX_LINE       480

// ============================================================
//  Enums (must match metaprofiler.inc)
// ============================================================

enum MPLogType
{
	MPL_HISTORY = 0,
	MPL_CHAT,
	MPL_GAMEPLAY,
	MPL_NAMES,
	MPL_IPS,
	MPL_COUNT
};

enum MPKeyMode
{
	MPK_NAME   = 0,
	MPK_AUTHID = 1,
	MPK_IP     = 2
};

enum MPOp
{
	MPO_EQ = 0, MPO_NE, MPO_GT, MPO_GE, MPO_LT, MPO_LE
};

static const char* MP_LOG_SECTION[MPL_COUNT] = { "History", "Chat", "Gameplay", "Names", "IPs" };
static int g_LogLimit[MPL_COUNT] = { 500, 500, 500, 25, 25 };

// ============================================================
//  Data structures
// ============================================================

struct KV
{
	std::string key;
	std::string val;
};

struct Section
{
	std::string     name;
	std::vector<KV> kv;
};

struct Profile
{
	bool          used;
	unsigned int  gen;
	std::string   key;
	bool          dirty;
	int           online;
	unsigned long lastUse;

	std::vector<Section>     sections;
	std::vector<std::string> logs[MPL_COUNT];

	Profile() : used(false), gen(0), dirty(false), online(0), lastUse(0) {}
};

struct PlayerSlot
{
	int         slot;
	time_t      sessionStart;
	std::string name;
	std::string authid;
	std::string ip;

	PlayerSlot() : slot(-1), sessionStart(0) {}
};

static std::vector<Profile>     g_Profiles;
static PlayerSlot               g_Players[MP_MAX_PLAYERS + 1];
static int                      g_KeyMode    = MPK_NAME;
static unsigned int             g_GenCounter = 0;
static unsigned long            g_UseCounter = 0;
static unsigned int             g_Saves      = 0;
static unsigned int             g_Loads      = 0;
static std::vector<std::string> g_Scan;

// forwards (registered in OnPluginsLoaded)
static int  g_FwdCreated = -1;
static int  g_FwdLoaded  = -1;
static int  g_FwdClosing = -1;
static int  g_FwdRenamed = -1;
static bool g_FwdsReady  = false;

// ============================================================
//  String helpers
// ============================================================

static std::string Trimmed(const std::string& s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

static std::string Lower(std::string s)
{
	for (size_t i = 0; i < s.size(); i++)
		s[i] = (char)tolower((unsigned char)s[i]);
	return s;
}

static bool IEq(const std::string& a, const std::string& b)
{
	return Lower(a) == Lower(b);
}

static int ICmp(const std::string& a, const std::string& b)
{
	int r = Lower(a).compare(Lower(b));
	return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

static bool IContains(const std::string& hay, const std::string& needle)
{
	return Lower(hay).find(Lower(needle)) != std::string::npos;
}

// section / key names: no characters that break the ini format
static std::string CleanToken(std::string s)
{
	for (size_t i = 0; i < s.size(); i++)
	{
		char c = s[i];
		if (c == '[' || c == ']' || c == '=' || c == ';' || c == '\r' || c == '\n' || c == '\t')
			s[i] = '_';
	}
	return Trimmed(s);
}

// values: single line only
static std::string CleanValue(std::string s)
{
	for (size_t i = 0; i < s.size(); i++)
	{
		char c = s[i];
		if (c == '\r' || c == '\n' || c == '\t')
			s[i] = ' ';
	}
	return Trimmed(s);
}

static std::string NowString(time_t t = 0)
{
	if (!t) t = time(nullptr);
	struct tm* lt = localtime(&t);
	char buf[96];
	if (lt)
		snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
			lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
	else
		snprintf(buf, sizeof(buf), "0000-00-00 00:00:00");
	return buf;
}

static std::string Str(AMX* amx, cell param)
{
	int len;
	char* s = MF_GetAmxString(amx, param, 0, &len);
	return std::string(s ? s : "");
}

static int LogTypeOf(const std::string& name)
{
	for (int i = 0; i < MPL_COUNT; i++)
		if (IEq(name, MP_LOG_SECTION[i])) return i;
	return -1;
}

static unsigned int HashString(const std::string& s)
{
	unsigned int h = 2166136261u;
	for (size_t i = 0; i < s.size(); i++)
	{
		h ^= (unsigned char)s[i];
		h *= 16777619u;
	}
	return h;
}

// ============================================================
//  Profile key -> safe file name
// ============================================================

static std::string MakeKey(const std::string& rawIn)
{
	std::string raw = Trimmed(rawIn);
	std::string out;
	bool altered = false;

	for (size_t i = 0; i < raw.size() && out.size() < 48; i++)
	{
		unsigned char c = (unsigned char)raw[i];

		if (c < 32 || c >= 127 || strchr("<>:\"/\\|?*=;[]%", c))
		{
			out += '_';
			altered = true;
		}
		else
		{
			out += (char)tolower(c);
		}
	}
	if (raw.size() > 48) altered = true;

	// Windows does not allow trailing dots / spaces
	while (!out.empty() && (out[out.size() - 1] == '.' || out[out.size() - 1] == ' '))
		out.erase(out.size() - 1);

	if (out.empty())
		out = "_unnamed";

	// reserved Windows device names
	static const char* reserved[] = { "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
		"com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9", nullptr };
	for (int i = 0; reserved[i]; i++)
		if (out == reserved[i]) { out = "_" + out; break; }

	if (altered)
	{
		char suf[16];
		snprintf(suf, sizeof(suf), "~%06x", HashString(raw) & 0xFFFFFF);
		out += suf;
	}
	return out;
}

static void EnsureDir()
{
	char dir[512];
	MF_BuildPathnameR(dir, sizeof(dir), "%s", MP_DATA_DIR);
	MP_MKDIR(dir);		// harmless if it already exists
}

static std::string ProfilePath(const std::string& key)
{
	char buf[640];
	MF_BuildPathnameR(buf, sizeof(buf), "%s/%s.ini", MP_DATA_DIR, key.c_str());
	return buf;
}

static bool FileExists(const std::string& path)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

/* Replaces path with tmp in one step: the old file is never missing in between */
static bool ReplaceFile(const std::string& tmp, const std::string& path)
{
#ifdef _WIN32
	return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	return rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

// ============================================================
//  Sections / values
// ============================================================

static Section* FindSection(Profile& p, const std::string& name, bool create)
{
	for (size_t i = 0; i < p.sections.size(); i++)
		if (IEq(p.sections[i].name, name)) return &p.sections[i];

	if (!create) return nullptr;

	Section s;
	s.name = name;
	p.sections.push_back(s);
	return &p.sections.back();
}

static KV* FindKV(Section& s, const std::string& key)
{
	for (size_t i = 0; i < s.kv.size(); i++)
		if (IEq(s.kv[i].key, key)) return &s.kv[i];
	return nullptr;
}

static bool PSet(Profile& p, const std::string& secRaw, const std::string& keyRaw, const std::string& val)
{
	std::string sec = CleanToken(secRaw), key = CleanToken(keyRaw);
	if (sec.empty() || key.empty() || LogTypeOf(sec) >= 0)
		return false;

	std::string v = CleanValue(val);
	Section* s = FindSection(p, sec, true);
	KV* kv = FindKV(*s, key);

	if (kv)
	{
		if (kv->val == v) return true;
		kv->val = v;
	}
	else
	{
		KV n; n.key = key; n.val = v;
		s->kv.push_back(n);
	}
	p.dirty = true;
	return true;
}

static bool PGet(Profile& p, const std::string& sec, const std::string& key, std::string& out)
{
	Section* s = FindSection(p, CleanToken(sec), false);
	if (!s) return false;
	KV* kv = FindKV(*s, CleanToken(key));
	if (!kv) return false;
	out = kv->val;
	return true;
}

static bool PHas(Profile& p, const std::string& sec, const std::string& key)
{
	std::string tmp;
	return PGet(p, sec, key, tmp);
}

static int PGetInt(Profile& p, const std::string& sec, const std::string& key, int def)
{
	std::string v;
	if (!PGet(p, sec, key, v) || v.empty()) return def;
	return (int)strtol(v.c_str(), nullptr, 10);
}

static double PGetFloat(Profile& p, const std::string& sec, const std::string& key, double def)
{
	std::string v;
	if (!PGet(p, sec, key, v) || v.empty()) return def;
	return strtod(v.c_str(), nullptr);
}

static bool PSetInt(Profile& p, const std::string& sec, const std::string& key, int val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", val);
	return PSet(p, sec, key, buf);
}

static int PAddInt(Profile& p, const std::string& sec, const std::string& key, int amount)
{
	int v = PGetInt(p, sec, key, 0) + amount;
	PSetInt(p, sec, key, v);
	return v;
}

static bool PRemove(Profile& p, const std::string& secRaw, const std::string& keyRaw)
{
	Section* s = FindSection(p, CleanToken(secRaw), false);
	if (!s) return false;

	std::string key = CleanToken(keyRaw);
	for (size_t i = 0; i < s->kv.size(); i++)
	{
		if (IEq(s->kv[i].key, key))
		{
			s->kv.erase(s->kv.begin() + i);
			p.dirty = true;
			return true;
		}
	}
	return false;
}

// ---- logs ---------------------------------------------------

static void TrimLog(Profile& p, int type)
{
	std::vector<std::string>& v = p.logs[type];
	int limit = g_LogLimit[type];
	if ((int)v.size() > limit)
		v.erase(v.begin(), v.begin() + (v.size() - limit));
}

static void AddLog(Profile& p, int type, const std::string& textIn)
{
	std::string text = CleanValue(textIn);
	if (text.size() > MP_MAX_LINE) text.resize(MP_MAX_LINE);

	p.logs[type].push_back(NowString() + " | " + text);
	TrimLog(p, type);
	p.dirty = true;
}

// names / ips: unique values only
static void AddUnique(Profile& p, int type, const std::string& valIn)
{
	std::string val = CleanValue(valIn);
	if (val.empty()) return;

	std::vector<std::string>& v = p.logs[type];
	for (size_t i = 0; i < v.size(); i++)
		if (v[i] == val) return;

	v.push_back(val);
	TrimLog(p, type);
	p.dirty = true;
}

// ============================================================
//  File I/O
// ============================================================

static bool ParseFile(const std::string& path, Profile& p)
{
	FILE* f = fopen(path.c_str(), "r");
	if (!f) return false;

	char line[2048];
	Section* cur = nullptr;
	int logType = -1;

	while (fgets(line, sizeof(line), f))
	{
		std::string s = Trimmed(line);
		if (s.empty() || s[0] == ';' || s[0] == '#')
			continue;

		if (s[0] == '[' && s[s.size() - 1] == ']')
		{
			std::string name = Trimmed(s.substr(1, s.size() - 2));
			logType = LogTypeOf(name);
			cur = (logType < 0) ? FindSection(p, name, true) : nullptr;
			continue;
		}

		if (logType >= 0)
		{
			if (logType >= MPL_NAMES && s.size() >= 2 && s[0] == '-' && s[1] == ' ')
				s = Trimmed(s.substr(2));
			p.logs[logType].push_back(s);
			continue;
		}

		if (!cur) continue;

		size_t eq = s.find('=');
		if (eq == std::string::npos) continue;

		std::string key = Trimmed(s.substr(0, eq));
		std::string val = Trimmed(s.substr(eq + 1));
		if (key.empty()) continue;

		KV* kv = FindKV(*cur, key);
		if (kv) kv->val = val;
		else { KV n; n.key = key; n.val = val; cur->kv.push_back(n); }
	}
	fclose(f);

	for (int t = 0; t < MPL_COUNT; t++)
		TrimLog(p, t);

	return true;
}

static bool SaveProfile(Profile& p)
{
	EnsureDir();

	std::string path = ProfilePath(p.key);
	std::string tmp  = path + ".tmp";

	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
	{
		MP_OUT("[MetaProfiler] ERROR: cannot write %s\n", tmp.c_str());
		return false;
	}

	fprintf(f, "; ==============================================================\n");
	fprintf(f, ";  MetaProfiler profile  (generated automatically)\n");
	fprintf(f, ";  Profile key : %s\n", p.key.c_str());
	fprintf(f, ";  Saved       : %s\n", NowString().c_str());
	fprintf(f, ";  Do not edit while the server is running - changes get overwritten.\n");
	fprintf(f, "; ==============================================================\n\n");

	// Profile and Stats first, then everything else
	static const char* order[] = { "Profile", "Stats", nullptr };
	std::vector<bool> written(p.sections.size(), false);

	for (int o = 0; order[o]; o++)
	{
		for (size_t i = 0; i < p.sections.size(); i++)
		{
			if (!written[i] && IEq(p.sections[i].name, order[o]))
			{
				written[i] = true;
				fprintf(f, "[%s]\n", p.sections[i].name.c_str());
				for (size_t k = 0; k < p.sections[i].kv.size(); k++)
					fprintf(f, "%s=%s\n", p.sections[i].kv[k].key.c_str(), p.sections[i].kv[k].val.c_str());
				fputc('\n', f);
			}
		}
	}

	for (size_t i = 0; i < p.sections.size(); i++)
	{
		if (written[i]) continue;
		fprintf(f, "[%s]\n", p.sections[i].name.c_str());
		for (size_t k = 0; k < p.sections[i].kv.size(); k++)
			fprintf(f, "%s=%s\n", p.sections[i].kv[k].key.c_str(), p.sections[i].kv[k].val.c_str());
		fputc('\n', f);
	}

	for (int t = 0; t < MPL_COUNT; t++)
	{
		fprintf(f, "[%s]\n", MP_LOG_SECTION[t]);
		for (size_t i = 0; i < p.logs[t].size(); i++)
			fprintf(f, "%s%s\n", (t >= MPL_NAMES) ? "- " : "", p.logs[t][i].c_str());
		fputc('\n', f);
	}

	bool ok = (ferror(f) == 0);
	fclose(f);

	if (!ok)
	{
		remove(tmp.c_str());
		return false;
	}

	if (!ReplaceFile(tmp, path))
	{
		MP_OUT("[MetaProfiler] ERROR: cannot replace %s\n", path.c_str());
		return false;
	}

	p.dirty = false;
	g_Saves++;
	return true;
}

static void SaveAllIndexes();

static void SaveAll()
{
	for (size_t i = 0; i < g_Profiles.size(); i++)
		if (g_Profiles[i].used && g_Profiles[i].dirty)
			SaveProfile(g_Profiles[i]);

	SaveAllIndexes();
}

// ============================================================
//  Profile table / handles
// ============================================================

static unsigned int NextGen()
{
	g_GenCounter = (g_GenCounter + 1) & 0x7FFFFF;
	if (g_GenCounter == 0) g_GenCounter = 1;
	return g_GenCounter;
}

static cell MakeHandle(int slot)
{
	return (cell)((g_Profiles[slot].gen << 8) | (unsigned int)(slot + 1));
}

/* fires a forward with (player id, profile handle) */
static void FireProfileFwd(int fwd, int id, int slot)
{
	if (!g_FwdsReady || fwd < 0) return;
	MF_ExecuteForward(fwd, (cell)id, MakeHandle(slot));
}

static Profile* HandleToProfile(cell h)
{
	if (h <= 0) return nullptr;

	int slot = (int)(h & 0xFF) - 1;
	if (slot < 0 || slot >= MP_MAX_PROFILES) return nullptr;

	Profile& p = g_Profiles[slot];
	if (!p.used || (cell)p.gen != (h >> 8)) return nullptr;

	p.lastUse = ++g_UseCounter;
	return &p;
}

static int FindLoaded(const std::string& key)
{
	for (int i = 0; i < MP_MAX_PROFILES; i++)
		if (g_Profiles[i].used && g_Profiles[i].key == key) return i;
	return -1;
}

static int AcquireSlot()
{
	for (int i = 0; i < MP_MAX_PROFILES; i++)
		if (!g_Profiles[i].used) return i;

	// cache full: evict the least recently used OFFLINE profile
	int best = -1;
	for (int i = 0; i < MP_MAX_PROFILES; i++)
	{
		if (g_Profiles[i].online > 0) continue;
		if (best < 0 || g_Profiles[i].lastUse < g_Profiles[best].lastUse) best = i;
	}
	if (best < 0) return -1;

	if (g_Profiles[best].dirty) SaveProfile(g_Profiles[best]);
	g_Profiles[best] = Profile();
	return best;
}

static int LoadProfileSlot(const std::string& key, bool create, bool* created)
{
	if (created) *created = false;

	int s = FindLoaded(key);
	if (s >= 0) return s;

	std::string path = ProfilePath(key);
	bool exists = FileExists(path);

	// crash recovery: the .ini is missing but a finished .tmp is still there
	if (!exists && FileExists(path + ".tmp") && ReplaceFile(path + ".tmp", path))
		exists = true;
	if (!exists && !create) return -1;

	s = AcquireSlot();
	if (s < 0) return -1;

	Profile& p = g_Profiles[s];
	p = Profile();
	p.used    = true;
	p.gen     = NextGen();
	p.key     = key;
	p.lastUse = ++g_UseCounter;

	if (exists)
	{
		ParseFile(path, p);
		g_Loads++;
	}
	else
	{
		p.dirty = true;
		if (created) *created = true;
	}
	return s;
}

// ============================================================
//  Player lifecycle
// ============================================================

static std::string StripPort(const std::string& ip)
{
	size_t c = ip.find(':');
	return (c == std::string::npos) ? ip : ip.substr(0, c);
}

static cell DoDisconnect(int id)
{
	if (id < 1 || id > MP_MAX_PLAYERS) return 0;

	PlayerSlot& ps = g_Players[id];
	if (ps.slot < 0) return 0;

	Profile& p = g_Profiles[ps.slot];
	if (p.used)
	{
		long sess = (long)(time(nullptr) - ps.sessionStart);
		if (sess < 0) sess = 0;

		PAddInt(p, "Profile", "total_playtime", (int)sess);
		PSetInt(p, "Profile", "last_session", (int)sess);
		PSet(p, "Profile", "last_seen", NowString());
		PSetInt(p, "Profile", "last_seen_ts", (int)time(nullptr));

		char buf[96];
		snprintf(buf, sizeof(buf), "disconnected (session %ldm %lds)", sess / 60, sess % 60);
		AddLog(p, MPL_HISTORY, buf);

		// plugins can still write to the profile here
		FireProfileFwd(g_FwdClosing, id, ps.slot);

		if (p.online > 0) p.online--;
		SaveProfile(p);
	}

	ps = PlayerSlot();
	return 1;
}

static cell DoConnect(int id, const std::string& name, const std::string& authid, const std::string& ipRaw)
{
	if (id < 1 || id > MP_MAX_PLAYERS) return 0;

	if (g_Players[id].slot >= 0)
		DoDisconnect(id);

	std::string ip = StripPort(ipRaw);
	const std::string& ident = (g_KeyMode == MPK_AUTHID) ? authid : (g_KeyMode == MPK_IP ? ip : name);

	std::string key = MakeKey(ident);
	bool created = false;
	int s = LoadProfileSlot(key, true, &created);
	if (s < 0) return 0;

	Profile& p = g_Profiles[s];
	p.online++;

	PlayerSlot& ps = g_Players[id];
	ps.slot         = s;
	ps.sessionStart = time(nullptr);
	ps.name         = name;
	ps.authid       = authid;
	ps.ip           = ip;

	PSet(p, "Profile", "name", name);
	PSet(p, "Profile", "authid", authid);
	PSet(p, "Profile", "ip", ip);
	if (!PHas(p, "Profile", "first_seen"))
	{
		PSet(p, "Profile", "first_seen", NowString());
		PSetInt(p, "Profile", "first_seen_ts", (int)time(nullptr));
	}
	PSet(p, "Profile", "last_seen", NowString());
	PSetInt(p, "Profile", "last_seen_ts", (int)time(nullptr));
	PAddInt(p, "Profile", "connects", 1);

	AddUnique(p, MPL_NAMES, name);
	AddUnique(p, MPL_IPS, ip);

	char buf[256];
	snprintf(buf, sizeof(buf), "connected as '%s' (%s) from %s%s", name.c_str(), authid.c_str(), ip.c_str(), created ? " [NEW PROFILE]" : "");
	AddLog(p, MPL_HISTORY, buf);

	cell handle = MakeHandle(s);

	if (created) FireProfileFwd(g_FwdCreated, id, s);
	FireProfileFwd(g_FwdLoaded, id, s);

	return handle;
}

static cell DoRename(int id, const std::string& newName)
{
	if (id < 1 || id > MP_MAX_PLAYERS || g_Players[id].slot < 0) return 0;

	PlayerSlot ps = g_Players[id];		// copy: DoDisconnect resets it
	if (ps.name == newName)
		return MakeHandle(ps.slot);

	if (g_KeyMode == MPK_NAME)
	{
		// the profile follows the name -> switch profile
		cell oldHandle = MakeHandle(ps.slot);

		AddLog(g_Profiles[ps.slot], MPL_HISTORY, "changed name to '" + newName + "'");
		DoDisconnect(id);
		cell h = DoConnect(id, newName, ps.authid, ps.ip);
		Profile* np = HandleToProfile(h);
		if (np) AddLog(*np, MPL_HISTORY, "name changed from '" + ps.name + "'");

		if (g_FwdsReady && g_FwdRenamed >= 0)
			MF_ExecuteForward(g_FwdRenamed, (cell)id, oldHandle, h);

		return h;
	}

	Profile& p = g_Profiles[ps.slot];
	PSet(p, "Profile", "name", newName);
	AddUnique(p, MPL_NAMES, newName);
	AddLog(p, MPL_HISTORY, "changed name from '" + ps.name + "' to '" + newName + "'");
	g_Players[id].name = newName;
	return MakeHandle(ps.slot);
}

// ============================================================
//  Directory scan
// ============================================================

static int DoScan(const std::string& filter)
{
	g_Scan.clear();

	char dir[512];
	MF_BuildPathnameR(dir, sizeof(dir), "%s", MP_DATA_DIR);

#ifdef _WIN32
	std::string pattern = std::string(dir) + "\\*.ini";
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			std::string n = fd.cFileName;
			if (n.size() > 4) n.resize(n.size() - 4);
			if (filter.empty() || IContains(n, filter)) g_Scan.push_back(n);
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
#else
	DIR* d = opendir(dir);
	if (d)
	{
		struct dirent* e;
		while ((e = readdir(d)) != nullptr)
		{
			std::string n = e->d_name;
			if (n.size() > 4 && n.compare(n.size() - 4, 4, ".ini") == 0)
			{
				n.resize(n.size() - 4);
				if (filter.empty() || IContains(n, filter)) g_Scan.push_back(n);
			}
		}
		closedir(d);
	}
#endif

	std::sort(g_Scan.begin(), g_Scan.end());
	return (int)g_Scan.size();
}

// ============================================================
//  Printing
// ============================================================

static void PrintLine(char c, int n)
{
	char buf[100];
	if (n > 90) n = 90;
	for (int i = 0; i < n; i++) buf[i] = c;
	buf[n] = '\0';
	MP_OUT("%s\n", buf);
}

static void PrintProfile(Profile& p, int slot)
{
	MP_OUT("\n");
	PrintLine('=', 70);
	MP_OUT("  PROFILE: %s   (slot %d, %s, %s)\n", p.key.c_str(), slot, p.online ? "ONLINE" : "offline", p.dirty ? "unsaved changes" : "saved");
	PrintLine('=', 70);

	for (size_t i = 0; i < p.sections.size(); i++)
	{
		MP_OUT("  [%s]\n", p.sections[i].name.c_str());
		for (size_t k = 0; k < p.sections[i].kv.size(); k++)
			MP_OUT("    %-22s = %s\n", p.sections[i].kv[k].key.c_str(), p.sections[i].kv[k].val.c_str());
	}

	for (int t = 0; t < MPL_COUNT; t++)
	{
		std::vector<std::string>& v = p.logs[t];
		MP_OUT("  [%s]  (%d entries)\n", MP_LOG_SECTION[t], (int)v.size());

		size_t from = (v.size() > 6) ? v.size() - 6 : 0;
		for (size_t i = from; i < v.size(); i++)
			MP_OUT("    %s\n", v[i].c_str());
	}
	PrintLine('=', 70);
	MP_OUT("\n");
}

// ============================================================
//  Indexes: independent key / column tables (no link to profiles)
//  addons/amxmodx/data/metaprofiler/indexes/<name>.ini
// ============================================================

#define MP_MAX_INDEXES   16
#define MP_IDX_KEYLEN    64
#define MP_IDX_DIR       MP_DATA_DIR "/indexes"

struct IdxRow
{
	std::string     key;
	std::vector<KV> cols;
};

struct IdxSorted
{
	std::string      id;       // column (lower case) + '+' (ascending) or '-' (descending)
	std::vector<int> order;    // row numbers, best first
};

struct Index
{
	bool         used;
	unsigned int gen;
	std::string  name;
	bool         dirty;

	std::vector<IdxRow>                   rows;
	std::unordered_map<std::string, int>  map;      // lower case key -> row number
	std::vector<IdxSorted>                cache;    // sorted views, cleared on every change

	Index() : used(false), gen(0), dirty(false) {}
};

static std::vector<Index> g_Indexes;
static unsigned int       g_IdxGen = 0;

static unsigned int NextIdxGen()
{
	g_IdxGen = (g_IdxGen + 1) & 0x7FFFFF;
	if (g_IdxGen == 0) g_IdxGen = 1;
	return g_IdxGen;
}

static std::string IdxPath(const std::string& name)
{
	char buf[640];
	MF_BuildPathnameR(buf, sizeof(buf), "%s/%s.ini", MP_IDX_DIR, name.c_str());
	return buf;
}

static void EnsureIdxDir()
{
	EnsureDir();
	char dir[512];
	MF_BuildPathnameR(dir, sizeof(dir), "%s", MP_IDX_DIR);
	MP_MKDIR(dir);
}

/* row key: same cleaning as section names, max 64 characters */
static std::string IdxKey(const std::string& raw)
{
	std::string k = CleanToken(raw);
	if (k.size() > MP_IDX_KEYLEN) k.resize(MP_IDX_KEYLEN);
	return Trimmed(k);
}

static cell IdxHandle(int slot)
{
	return (cell)((g_Indexes[slot].gen << 8) | (unsigned int)(slot + 1));
}

static Index* IdxFromHandle(cell h)
{
	if (h <= 0) return nullptr;

	int slot = (int)(h & 0xFF) - 1;
	if (slot < 0 || slot >= MP_MAX_INDEXES) return nullptr;

	Index& ix = g_Indexes[slot];
	if (!ix.used || (cell)ix.gen != (h >> 8)) return nullptr;
	return &ix;
}

static int IdxFindRow(Index& ix, const std::string& key)
{
	std::unordered_map<std::string, int>::iterator it = ix.map.find(Lower(key));
	return (it == ix.map.end()) ? -1 : it->second;
}

static int IdxAddRow(Index& ix, const std::string& key)
{
	IdxRow r;
	r.key = key;
	ix.rows.push_back(r);

	int n = (int)ix.rows.size() - 1;
	ix.map[Lower(key)] = n;
	return n;
}

static void IdxRemoveRow(Index& ix, int n)
{
	ix.map.erase(Lower(ix.rows[n].key));

	int last = (int)ix.rows.size() - 1;
	if (n != last)
	{
		ix.rows[n] = ix.rows[last];
		ix.map[Lower(ix.rows[n].key)] = n;
	}
	ix.rows.pop_back();

	ix.cache.clear();
	ix.dirty = true;
}

static bool IdxSet(Index& ix, const std::string& keyRaw, const std::string& colRaw, const std::string& val)
{
	std::string key = IdxKey(keyRaw), col = CleanToken(colRaw);
	if (key.empty() || col.empty())
		return false;

	int n = IdxFindRow(ix, key);
	if (n < 0) n = IdxAddRow(ix, key);

	std::string v = CleanValue(val);
	std::vector<KV>& cols = ix.rows[n].cols;

	for (size_t i = 0; i < cols.size(); i++)
	{
		if (IEq(cols[i].key, col))
		{
			if (cols[i].val == v) return true;
			cols[i].val = v;
			ix.dirty = true;
			ix.cache.clear();
			return true;
		}
	}

	KV kv;
	kv.key = col;
	kv.val = v;
	cols.push_back(kv);

	ix.dirty = true;
	ix.cache.clear();
	return true;
}

static bool IdxGet(Index& ix, const std::string& keyRaw, const std::string& colRaw, std::string& out)
{
	int n = IdxFindRow(ix, IdxKey(keyRaw));
	if (n < 0) return false;

	std::string col = CleanToken(colRaw);
	std::vector<KV>& cols = ix.rows[n].cols;

	for (size_t i = 0; i < cols.size(); i++)
	{
		if (IEq(cols[i].key, col))
		{
			out = cols[i].val;
			return true;
		}
	}
	return false;
}

/* numeric value of a column (ok = false if the row has no such column or it is empty) */
static double IdxNum(IdxRow& r, const std::string& col, bool& ok)
{
	ok = false;
	for (size_t i = 0; i < r.cols.size(); i++)
	{
		if (IEq(r.cols[i].key, col))
		{
			if (r.cols[i].val.empty()) return 0.0;
			ok = true;
			return strtod(r.cols[i].val.c_str(), nullptr);
		}
	}
	return 0.0;
}

/* rows that have the column, best first (ties are ordered by key). Cached until the next change. */
static const std::vector<int>& IdxGetSorted(Index& ix, const std::string& colIn, bool asc)
{
	std::string col = CleanToken(colIn);
	std::string id  = Lower(col) + (asc ? "+" : "-");

	for (size_t i = 0; i < ix.cache.size(); i++)
		if (ix.cache[i].id == id) return ix.cache[i].order;

	if (ix.cache.size() > 8)
		ix.cache.clear();

	std::vector<std::pair<double, int> > tmp;
	for (size_t i = 0; i < ix.rows.size(); i++)
	{
		bool ok = false;
		double v = IdxNum(ix.rows[i], col, ok);
		if (ok) tmp.push_back(std::make_pair(v, (int)i));
	}

	std::sort(tmp.begin(), tmp.end(), [&ix, asc](const std::pair<double, int>& a, const std::pair<double, int>& b)
	{
		if (a.first != b.first)
			return asc ? (a.first < b.first) : (a.first > b.first);
		return Lower(ix.rows[a.second].key) < Lower(ix.rows[b.second].key);
	});

	IdxSorted s;
	s.id = id;
	for (size_t i = 0; i < tmp.size(); i++)
		s.order.push_back(tmp[i].second);

	ix.cache.push_back(s);
	return ix.cache.back().order;
}

static bool IdxParse(const std::string& path, Index& ix)
{
	FILE* f = fopen(path.c_str(), "r");
	if (!f) return false;

	char line[2048];
	int cur = -1;

	while (fgets(line, sizeof(line), f))
	{
		std::string s = Trimmed(line);
		if (s.empty() || s[0] == ';' || s[0] == '#')
			continue;

		if (s[0] == '[' && s[s.size() - 1] == ']')
		{
			std::string key = IdxKey(s.substr(1, s.size() - 2));
			if (key.empty()) { cur = -1; continue; }

			cur = IdxFindRow(ix, key);
			if (cur < 0) cur = IdxAddRow(ix, key);
			continue;
		}

		if (cur < 0) continue;

		size_t eq = s.find('=');
		if (eq == std::string::npos) continue;

		std::string col = CleanToken(s.substr(0, eq));
		std::string val = Trimmed(s.substr(eq + 1));
		if (col.empty()) continue;

		std::vector<KV>& cols = ix.rows[cur].cols;
		bool found = false;
		for (size_t i = 0; i < cols.size(); i++)
		{
			if (IEq(cols[i].key, col)) { cols[i].val = val; found = true; break; }
		}
		if (!found)
		{
			KV kv; kv.key = col; kv.val = val;
			cols.push_back(kv);
		}
	}
	fclose(f);
	return true;
}

static bool IdxSave(Index& ix)
{
	EnsureIdxDir();

	std::string path = IdxPath(ix.name);
	std::string tmp  = path + ".tmp";

	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
	{
		MP_OUT("[MetaProfiler] ERROR: cannot write %s\n", tmp.c_str());
		return false;
	}

	fprintf(f, "; MetaProfiler index: %s\n", ix.name.c_str());
	fprintf(f, "; Rows  : %d\n", (int)ix.rows.size());
	fprintf(f, "; Saved : %s\n\n", NowString().c_str());

	for (size_t i = 0; i < ix.rows.size(); i++)
	{
		fprintf(f, "[%s]\n", ix.rows[i].key.c_str());
		for (size_t k = 0; k < ix.rows[i].cols.size(); k++)
			fprintf(f, "%s=%s\n", ix.rows[i].cols[k].key.c_str(), ix.rows[i].cols[k].val.c_str());
		fputc('\n', f);
	}

	bool ok = (ferror(f) == 0);
	fclose(f);

	if (!ok)
	{
		remove(tmp.c_str());
		return false;
	}

	if (!ReplaceFile(tmp, path))
	{
		MP_OUT("[MetaProfiler] ERROR: cannot replace %s\n", path.c_str());
		return false;
	}

	ix.dirty = false;
	g_Saves++;
	return true;
}

static void SaveAllIndexes()
{
	for (size_t i = 0; i < g_Indexes.size(); i++)
		if (g_Indexes[i].used && g_Indexes[i].dirty)
			IdxSave(g_Indexes[i]);
}

static int IdxOpen(const std::string& nameRaw, bool create)
{
	std::string name = MakeKey(nameRaw);

	for (int i = 0; i < MP_MAX_INDEXES; i++)
		if (g_Indexes[i].used && g_Indexes[i].name == name) return i;

	std::string path = IdxPath(name);
	bool exists = FileExists(path);

	// crash recovery
	if (!exists && FileExists(path + ".tmp") && ReplaceFile(path + ".tmp", path))
		exists = true;

	if (!exists && !create)
		return -1;

	int s = -1;
	for (int i = 0; i < MP_MAX_INDEXES; i++)
		if (!g_Indexes[i].used) { s = i; break; }
	if (s < 0) return -1;

	Index& ix = g_Indexes[s];
	ix = Index();
	ix.used = true;
	ix.gen  = NextIdxGen();
	ix.name = name;

	if (exists)
	{
		IdxParse(path, ix);
		g_Loads++;
	}
	else
	{
		ix.dirty = true;
	}
	return s;
}

// ============================================================
//  Natives
// ============================================================

#define MP_NATIVE(fn)  static cell AMX_NATIVE_CALL fn(AMX* amx, cell* params)
#define MP_GETP(var, idx)  Profile* var = HandleToProfile(params[idx]); if (!var) return 0;

static inline int MP_ParamCount(cell* params) { return (int)(params[0] / sizeof(cell)); }

// ---- lifecycle ------------------------------------------------

// MPProfile:metaprofiler_player_connect(id, const name[], const authid[], const ip[])
MP_NATIVE(n_player_connect)
{
	int len;
	std::string name   = MF_GetAmxString(amx, params[2], 0, &len);
	std::string authid = MF_GetAmxString(amx, params[3], 1, &len);
	std::string ip     = MF_GetAmxString(amx, params[4], 2, &len);
	return DoConnect(params[1], name, authid, ip);
}

MP_NATIVE(n_player_disconnect) { return DoDisconnect(params[1]); }

MP_NATIVE(n_player_rename) { return DoRename(params[1], Str(amx, params[2])); }

MP_NATIVE(n_get_handle)
{
	int id = params[1];
	if (id < 1 || id > MP_MAX_PLAYERS || g_Players[id].slot < 0) return 0;
	return MakeHandle(g_Players[id].slot);
}

MP_NATIVE(n_load)
{
	std::string key = MakeKey(Str(amx, params[1]));
	bool create = (MP_ParamCount(params) >= 2) && params[2] != 0;
	int s = LoadProfileSlot(key, create, nullptr);
	return (s >= 0) ? MakeHandle(s) : 0;
}

MP_NATIVE(n_exists)
{
	std::string key = MakeKey(Str(amx, params[1]));
	return (FindLoaded(key) >= 0 || FileExists(ProfilePath(key))) ? 1 : 0;
}

MP_NATIVE(n_delete)
{
	std::string key = MakeKey(Str(amx, params[1]));

	int s = FindLoaded(key);
	if (s >= 0)
	{
		if (g_Profiles[s].online > 0) return 0;		// never delete a connected player
		g_Profiles[s] = Profile();
	}
	return remove(ProfilePath(key).c_str()) == 0 ? 1 : 0;
}

MP_NATIVE(n_is_valid)  { return HandleToProfile(params[1]) ? 1 : 0; }

MP_NATIVE(n_is_online)
{
	MP_GETP(p, 1);
	return p->online > 0 ? 1 : 0;
}

MP_NATIVE(n_get_key)
{
	MP_GETP(p, 1);
	return MF_SetAmxString(amx, params[2], p->key.c_str(), params[3]);
}

// ---- key/value ------------------------------------------------

// metaprofiler_set(h, section, key, value)
MP_NATIVE(n_set)
{
	MP_GETP(p, 1);
	int len;
	std::string sec = MF_GetAmxString(amx, params[2], 0, &len);
	std::string key = MF_GetAmxString(amx, params[3], 1, &len);
	std::string val = MF_GetAmxString(amx, params[4], 2, &len);
	return PSet(*p, sec, key, val) ? 1 : 0;
}

// metaprofiler_get(h, section, key, out[], len, def[] = "")
MP_NATIVE(n_get)
{
	MP_GETP(p, 1);
	int len;
	std::string sec = MF_GetAmxString(amx, params[2], 0, &len);
	std::string key = MF_GetAmxString(amx, params[3], 1, &len);

	std::string out;
	bool found = PGet(*p, sec, key, out);
	if (!found)
		out = (MP_ParamCount(params) >= 6) ? Str(amx, params[6]) : "";

	MF_SetAmxString(amx, params[4], out.c_str(), params[5]);
	return found ? 1 : 0;
}

MP_NATIVE(n_set_int)
{
	MP_GETP(p, 1);
	int len;
	std::string sec = MF_GetAmxString(amx, params[2], 0, &len);
	std::string key = MF_GetAmxString(amx, params[3], 1, &len);
	return PSetInt(*p, sec, key, params[4]) ? 1 : 0;
}

MP_NATIVE(n_get_int)
{
	MP_GETP(p, 1);
	int len;
	std::string sec = MF_GetAmxString(amx, params[2], 0, &len);
	std::string key = MF_GetAmxString(amx, params[3], 1, &len);
	return PGetInt(*p, sec, key, (MP_ParamCount(params) >= 4) ? params[4] : 0);
}

MP_NATIVE(n_add_int)
{
	MP_GETP(p, 1);
	int len;
	std::string sec = MF_GetAmxString(amx, params[2], 0, &len);
	std::string key = MF_GetAmxString(amx, params[3], 1, &len);
	return PAddInt(*p, sec, key, (MP_ParamCount(params) >= 4) ? params[4] : 1);
}

MP_NATIVE(n_set_float)
{
	MP_GETP(p, 1);
	int len;
	std::string sec = MF_GetAmxString(amx, params[2], 0, &len);
	std::string key = MF_GetAmxString(amx, params[3], 1, &len);
	char buf[48];
	snprintf(buf, sizeof(buf), "%.4f", (double)amx_ctof(params[4]));
	return PSet(*p, sec, key, buf) ? 1 : 0;
}

MP_NATIVE(n_get_float)
{
	MP_GETP(p, 1);
	int len;
	std::string sec = MF_GetAmxString(amx, params[2], 0, &len);
	std::string key = MF_GetAmxString(amx, params[3], 1, &len);
	double def = (MP_ParamCount(params) >= 4) ? (double)amx_ctof(params[4]) : 0.0;
	return amx_ftoc((float)PGetFloat(*p, sec, key, def));
}

MP_NATIVE(n_has)
{
	MP_GETP(p, 1);
	int len;
	std::string sec = MF_GetAmxString(amx, params[2], 0, &len);
	std::string key = MF_GetAmxString(amx, params[3], 1, &len);
	return PHas(*p, sec, key) ? 1 : 0;
}

MP_NATIVE(n_remove)
{
	MP_GETP(p, 1);
	int len;
	std::string sec = MF_GetAmxString(amx, params[2], 0, &len);
	std::string key = MF_GetAmxString(amx, params[3], 1, &len);
	return PRemove(*p, sec, key) ? 1 : 0;
}

MP_NATIVE(n_key_count)
{
	MP_GETP(p, 1);
	Section* s = FindSection(*p, CleanToken(Str(amx, params[2])), false);
	return s ? (cell)s->kv.size() : 0;
}

// metaprofiler_key_at(h, section, index, out[], len)
MP_NATIVE(n_key_at)
{
	MP_GETP(p, 1);
	Section* s = FindSection(*p, CleanToken(Str(amx, params[2])), false);
	int idx = params[3];
	if (!s || idx < 0 || idx >= (int)s->kv.size()) return 0;
	MF_SetAmxString(amx, params[4], s->kv[idx].key.c_str(), params[5]);
	return 1;
}

MP_NATIVE(n_section_count)
{
	MP_GETP(p, 1);
	return (cell)p->sections.size();
}

MP_NATIVE(n_section_at)
{
	MP_GETP(p, 1);
	int idx = params[2];
	if (idx < 0 || idx >= (int)p->sections.size()) return 0;
	MF_SetAmxString(amx, params[3], p->sections[idx].name.c_str(), params[4]);
	return 1;
}

// ---- compare / check -----------------------------------------

// metaprofiler_compare_int(a, b, section, key) -> -1 / 0 / 1
MP_NATIVE(n_compare_int)
{
	Profile* a = HandleToProfile(params[1]);
	Profile* b = HandleToProfile(params[2]);
	if (!a || !b) return 0;
	int len;
	std::string sec = MF_GetAmxString(amx, params[3], 0, &len);
	std::string key = MF_GetAmxString(amx, params[4], 1, &len);
	int va = PGetInt(*a, sec, key, 0), vb = PGetInt(*b, sec, key, 0);
	return va < vb ? -1 : (va > vb ? 1 : 0);
}

MP_NATIVE(n_compare_float)
{
	Profile* a = HandleToProfile(params[1]);
	Profile* b = HandleToProfile(params[2]);
	if (!a || !b) return 0;
	int len;
	std::string sec = MF_GetAmxString(amx, params[3], 0, &len);
	std::string key = MF_GetAmxString(amx, params[4], 1, &len);
	double va = PGetFloat(*a, sec, key, 0.0), vb = PGetFloat(*b, sec, key, 0.0);
	return va < vb ? -1 : (va > vb ? 1 : 0);
}

// metaprofiler_compare_str(a, b, section, key, bool:ignorecase = true)
MP_NATIVE(n_compare_str)
{
	Profile* a = HandleToProfile(params[1]);
	Profile* b = HandleToProfile(params[2]);
	if (!a || !b) return 0;
	int len;
	std::string sec = MF_GetAmxString(amx, params[3], 0, &len);
	std::string key = MF_GetAmxString(amx, params[4], 1, &len);
	bool icase = (MP_ParamCount(params) >= 5) ? (params[5] != 0) : true;

	std::string va, vb;
	PGet(*a, sec, key, va);
	PGet(*b, sec, key, vb);

	if (icase) return ICmp(va, vb);
	int r = va.compare(vb);
	return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

// metaprofiler_check(h, section, key, MPOp:op, value)
MP_NATIVE(n_check)
{
	MP_GETP(p, 1);
	int len;
	std::string sec = MF_GetAmxString(amx, params[2], 0, &len);
	std::string key = MF_GetAmxString(amx, params[3], 1, &len);
	int lhs = PGetInt(*p, sec, key, 0), rhs = params[5];

	switch (params[4])
	{
		case MPO_EQ: return lhs == rhs;
		case MPO_NE: return lhs != rhs;
		case MPO_GT: return lhs >  rhs;
		case MPO_GE: return lhs >= rhs;
		case MPO_LT: return lhs <  rhs;
		case MPO_LE: return lhs <= rhs;
	}
	return 0;
}

// metaprofiler_equals(h, section, key, const value[], bool:ignorecase = true)
MP_NATIVE(n_equals)
{
	MP_GETP(p, 1);
	int len;
	std::string sec = MF_GetAmxString(amx, params[2], 0, &len);
	std::string key = MF_GetAmxString(amx, params[3], 1, &len);
	std::string val = MF_GetAmxString(amx, params[4], 2, &len);
	bool icase = (MP_ParamCount(params) >= 5) ? (params[5] != 0) : true;

	std::string cur;
	if (!PGet(*p, sec, key, cur)) return 0;
	return (icase ? IEq(cur, val) : cur == val) ? 1 : 0;
}

// metaprofiler_diff(a, b, section) -> number of keys that differ / are missing on one side
MP_NATIVE(n_diff)
{
	Profile* a = HandleToProfile(params[1]);
	Profile* b = HandleToProfile(params[2]);
	if (!a || !b) return 0;

	std::string sec = CleanToken(Str(amx, params[3]));
	Section* sa = FindSection(*a, sec, false);
	Section* sb = FindSection(*b, sec, false);

	int diff = 0;
	if (sa)
	{
		for (size_t i = 0; i < sa->kv.size(); i++)
		{
			KV* o = sb ? FindKV(*sb, sa->kv[i].key) : nullptr;
			if (!o || o->val != sa->kv[i].val) diff++;
		}
	}
	if (sb)
	{
		for (size_t i = 0; i < sb->kv.size(); i++)
		{
			KV* o = sa ? FindKV(*sa, sb->kv[i].key) : nullptr;
			if (!o) diff++;
		}
	}
	return diff;
}

// metaprofiler_copy(src, dst, section, bool:overwrite = true) -> keys copied
MP_NATIVE(n_copy)
{
	Profile* src = HandleToProfile(params[1]);
	Profile* dst = HandleToProfile(params[2]);
	if (!src || !dst || src == dst) return 0;

	std::string sec = CleanToken(Str(amx, params[3]));
	bool overwrite = (MP_ParamCount(params) >= 4) ? (params[4] != 0) : true;

	Section* s = FindSection(*src, sec, false);
	if (!s) return 0;

	std::vector<KV> copy = s->kv;		// copy first: PSet may reallocate
	int n = 0;
	for (size_t i = 0; i < copy.size(); i++)
	{
		if (!overwrite && PHas(*dst, sec, copy[i].key)) continue;
		if (PSet(*dst, sec, copy[i].key, copy[i].val)) n++;
	}
	return n;
}

// ---- logs -----------------------------------------------------

static bool ValidType(int t, bool write)
{
	return t >= 0 && t < (write ? MPL_NAMES : MPL_COUNT);
}

// metaprofiler_log(h, MPLog:type, const fmt[], any:...)
MP_NATIVE(n_log)
{
	MP_GETP(p, 1);
	if (!ValidType(params[2], true)) return 0;

	int len;
	char* msg = MF_FormatAmxString(amx, params, 3, &len);
	AddLog(*p, params[2], msg ? msg : "");
	return 1;
}

MP_NATIVE(n_log_count)
{
	MP_GETP(p, 1);
	if (!ValidType(params[2], false)) return 0;
	return (cell)p->logs[params[2]].size();
}

// metaprofiler_log_get(h, type, index, out[], len)   index 0 = newest
MP_NATIVE(n_log_get)
{
	MP_GETP(p, 1);
	if (!ValidType(params[2], false)) return 0;

	std::vector<std::string>& v = p->logs[params[2]];
	int idx = params[3];
	if (idx < 0 || idx >= (int)v.size()) return 0;

	MF_SetAmxString(amx, params[4], v[v.size() - 1 - idx].c_str(), params[5]);
	return 1;
}

MP_NATIVE(n_log_clear)
{
	MP_GETP(p, 1);
	if (!ValidType(params[2], true)) return 0;
	p->logs[params[2]].clear();
	p->dirty = true;
	return 1;
}

// metaprofiler_log_search(h, type, const text[]) -> number of matching lines
MP_NATIVE(n_log_search)
{
	MP_GETP(p, 1);
	if (!ValidType(params[2], false)) return 0;

	std::string text = Str(amx, params[3]);
	std::vector<std::string>& v = p->logs[params[2]];

	int n = 0;
	for (size_t i = 0; i < v.size(); i++)
		if (IContains(v[i], text)) n++;
	return n;
}

// metaprofiler_set_log_limit(MPLog:type, limit)
MP_NATIVE(n_set_log_limit)
{
	if (!ValidType(params[1], false)) return 0;
	int lim = params[2];
	if (lim < 5) lim = 5;
	if (lim > 5000) lim = 5000;
	g_LogLimit[params[1]] = lim;
	return 1;
}

// ---- persistence / config ---------------------------------------

MP_NATIVE(n_save)
{
	MP_GETP(p, 1);
	return SaveProfile(*p) ? 1 : 0;
}

MP_NATIVE(n_save_all)
{
	SaveAll();
	return 1;
}

MP_NATIVE(n_reload)
{
	MP_GETP(p, 1);

	std::string key = p->key;
	std::string path = ProfilePath(key);
	if (!FileExists(path)) return 0;

	// rebuild from disk but keep the slot, generation and online state
	Profile fresh;
	fresh.used = true; fresh.gen = p->gen; fresh.key = key;
	fresh.online = p->online; fresh.lastUse = p->lastUse;
	if (!ParseFile(path, fresh)) return 0;

	*p = fresh;
	return 1;
}

MP_NATIVE(n_is_dirty)
{
	MP_GETP(p, 1);
	return p->dirty ? 1 : 0;
}

MP_NATIVE(n_set_keymode)
{
	if (params[1] < MPK_NAME || params[1] > MPK_IP) return 0;
	g_KeyMode = params[1];
	return 1;
}

MP_NATIVE(n_get_keymode) { return g_KeyMode; }

// metaprofiler_scan(const filter[] = "") -> count
MP_NATIVE(n_scan)
{
	std::string filter = (MP_ParamCount(params) >= 1) ? Str(amx, params[1]) : "";
	return DoScan(filter);
}

// metaprofiler_scan_get(index, out[], len)
MP_NATIVE(n_scan_get)
{
	int idx = params[1];
	if (idx < 0 || idx >= (int)g_Scan.size()) return 0;
	MF_SetAmxString(amx, params[2], g_Scan[idx].c_str(), params[3]);
	return 1;
}

MP_NATIVE(n_dump)
{
	MP_GETP(p, 1);
	PrintProfile(*p, (int)(p - &g_Profiles[0]));
	return 1;
}

MP_NATIVE(n_diag)
{
	static const char* modes[] = { "NAME", "AUTHID", "IP" };

	MP_OUT("\n");
	PrintLine('=', 78);
	MP_OUT("                 METAPROFILER v%s - DIAGNOSTIC REPORT\n", MP_VERSION);
	PrintLine('=', 78);

	char dir[512];
	MF_BuildPathnameR(dir, sizeof(dir), "%s", MP_DATA_DIR);

	MP_OUT("\n  [MODULE]\n");
	MP_OUT("  Key mode         : %s\n", modes[g_KeyMode]);
	MP_OUT("  Data directory   : %s\n", dir);
	MP_OUT("  Log limits       : history %d | chat %d | gameplay %d | names %d | ips %d\n",
		g_LogLimit[0], g_LogLimit[1], g_LogLimit[2], g_LogLimit[3], g_LogLimit[4]);
	MP_OUT("  Files saved/read : %u / %u\n", g_Saves, g_Loads);
	MP_OUT("  Forwards         : %s\n", g_FwdsReady ? "ENABLED" : "DISABLED (enable FN_AMXX_PLUGINSLOADED in moduleconfig.h)");

	int loaded = 0;
	for (int i = 0; i < MP_MAX_PROFILES; i++) if (g_Profiles[i].used) loaded++;
	MP_OUT("  Cached profiles  : %d / %d\n", loaded, MP_MAX_PROFILES);
	MP_OUT("  Profiles on disk : %d\n", DoScan(""));

	MP_OUT("\n  [LOADED PROFILES]\n");
	MP_OUT("  %-28s %-8s %-8s %-9s %-6s\n", "Key", "State", "Saved", "Sections", "Lines");
	PrintLine('-', 78);

	for (int i = 0; i < MP_MAX_PROFILES; i++)
	{
		Profile& p = g_Profiles[i];
		if (!p.used) continue;

		int lines = 0;
		for (int t = 0; t < MPL_COUNT; t++) lines += (int)p.logs[t].size();

		MP_OUT("  %-28s %-8s %-8s %-9d %-6d\n", p.key.c_str(), p.online ? "ONLINE" : "offline",
			p.dirty ? "NO" : "yes", (int)p.sections.size(), lines);
	}

	MP_OUT("\n  [INDEXES]\n");
	MP_OUT("  %-28s %-8s %-8s\n", "Name", "Rows", "Saved");
	PrintLine('-', 78);

	int idxCount = 0;
	for (int i = 0; i < MP_MAX_INDEXES; i++)
	{
		if (!g_Indexes[i].used) continue;
		MP_OUT("  %-28s %-8d %-8s\n", g_Indexes[i].name.c_str(), (int)g_Indexes[i].rows.size(), g_Indexes[i].dirty ? "NO" : "yes");
		idxCount++;
	}
	if (!idxCount)
		MP_OUT("  (none)\n");

	PrintLine('=', 78);
	MP_OUT("\n");
	return 1;
}

// ============================================================
//  Natives: indexes
// ============================================================

#define MP_GETI(var, idx)  Index* var = IdxFromHandle(params[idx]); if (!var) return 0;

// MPIndex:metaprofiler_index_open(const name[], bool:create = true)
MP_NATIVE(n_idx_open)
{
	bool create = (MP_ParamCount(params) >= 2) ? (params[2] != 0) : true;
	int s = IdxOpen(Str(amx, params[1]), create);
	return (s >= 0) ? IdxHandle(s) : 0;
}

MP_NATIVE(n_idx_exists)
{
	std::string name = MakeKey(Str(amx, params[1]));

	for (int i = 0; i < MP_MAX_INDEXES; i++)
		if (g_Indexes[i].used && g_Indexes[i].name == name) return 1;

	return FileExists(IdxPath(name)) ? 1 : 0;
}

MP_NATIVE(n_idx_is_valid) { return IdxFromHandle(params[1]) ? 1 : 0; }

// metaprofiler_index_delete(const name[]) : removes the file and unloads the index
MP_NATIVE(n_idx_delete)
{
	std::string name = MakeKey(Str(amx, params[1]));

	for (int i = 0; i < MP_MAX_INDEXES; i++)
		if (g_Indexes[i].used && g_Indexes[i].name == name) g_Indexes[i] = Index();

	return remove(IdxPath(name).c_str()) == 0 ? 1 : 0;
}

MP_NATIVE(n_idx_clear)
{
	MP_GETI(ix, 1);
	ix->rows.clear();
	ix->map.clear();
	ix->cache.clear();
	ix->dirty = true;
	return 1;
}

MP_NATIVE(n_idx_save)
{
	MP_GETI(ix, 1);
	return IdxSave(*ix) ? 1 : 0;
}

MP_NATIVE(n_idx_count)
{
	MP_GETI(ix, 1);
	return (cell)ix->rows.size();
}

// metaprofiler_index_key_at(index, position, out[], len)
MP_NATIVE(n_idx_key_at)
{
	MP_GETI(ix, 1);
	int pos = params[2];
	if (pos < 0 || pos >= (int)ix->rows.size()) return 0;
	MF_SetAmxString(amx, params[3], ix->rows[pos].key.c_str(), params[4]);
	return 1;
}

MP_NATIVE(n_idx_has)
{
	MP_GETI(ix, 1);
	return IdxFindRow(*ix, IdxKey(Str(amx, params[2]))) >= 0 ? 1 : 0;
}

MP_NATIVE(n_idx_remove)
{
	MP_GETI(ix, 1);
	int n = IdxFindRow(*ix, IdxKey(Str(amx, params[2])));
	if (n < 0) return 0;
	IdxRemoveRow(*ix, n);
	return 1;
}

// metaprofiler_index_set(index, key, column, value)
MP_NATIVE(n_idx_set)
{
	MP_GETI(ix, 1);
	int len;
	std::string key = MF_GetAmxString(amx, params[2], 0, &len);
	std::string col = MF_GetAmxString(amx, params[3], 1, &len);
	std::string val = MF_GetAmxString(amx, params[4], 2, &len);
	return IdxSet(*ix, key, col, val) ? 1 : 0;
}

// metaprofiler_index_get(index, key, column, out[], len, def[] = "")
MP_NATIVE(n_idx_get)
{
	MP_GETI(ix, 1);
	int len;
	std::string key = MF_GetAmxString(amx, params[2], 0, &len);
	std::string col = MF_GetAmxString(amx, params[3], 1, &len);

	std::string out;
	bool found = IdxGet(*ix, key, col, out);
	if (!found)
		out = (MP_ParamCount(params) >= 6) ? Str(amx, params[6]) : "";

	MF_SetAmxString(amx, params[4], out.c_str(), params[5]);
	return found ? 1 : 0;
}

static int IdxGetInt(Index& ix, const std::string& key, const std::string& col, int def)
{
	std::string v;
	if (!IdxGet(ix, key, col, v) || v.empty()) return def;
	return (int)strtol(v.c_str(), nullptr, 10);
}

MP_NATIVE(n_idx_set_int)
{
	MP_GETI(ix, 1);
	int len;
	std::string key = MF_GetAmxString(amx, params[2], 0, &len);
	std::string col = MF_GetAmxString(amx, params[3], 1, &len);
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", (int)params[4]);
	return IdxSet(*ix, key, col, buf) ? 1 : 0;
}

MP_NATIVE(n_idx_get_int)
{
	MP_GETI(ix, 1);
	int len;
	std::string key = MF_GetAmxString(amx, params[2], 0, &len);
	std::string col = MF_GetAmxString(amx, params[3], 1, &len);
	return IdxGetInt(*ix, key, col, (MP_ParamCount(params) >= 4) ? params[4] : 0);
}

MP_NATIVE(n_idx_add_int)
{
	MP_GETI(ix, 1);
	int len;
	std::string key = MF_GetAmxString(amx, params[2], 0, &len);
	std::string col = MF_GetAmxString(amx, params[3], 1, &len);
	int amount = (MP_ParamCount(params) >= 4) ? params[4] : 1;

	int v = IdxGetInt(*ix, key, col, 0) + amount;
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", v);
	IdxSet(*ix, key, col, buf);
	return v;
}

MP_NATIVE(n_idx_set_float)
{
	MP_GETI(ix, 1);
	int len;
	std::string key = MF_GetAmxString(amx, params[2], 0, &len);
	std::string col = MF_GetAmxString(amx, params[3], 1, &len);
	char buf[48];
	snprintf(buf, sizeof(buf), "%.4f", (double)amx_ctof(params[4]));
	return IdxSet(*ix, key, col, buf) ? 1 : 0;
}

MP_NATIVE(n_idx_get_float)
{
	MP_GETI(ix, 1);
	int len;
	std::string key = MF_GetAmxString(amx, params[2], 0, &len);
	std::string col = MF_GetAmxString(amx, params[3], 1, &len);
	double def = (MP_ParamCount(params) >= 4) ? (double)amx_ctof(params[4]) : 0.0;

	std::string v;
	if (!IdxGet(*ix, key, col, v) || v.empty())
		return amx_ftoc((float)def);
	return amx_ftoc((float)strtod(v.c_str(), nullptr));
}

// metaprofiler_index_top(index, column, position, key[], keylen, &value, bool:ascending = false)
MP_NATIVE(n_idx_top)
{
	MP_GETI(ix, 1);
	std::string col = Str(amx, params[2]);
	int pos = params[3];
	bool asc = (MP_ParamCount(params) >= 7) ? (params[7] != 0) : false;

	const std::vector<int>& order = IdxGetSorted(*ix, col, asc);
	if (pos < 0 || pos >= (int)order.size()) return 0;

	IdxRow& r = ix->rows[order[pos]];
	MF_SetAmxString(amx, params[4], r.key.c_str(), params[5]);

	cell* addr = MF_GetAmxAddr(amx, params[6]);
	if (addr)
	{
		bool ok = false;
		*addr = (cell)IdxNum(r, CleanToken(col), ok);
	}
	return 1;
}

// metaprofiler_index_rank(index, column, key, bool:ascending = false) -> 1 = best, 0 = not ranked
MP_NATIVE(n_idx_rank)
{
	MP_GETI(ix, 1);
	std::string col = CleanToken(Str(amx, params[2]));
	std::string key = IdxKey(Str(amx, params[3]));
	bool asc = (MP_ParamCount(params) >= 4) ? (params[4] != 0) : false;

	int n = IdxFindRow(*ix, key);
	if (n < 0) return 0;

	bool ok = false;
	double mine = IdxNum(ix->rows[n], col, ok);
	if (!ok) return 0;

	int better = 0;
	for (size_t i = 0; i < ix->rows.size(); i++)
	{
		bool ok2 = false;
		double v = IdxNum(ix->rows[i], col, ok2);
		if (ok2 && (asc ? (v < mine) : (v > mine))) better++;
	}
	return better + 1;
}

// metaprofiler_index_count_where(index, column, MPOp:op, value)
MP_NATIVE(n_idx_count_where)
{
	MP_GETI(ix, 1);
	std::string col = CleanToken(Str(amx, params[2]));
	int op = params[3];
	double rhs = (double)params[4];

	int n = 0;
	for (size_t i = 0; i < ix->rows.size(); i++)
	{
		bool ok = false;
		double v = IdxNum(ix->rows[i], col, ok);
		if (!ok) continue;

		bool pass = false;
		switch (op)
		{
			case MPO_EQ: pass = (v == rhs); break;
			case MPO_NE: pass = (v != rhs); break;
			case MPO_GT: pass = (v >  rhs); break;
			case MPO_GE: pass = (v >= rhs); break;
			case MPO_LT: pass = (v <  rhs); break;
			case MPO_LE: pass = (v <= rhs); break;
		}
		if (pass) n++;
	}
	return n;
}

// ---- save_step / version ------------------------------------------

// metaprofiler_save_step(max = 4) : saves at most max dirty files (profiles + indexes),
// returns how many are still waiting. Call it from a timer to spread the disk writes.
MP_NATIVE(n_save_step)
{
	int maxn = (MP_ParamCount(params) >= 1) ? params[1] : 4;
	if (maxn < 1) maxn = 1;

	int saved = 0;

	for (size_t i = 0; i < g_Profiles.size() && saved < maxn; i++)
	{
		if (g_Profiles[i].used && g_Profiles[i].dirty)
		{
			SaveProfile(g_Profiles[i]);
			saved++;
		}
	}

	for (size_t i = 0; i < g_Indexes.size() && saved < maxn; i++)
	{
		if (g_Indexes[i].used && g_Indexes[i].dirty)
		{
			IdxSave(g_Indexes[i]);
			saved++;
		}
	}

	int left = 0;
	for (size_t i = 0; i < g_Profiles.size(); i++)
		if (g_Profiles[i].used && g_Profiles[i].dirty) left++;
	for (size_t i = 0; i < g_Indexes.size(); i++)
		if (g_Indexes[i].used && g_Indexes[i].dirty) left++;

	return left;
}

MP_NATIVE(n_version) { return MP_VERSION_NUM; }

MP_NATIVE(n_version_string)
{
	return MF_SetAmxString(amx, params[1], MP_VERSION, params[2]);
}

// ============================================================
//  Native table
// ============================================================

AMX_NATIVE_INFO metaprofiler_natives[] =
{
	// lifecycle
	{ "metaprofiler_player_connect",    n_player_connect },
	{ "metaprofiler_player_disconnect", n_player_disconnect },
	{ "metaprofiler_player_rename",     n_player_rename },
	{ "metaprofiler_get_handle",        n_get_handle },
	{ "metaprofiler_load",              n_load },
	{ "metaprofiler_exists",            n_exists },
	{ "metaprofiler_delete",            n_delete },
	{ "metaprofiler_is_valid",          n_is_valid },
	{ "metaprofiler_is_online",         n_is_online },
	{ "metaprofiler_get_key",           n_get_key },

	// key / value
	{ "metaprofiler_set",               n_set },
	{ "metaprofiler_get",               n_get },
	{ "metaprofiler_set_int",           n_set_int },
	{ "metaprofiler_get_int",           n_get_int },
	{ "metaprofiler_add_int",           n_add_int },
	{ "metaprofiler_set_float",         n_set_float },
	{ "metaprofiler_get_float",         n_get_float },
	{ "metaprofiler_has",               n_has },
	{ "metaprofiler_remove",            n_remove },
	{ "metaprofiler_key_count",         n_key_count },
	{ "metaprofiler_key_at",            n_key_at },
	{ "metaprofiler_section_count",     n_section_count },
	{ "metaprofiler_section_at",        n_section_at },

	// compare / check
	{ "metaprofiler_compare_int",       n_compare_int },
	{ "metaprofiler_compare_float",     n_compare_float },
	{ "metaprofiler_compare_str",       n_compare_str },
	{ "metaprofiler_check",             n_check },
	{ "metaprofiler_equals",            n_equals },
	{ "metaprofiler_diff",              n_diff },
	{ "metaprofiler_copy",              n_copy },

	// logs
	{ "metaprofiler_log",               n_log },
	{ "metaprofiler_log_count",         n_log_count },
	{ "metaprofiler_log_get",           n_log_get },
	{ "metaprofiler_log_clear",         n_log_clear },
	{ "metaprofiler_log_search",        n_log_search },
	{ "metaprofiler_set_log_limit",     n_set_log_limit },

	// persistence / config / tools
	{ "metaprofiler_save",              n_save },
	{ "metaprofiler_save_all",          n_save_all },
	{ "metaprofiler_save_step",         n_save_step },
	{ "metaprofiler_version",           n_version },
	{ "metaprofiler_version_string",    n_version_string },
	{ "metaprofiler_reload",            n_reload },
	{ "metaprofiler_is_dirty",          n_is_dirty },
	{ "metaprofiler_set_keymode",       n_set_keymode },
	{ "metaprofiler_get_keymode",       n_get_keymode },
	{ "metaprofiler_scan",              n_scan },
	{ "metaprofiler_scan_get",          n_scan_get },
	{ "metaprofiler_dump",              n_dump },
	{ "metaprofiler_diag",              n_diag },

	// indexes
	{ "metaprofiler_index_open",        n_idx_open },
	{ "metaprofiler_index_exists",      n_idx_exists },
	{ "metaprofiler_index_is_valid",    n_idx_is_valid },
	{ "metaprofiler_index_delete",      n_idx_delete },
	{ "metaprofiler_index_clear",       n_idx_clear },
	{ "metaprofiler_index_save",        n_idx_save },
	{ "metaprofiler_index_count",       n_idx_count },
	{ "metaprofiler_index_key_at",      n_idx_key_at },
	{ "metaprofiler_index_has",         n_idx_has },
	{ "metaprofiler_index_remove",      n_idx_remove },
	{ "metaprofiler_index_set",         n_idx_set },
	{ "metaprofiler_index_get",         n_idx_get },
	{ "metaprofiler_index_set_int",     n_idx_set_int },
	{ "metaprofiler_index_get_int",     n_idx_get_int },
	{ "metaprofiler_index_add_int",     n_idx_add_int },
	{ "metaprofiler_index_set_float",   n_idx_set_float },
	{ "metaprofiler_index_get_float",   n_idx_get_float },
	{ "metaprofiler_index_top",         n_idx_top },
	{ "metaprofiler_index_rank",        n_idx_rank },
	{ "metaprofiler_index_count_where", n_idx_count_where },

	{ nullptr, nullptr }
};

// ============================================================
//  Module lifecycle
// ============================================================

void OnAmxxAttach()
{
	g_Profiles.assign(MP_MAX_PROFILES, Profile());
	g_Indexes.assign(MP_MAX_INDEXES, Index());
	EnsureDir();
	MF_AddNatives(metaprofiler_natives);
	MP_OUT("[MetaProfiler] v%s loaded successfully.\n", MP_VERSION);
}

void OnPluginsLoaded()
{
	// forwards must be registered after the plugins are loaded
	g_FwdCreated = MF_RegisterForward("metaprofiler_profile_created", ET_IGNORE, FP_CELL, FP_CELL, FP_DONE);
	g_FwdLoaded  = MF_RegisterForward("metaprofiler_profile_loaded",  ET_IGNORE, FP_CELL, FP_CELL, FP_DONE);
	g_FwdClosing = MF_RegisterForward("metaprofiler_profile_closing", ET_IGNORE, FP_CELL, FP_CELL, FP_DONE);
	g_FwdRenamed = MF_RegisterForward("metaprofiler_profile_renamed", ET_IGNORE, FP_CELL, FP_CELL, FP_CELL, FP_DONE);
	g_FwdsReady  = true;
}

void OnPluginsUnloaded()
{
	// the plugins are going away: do not call into them any more
	g_FwdsReady = false;

	// map change: close every session and flush to disk
	for (int id = 1; id <= MP_MAX_PLAYERS; id++)
		if (g_Players[id].slot >= 0) DoDisconnect(id);
	SaveAll();
}

void OnAmxxDetach()
{
	g_FwdsReady = false;
	for (int id = 1; id <= MP_MAX_PLAYERS; id++)
		if (g_Players[id].slot >= 0) DoDisconnect(id);
	SaveAll();
}
