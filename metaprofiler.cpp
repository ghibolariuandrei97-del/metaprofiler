// ============================================================
//  MetaProfiler - Persistent Player Profile Engine for AMX Mod X
//  Version: 1.0.0
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
//  Optional but recommended - in moduleconfig.h enable:
//     #define FN_AMXX_PLUGINSUNLOADED OnPluginsUnloaded
// ============================================================

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <string>
#include <vector>
#include <algorithm>
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

#define MP_VERSION    "1.0.0"
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
	char buf[32];
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

	remove(path.c_str());		// Windows can't rename over an existing file
	if (rename(tmp.c_str(), path.c_str()) != 0)
	{
		MP_OUT("[MetaProfiler] ERROR: cannot replace %s\n", path.c_str());
		return false;
	}

	p.dirty = false;
	g_Saves++;
	return true;
}

static void SaveAll()
{
	for (size_t i = 0; i < g_Profiles.size(); i++)
		if (g_Profiles[i].used && g_Profiles[i].dirty)
			SaveProfile(g_Profiles[i]);
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

	return MakeHandle(s);
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
		AddLog(g_Profiles[ps.slot], MPL_HISTORY, "changed name to '" + newName + "'");
		DoDisconnect(id);
		cell h = DoConnect(id, newName, ps.authid, ps.ip);
		Profile* np = HandleToProfile(h);
		if (np) AddLog(*np, MPL_HISTORY, "name changed from '" + ps.name + "'");
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

	PrintLine('=', 78);
	MP_OUT("\n");
	return 1;
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
	{ "metaprofiler_reload",            n_reload },
	{ "metaprofiler_is_dirty",          n_is_dirty },
	{ "metaprofiler_set_keymode",       n_set_keymode },
	{ "metaprofiler_get_keymode",       n_get_keymode },
	{ "metaprofiler_scan",              n_scan },
	{ "metaprofiler_scan_get",          n_scan_get },
	{ "metaprofiler_dump",              n_dump },
	{ "metaprofiler_diag",              n_diag },

	{ nullptr, nullptr }
};

// ============================================================
//  Module lifecycle
// ============================================================

void OnAmxxAttach()
{
	g_Profiles.assign(MP_MAX_PROFILES, Profile());
	EnsureDir();
	MF_AddNatives(metaprofiler_natives);
	MP_OUT("[MetaProfiler] v%s loaded successfully.\n", MP_VERSION);
}

void OnPluginsUnloaded()
{
	// map change: close every session and flush to disk
	for (int id = 1; id <= MP_MAX_PLAYERS; id++)
		if (g_Players[id].slot >= 0) DoDisconnect(id);
	SaveAll();
}

void OnAmxxDetach()
{
	for (int id = 1; id <= MP_MAX_PLAYERS; id++)
		if (g_Players[id].slot >= 0) DoDisconnect(id);
	SaveAll();
}
