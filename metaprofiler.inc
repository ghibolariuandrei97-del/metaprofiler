#if defined _metaprofiler_included
	#endinput
#endif
#define _metaprofiler_included

// ============================================================
//  MetaProfiler v1.0.0 - Persistent Player Profile Engine
//  One .ini file per player: addons/amxmodx/data/metaprofiler/<key>.ini
// ============================================================
//
//  HOW IT WORKS
//  - The module stores/reads the profiles. The collector plugin
//    (metaprofiler.sma) feeds it events, but ANY plugin can read and
//    write profile data with the natives below.
//  - Everything works with a profile HANDLE (MPProfile:).
//      Online player : metaprofiler_get_handle(id)
//      Any player    : metaprofiler_load("name")   (offline too)
//  - A handle stays valid while the profile is cached. If it becomes
//    invalid (metaprofiler_is_valid), load it again.
//  - Section and key names are case-insensitive.
//    Values are single-line text (leading/trailing spaces are trimmed).
//
// ============================================================

#pragma reqlib metaprofiler
#if !defined AMXMODX_NOAUTOLOAD
	#pragma loadlib metaprofiler
#endif

// ------------------------------------------------------------
//  Types
// ------------------------------------------------------------

enum MPProfile
{
	MP_INVALID = 0
};

/* Log sections stored at the bottom of each profile */
enum MPLog
{
	MPLOG_HISTORY = 0,   // connects, disconnects, name changes...
	MPLOG_CHAT,          // chat messages
	MPLOG_GAMEPLAY,      // kills, maps, events
	MPLOG_NAMES,         // unique names used (read-only)
	MPLOG_IPS            // unique IPs used (read-only)
};

/* What identifies a player (decides the file name) */
enum MPKeyMode
{
	MPKEY_NAME = 0,      // default: name.ini
	MPKEY_AUTHID,        // steamid.ini
	MPKEY_IP             // ip.ini
};

/* Operators for metaprofiler_check() */
enum MPOp
{
	MPOP_EQ = 0,         // ==
	MPOP_NE,             // !=
	MPOP_GT,             // >
	MPOP_GE,             // >=
	MPOP_LT,             // <
	MPOP_LE              // <=
};

/* Default sections created by the collector plugin */
#define MP_SEC_PROFILE   "Profile"
#define MP_SEC_STATS     "Stats"

// ============================================================
//  PLAYER LIFECYCLE (used by the collector plugin)
// ============================================================

/* Loads / creates the profile of a connecting player and binds it to id.
   Returns the handle (MP_INVALID on failure). */
native MPProfile:metaprofiler_player_connect(id, const name[], const authid[], const ip[]);

/* Closes the session (adds playtime), saves and unbinds the player. */
native metaprofiler_player_disconnect(id);

/* Call when a player changes name. In MPKEY_NAME mode the player is moved
   to the profile of the new name. Returns the (new) handle. */
native MPProfile:metaprofiler_player_rename(id, const newname[]);

// ============================================================
//  HANDLES
// ============================================================

/* Handle of a connected player (MP_INVALID if none). */
native MPProfile:metaprofiler_get_handle(id);

/* Loads a profile by identity (name / authid / ip, depending on key mode).
   create = true creates an empty profile if it does not exist. */
native MPProfile:metaprofiler_load(const identity[], bool:create = false);

/* Does a profile exist (cached or on disk)? */
native bool:metaprofiler_exists(const identity[]);

/* Deletes a profile file. Refuses (returns false) if the player is online. */
native bool:metaprofiler_delete(const identity[]);

native bool:metaprofiler_is_valid(MPProfile:profile);
native bool:metaprofiler_is_online(MPProfile:profile);

/* File name (without .ini) of the profile. */
native metaprofiler_get_key(MPProfile:profile, out[], len);

// ============================================================
//  KEY / VALUE API
// ============================================================

/* Writes a value. Section names History/Chat/Gameplay/Names/IPs are reserved. */
native bool:metaprofiler_set(MPProfile:profile, const section[], const key[], const value[]);

/* Reads a value into out[]. Returns true if the key exists,
   otherwise out[] = def[] and false is returned. Use charsmax() for len. */
native bool:metaprofiler_get(MPProfile:profile, const section[], const key[], out[], len, const def[] = "");

native bool:metaprofiler_set_int(MPProfile:profile, const section[], const key[], value);
native metaprofiler_get_int(MPProfile:profile, const section[], const key[], def = 0);

/* Adds amount to a counter and returns the new value. */
native metaprofiler_add_int(MPProfile:profile, const section[], const key[], amount = 1);

native bool:metaprofiler_set_float(MPProfile:profile, const section[], const key[], Float:value);
native Float:metaprofiler_get_float(MPProfile:profile, const section[], const key[], Float:def = 0.0);

native bool:metaprofiler_has(MPProfile:profile, const section[], const key[]);
native bool:metaprofiler_remove(MPProfile:profile, const section[], const key[]);

/* Enumeration */
native metaprofiler_key_count(MPProfile:profile, const section[]);
native bool:metaprofiler_key_at(MPProfile:profile, const section[], index, out[], len);
native metaprofiler_section_count(MPProfile:profile);
native bool:metaprofiler_section_at(MPProfile:profile, index, out[], len);

// ============================================================
//  COMPARE / CHECK
// ============================================================

/* Compare the same key in two profiles. Returns -1 (a < b), 0 (equal), 1 (a > b).
   Missing keys count as 0 (numbers) or "" (strings). Invalid handles return 0. */
native metaprofiler_compare_int(MPProfile:a, MPProfile:b, const section[], const key[]);
native metaprofiler_compare_float(MPProfile:a, MPProfile:b, const section[], const key[]);
native metaprofiler_compare_str(MPProfile:a, MPProfile:b, const section[], const key[], bool:ignorecase = true);

/* Checks an int value against a number, e.g.
   metaprofiler_check(h, "Stats", "kills", MPOP_GE, 100) */
native bool:metaprofiler_check(MPProfile:profile, const section[], const key[], MPOp:op, value);

/* Is the stored text equal to value? (false if the key is missing) */
native bool:metaprofiler_equals(MPProfile:profile, const section[], const key[], const value[], bool:ignorecase = true);

/* Number of keys in a section that differ or exist on only one side. */
native metaprofiler_diff(MPProfile:a, MPProfile:b, const section[]);

/* Copies a whole section from one profile to another. Returns keys copied. */
native metaprofiler_copy(MPProfile:src, MPProfile:dst, const section[], bool:overwrite = true);

// ============================================================
//  LOGS  (History / Chat / Gameplay are writable; Names / IPs read-only)
// ============================================================

/* Adds a timestamped line: "2026-10-30 12:00:00 | text" */
native bool:metaprofiler_log(MPProfile:profile, MPLog:type, const fmt[], any:...);

native metaprofiler_log_count(MPProfile:profile, MPLog:type);

/* index 0 = newest line */
native bool:metaprofiler_log_get(MPProfile:profile, MPLog:type, index, out[], len);

native bool:metaprofiler_log_clear(MPProfile:profile, MPLog:type);

/* Number of lines containing text (case-insensitive). */
native metaprofiler_log_search(MPProfile:profile, MPLog:type, const text[]);

/* Max stored lines per log section (5 - 5000). Oldest lines are dropped. */
native metaprofiler_set_log_limit(MPLog:type, limit);

// ============================================================
//  PERSISTENCE / CONFIG / TOOLS
// ============================================================

native bool:metaprofiler_save(MPProfile:profile);
native metaprofiler_save_all();
native bool:metaprofiler_reload(MPProfile:profile);   // discards unsaved changes
native bool:metaprofiler_is_dirty(MPProfile:profile);

/* Set it once at startup, before players connect. */
native bool:metaprofiler_set_keymode(MPKeyMode:mode);
native MPKeyMode:metaprofiler_get_keymode();

/* Lists the profiles on disk. scan() returns the count (filter = substring),
   then read them with scan_get(index). */
native metaprofiler_scan(const filter[] = "");
native bool:metaprofiler_scan_get(index, out[], len);

/* Prints one profile / the whole module state to the server console. */
native metaprofiler_dump(MPProfile:profile);
native metaprofiler_diag();

// ============================================================
//  STOCKS
// ============================================================

/* Quick counter for a connected player: metaprofiler_count(id, "Stats", "kills") */
stock metaprofiler_count(id, const section[], const key[], amount = 1)
{
	new MPProfile:h = metaprofiler_get_handle(id);
	if (h == MP_INVALID)
		return 0;

	return metaprofiler_add_int(h, section, key, amount);
}

/* Reads an int from a connected player's profile. */
stock metaprofiler_player_int(id, const section[], const key[], def = 0)
{
	new MPProfile:h = metaprofiler_get_handle(id);
	if (h == MP_INVALID)
		return def;

	return metaprofiler_get_int(h, section, key, def);
}
