#if defined _metaprofiler_included
	#endinput
#endif
#define _metaprofiler_included

// ============================================================
//  MetaProfiler v1.0.0 - Persistent Player Profile Engine
//  One .ini file per player: addons/amxmodx/data/metaprofiler/<key>.ini
// ============================================================
//
//  WHAT IS IT
//  ----------
//  MetaProfiler stores a profile for every player in its own .ini
//  file. The module only STORES and READS data. A collector plugin
//  (metaprofiler.sma) watches the game and feeds the module, but ANY
//  plugin can read and write profile data with the natives below.
//
//  QUICK START
//  -----------
//      #include <amxmodx>
//      #include <metaprofiler>
//
//      public my_event(id)
//      {
//          // online player: get the handle, then use it
//          new MPProfile:h = metaprofiler_get_handle(id);
//          if (h == MP_INVALID)
//              return;
//
//          metaprofiler_add_int(h, "Stats", "my_counter");        // +1
//          metaprofiler_set(h, "Settings", "color", "red");
//          metaprofiler_log(h, MPLOG_GAMEPLAY, "did something cool");
//      }
//
//      // offline player: load by name (no need to be connected)
//      new MPProfile:h = metaprofiler_load("john");
//      if (h != MP_INVALID)
//          server_print("kills: %d", metaprofiler_get_int(h, "Stats", "kills"));
//
//  HOW A PROFILE FILE LOOKS
//  ------------------------
//      [Profile]   <- normal sections: key=value (you can create any)
//      name=John
//      [Stats]
//      kills=12
//      [History]   <- log sections: timestamped lines
//      2026-10-30 12:00:00 | connected as 'John' ...
//
//      Reserved log sections (cannot be used as normal sections):
//      History, Chat, Gameplay, Names, IPs
//
//  IMPORTANT RULES
//  ---------------
//  - HANDLES: everything works with a profile handle (MPProfile:).
//    A handle stays valid while the profile is cached in memory.
//    Offline profiles can be dropped from the cache when it is full
//    (255 profiles), so if metaprofiler_is_valid(h) is false, load it again.
//    Handles of ONLINE players are never dropped.
//  - NAMES: section and key names are case-insensitive ("Stats" == "stats").
//    The characters  [ ] = ;  and line breaks are replaced with "_".
//  - VALUES: always stored as single-line text. Line breaks and tabs
//    become spaces and leading / trailing spaces are removed.
//  - STRING OUTPUT: for natives with  out[], len  always pass
//    charsmax(out) as len, like get_user_name().
//  - SAVING: profiles are saved on disconnect, on map change, by
//    metaprofiler_save_all() and when a profile is dropped from the cache.
//    A server crash can lose the changes made since the last save.
//
// ============================================================

#pragma reqlib metaprofiler
#if !defined AMXMODX_NOAUTOLOAD
	#pragma loadlib metaprofiler
#endif

// ------------------------------------------------------------
//  Types
// ------------------------------------------------------------

/* Profile handle. MP_INVALID (0) means "no profile" / failure. */
enum MPProfile
{
	MP_INVALID = 0
};

/* Log sections stored at the bottom of each profile.
   MPLOG_HISTORY, MPLOG_CHAT and MPLOG_GAMEPLAY can be written by plugins.
   MPLOG_NAMES and MPLOG_IPS are filled by the module (read-only). */
enum MPLog
{
	MPLOG_HISTORY = 0,   // connects, disconnects, name changes...
	MPLOG_CHAT,          // chat messages
	MPLOG_GAMEPLAY,      // kills, maps, custom events
	MPLOG_NAMES,         // unique names the player used (read-only)
	MPLOG_IPS            // unique IPs the player used (read-only)
};

/* What identifies a player = what the profile file is named after. */
enum MPKeyMode
{
	MPKEY_NAME = 0,      // default: name.ini   (lower-cased, safe characters only)
	MPKEY_AUTHID,        // steamid.ini
	MPKEY_IP             // ip.ini
};

/* Comparison operators for metaprofiler_check(). */
enum MPOp
{
	MPOP_EQ = 0,         // equal               ==
	MPOP_NE,             // not equal           !=
	MPOP_GT,             // greater than        >
	MPOP_GE,             // greater or equal    >=
	MPOP_LT,             // less than           <
	MPOP_LE              // less or equal       <=
};

/* Default section names used by the collector plugin. */
#define MP_SEC_PROFILE   "Profile"
#define MP_SEC_STATS     "Stats"

// ============================================================
//  PLAYER LIFECYCLE  (normally used only by the collector plugin)
// ============================================================

/**
 * A player joined: loads the profile (or creates it) and binds it to the player.
 * Also updates [Profile] (name, authid, ip, first_seen, last_seen, connects),
 * the unique Names / IPs lists, and writes a line in the History log.
 *
 * Call it from client_putinserver(). If the player is already bound
 * (reconnect), the old session is closed first.
 *
 * @param id       Player index (1-32)
 * @param name     Player name
 * @param authid   SteamID (or whatever get_user_authid() returns)
 * @param ip       IP address (the ":port" part is removed automatically)
 *
 * @return         Profile handle, or MP_INVALID on failure
 *                 (invalid id, or the cache is full of online profiles)
 */
native MPProfile:metaprofiler_player_connect(id, const name[], const authid[], const ip[]);

/**
 * A player left: adds the session time to [Profile] total_playtime,
 * sets last_session, writes a History line, saves the file and unbinds the player.
 *
 * Call it from client_disconnected() and from plugin_end() (map change).
 *
 * @param id       Player index (1-32)
 *
 * @return         1 if a session was closed, 0 if the player was not bound
 */
native metaprofiler_player_disconnect(id);

/**
 * The player changed name. Does nothing if the name is the same.
 *  - MPKEY_NAME mode : the player is moved to the profile of the new name
 *                      (the old session is closed, a new one starts).
 *  - Other modes     : the profile stays, the new name is added to
 *                      [Profile] name and to the Names list.
 * Both cases write a History line.
 *
 * Call it from client_infochanged().
 *
 * @param id       Player index (1-32)
 * @param newname  The new name
 *
 * @return         The (possibly new) handle, or MP_INVALID if the player is not bound.
 *                 In MPKEY_NAME mode the handle changes: update your stored copy!
 */
native MPProfile:metaprofiler_player_rename(id, const newname[]);

// ============================================================
//  HANDLES
// ============================================================

/**
 * Gets the handle of a connected player.
 *
 * @param id       Player index (1-32)
 *
 * @return         Profile handle, or MP_INVALID if the player has no profile
 *                 (not connected, bot, or the collector did not bind him)
 */
native MPProfile:metaprofiler_get_handle(id);

/**
 * Loads any profile by identity, even if the player is offline.
 * The identity is the NAME, SteamID or IP, depending on the key mode,
 * and is converted to the file name with the same safe rules as for players
 * ("John" and "john" are the same profile).
 *
 * The profile is cached; later loads of the same player are instant.
 *
 * @param identity Name / SteamID / IP of the player
 * @param create   true = create an empty profile if it does not exist
 *                 false = return MP_INVALID if it does not exist
 *
 * @return         Profile handle, or MP_INVALID
 */
native MPProfile:metaprofiler_load(const identity[], bool:create = false);

/**
 * Checks if a profile exists (in memory or on disk).
 *
 * @param identity Name / SteamID / IP of the player
 *
 * @return         true if it exists
 */
native bool:metaprofiler_exists(const identity[]);

/**
 * Deletes a profile file from disk and from the cache. This cannot be undone.
 * Refuses to delete a profile that belongs to a connected player.
 *
 * @param identity Name / SteamID / IP of the player
 *
 * @return         true if the file was deleted
 */
native bool:metaprofiler_delete(const identity[]);

/**
 * Is this handle still usable?
 * Returns false for MP_INVALID and for handles of offline profiles that
 * were dropped from the cache. Load the profile again in that case.
 */
native bool:metaprofiler_is_valid(MPProfile:profile);

/**
 * Is a player currently connected with this profile?
 * Returns false for invalid handles.
 */
native bool:metaprofiler_is_online(MPProfile:profile);

/**
 * Gets the file name of a profile (without ".ini"), for example "john".
 *
 * @param profile  Profile handle
 * @param out      Buffer for the key
 * @param len      Buffer size, use charsmax(out)
 *
 * @return         Non-zero on success, 0 on an invalid handle
 */
native metaprofiler_get_key(MPProfile:profile, out[], len);

// ============================================================
//  KEY / VALUE API
//  Store anything you want: native metaprofiler_set(h, "MySection", "MyKey", "value")
// ============================================================

/**
 * Writes a text value (creates the section and the key if needed).
 *
 * @param profile  Profile handle
 * @param section  Section name. The names History, Chat, Gameplay, Names and IPs are reserved
 * @param key      Key name
 * @param value    Value to store
 *
 * @return         true on success, false on an invalid handle, an empty
 *                 section / key, or a reserved section name
 */
native bool:metaprofiler_set(MPProfile:profile, const section[], const key[], const value[]);

/**
 * Reads a text value.
 *
 * @param profile  Profile handle
 * @param section  Section name
 * @param key      Key name
 * @param out      Buffer for the value
 * @param len      Buffer size, use charsmax(out)
 * @param def      Text copied to out[] if the key does not exist
 *
 * @return         true if the key exists. If false, out[] contains def[].
 */
native bool:metaprofiler_get(MPProfile:profile, const section[], const key[], out[], len, const def[] = "");

/**
 * Writes a whole number.
 *
 * @return         true on success
 */
native bool:metaprofiler_set_int(MPProfile:profile, const section[], const key[], value);

/**
 * Reads a whole number.
 *
 * @param def      Value returned if the key does not exist or is empty
 *
 * @return         The stored number (text that is not a number gives 0)
 */
native metaprofiler_get_int(MPProfile:profile, const section[], const key[], def = 0);

/**
 * Adds to a counter (a missing key starts at 0). Use a negative amount to subtract.
 * Example: metaprofiler_add_int(h, "Stats", "kills");   // kills + 1
 *
 * @param amount   Amount to add (default 1)
 *
 * @return         The new value, 0 on an invalid handle
 */
native metaprofiler_add_int(MPProfile:profile, const section[], const key[], amount = 1);

/**
 * Writes a decimal number (stored with 4 decimals).
 *
 * @return         true on success
 */
native bool:metaprofiler_set_float(MPProfile:profile, const section[], const key[], Float:value);

/**
 * Reads a decimal number.
 *
 * @param def      Value returned if the key does not exist or is empty
 */
native Float:metaprofiler_get_float(MPProfile:profile, const section[], const key[], Float:def = 0.0);

/**
 * Does the key exist?
 */
native bool:metaprofiler_has(MPProfile:profile, const section[], const key[]);

/**
 * Deletes a key from a section.
 *
 * @return         true if the key existed and was removed
 */
native bool:metaprofiler_remove(MPProfile:profile, const section[], const key[]);

/**
 * Enumeration: lets you list everything stored in a profile.
 * Indexes start at 0 and follow the order in which keys / sections were created.
 *
 * Example:
 *     new key[32], value[64];
 *     for (new i = 0; i < metaprofiler_key_count(h, "Stats"); i++)
 *     {
 *         metaprofiler_key_at(h, "Stats", i, key, charsmax(key));
 *         metaprofiler_get(h, "Stats", key, value, charsmax(value));
 *     }
 */

/* Number of keys in a section (0 if the section does not exist). */
native metaprofiler_key_count(MPProfile:profile, const section[]);

/* Name of the key at an index. Returns false if the index is out of range. */
native bool:metaprofiler_key_at(MPProfile:profile, const section[], index, out[], len);

/* Number of normal sections (the log sections are not counted). */
native metaprofiler_section_count(MPProfile:profile);

/* Name of the section at an index. Returns false if the index is out of range. */
native bool:metaprofiler_section_at(MPProfile:profile, index, out[], len);

// ============================================================
//  COMPARE / CHECK
// ============================================================

/**
 * Compares the same key in two profiles.
 * A missing key counts as 0 (numbers) or "" (text).
 * Note: an invalid handle also returns 0, so check metaprofiler_is_valid() first
 * if you need to tell "equal" from "invalid".
 *
 * Example: who has more kills?
 *     new r = metaprofiler_compare_int(hA, hB, "Stats", "kills");
 *     // r > 0 : A has more, r < 0 : B has more, r == 0 : tie
 *
 * @return         -1 if a < b, 0 if equal, 1 if a > b
 */
native metaprofiler_compare_int(MPProfile:a, MPProfile:b, const section[], const key[]);

/* Same as compare_int, but for decimal numbers. */
native metaprofiler_compare_float(MPProfile:a, MPProfile:b, const section[], const key[]);

/* Same as compare_int, but compares the text (alphabetical order).
   ignorecase = true treats "John" and "john" as equal. */
native metaprofiler_compare_str(MPProfile:a, MPProfile:b, const section[], const key[], bool:ignorecase = true);

/**
 * Checks a whole number of a profile against a value.
 * A missing key counts as 0.
 *
 * Example: has the player 100 kills or more?
 *     if (metaprofiler_check(h, "Stats", "kills", MPOP_GE, 100)) ...
 *
 * @param op       One of the MPOP_* operators
 * @param value    The number to compare with
 *
 * @return         true if the check passes
 */
native bool:metaprofiler_check(MPProfile:profile, const section[], const key[], MPOp:op, value);

/**
 * Is the stored text equal to a given text?
 *
 * @param ignorecase true = "Red" equals "red"
 *
 * @return         true if equal, false if different or if the key does not exist
 */
native bool:metaprofiler_equals(MPProfile:profile, const section[], const key[], const value[], bool:ignorecase = true);

/**
 * Counts how many keys of a section are different between two profiles.
 * A key counts if the values differ (case-sensitive) or if it exists in only one profile.
 * 0 means the section is identical in both.
 *
 * @return         Number of different keys
 */
native metaprofiler_diff(MPProfile:a, MPProfile:b, const section[]);

/**
 * Copies a whole section from one profile to another.
 *
 * @param overwrite true = replace keys that already exist in dst
 *                  false = only copy keys that dst does not have yet
 *
 * @return         Number of keys copied
 */
native metaprofiler_copy(MPProfile:src, MPProfile:dst, const section[], bool:overwrite = true);

// ============================================================
//  LOGS
//  History / Chat / Gameplay are writable. Names / IPs are read-only.
//  Every line is stored as:  "2026-10-30 12:00:00 | your text"
//  Each log has a size limit, the oldest lines are removed first.
// ============================================================

/**
 * Adds a line to a log (the time is added automatically).
 * Works like server_print(): the message can use formats (%d, %s ...).
 * Lines are limited to about 480 characters.
 *
 * Example: metaprofiler_log(h, MPLOG_GAMEPLAY, "won the round on %s", map);
 *
 * @param type     MPLOG_HISTORY, MPLOG_CHAT or MPLOG_GAMEPLAY
 *
 * @return         true on success, false on an invalid handle or a read-only log
 */
native bool:metaprofiler_log(MPProfile:profile, MPLog:type, const fmt[], any:...);

/**
 * Number of lines currently stored in a log (any of the 5 logs).
 */
native metaprofiler_log_count(MPProfile:profile, MPLog:type);

/**
 * Reads one log line (any of the 5 logs).
 *
 * @param index    0 = the newest line, 1 = the one before it, and so on
 * @param out      Buffer for the line
 * @param len      Buffer size, use charsmax(out)
 *
 * @return         true on success, false if the index is out of range
 */
native bool:metaprofiler_log_get(MPProfile:profile, MPLog:type, index, out[], len);

/**
 * Deletes all lines of a writable log. This cannot be undone.
 *
 * @return         true on success
 */
native bool:metaprofiler_log_clear(MPProfile:profile, MPLog:type);

/**
 * Counts the lines of a log that contain a text (case-insensitive).
 * Example: how many times did he say "hello"?
 *     metaprofiler_log_search(h, MPLOG_CHAT, "hello");
 */
native metaprofiler_log_search(MPProfile:profile, MPLog:type, const text[]);

/**
 * Sets how many lines a log may keep (any of the 5 logs), from 5 to 5000.
 * Defaults: History 500, Chat 500, Gameplay 500, Names 25, IPs 25.
 * It applies to all profiles; extra old lines are removed the next time
 * a line is added or a profile is loaded. Call it once, in plugin_init().
 *
 * @return         true on success
 */
native metaprofiler_set_log_limit(MPLog:type, limit);

// ============================================================
//  PERSISTENCE / CONFIG / TOOLS
// ============================================================

/**
 * Saves one profile to disk right now.
 * The save is safe: the file is written to a temporary file first,
 * then replaces the old one, so a crash cannot leave a half-written profile.
 *
 * @return         true on success
 */
native bool:metaprofiler_save(MPProfile:profile);

/**
 * Saves every profile that has unsaved changes. Good for a timer or plugin_end().
 */
native metaprofiler_save_all();

/**
 * Reloads a profile from its file and DISCARDS all unsaved changes.
 * The handle and the online state stay the same.
 *
 * @return         true on success, false if the file does not exist
 */
native bool:metaprofiler_reload(MPProfile:profile);

/**
 * Does the profile have changes that are not saved yet?
 */
native bool:metaprofiler_is_dirty(MPProfile:profile);

/**
 * Chooses what the profile file is named after (name, SteamID or IP).
 * Call it once, in plugin_init(), before players connect.
 * It only affects players who connect afterwards; changing it later
 * makes players use a different file than before.
 *
 * @return         true on success, false for an invalid mode
 */
native bool:metaprofiler_set_keymode(MPKeyMode:mode);

/* Current key mode. */
native MPKeyMode:metaprofiler_get_keymode();

/**
 * Lists the profile files on disk, in two steps:
 *   1) metaprofiler_scan()  -> counts and remembers the matching profiles
 *   2) metaprofiler_scan_get(index) -> reads them one by one (alphabetical order)
 *
 * Example:
 *     new total = metaprofiler_scan("jo");     // all profiles with "jo" in the name
 *     new key[48];
 *     for (new i = 0; i < total; i++)
 *     {
 *         metaprofiler_scan_get(i, key, charsmax(key));
 *         server_print("%s", key);
 *     }
 *
 * The results are replaced by the next scan. metaprofiler_diag() also
 * scans, so do not call it between scan() and scan_get().
 *
 * @param filter   Only profiles containing this text (case-insensitive). "" = all
 *
 * @return         Number of matching profiles
 */
native metaprofiler_scan(const filter[] = "");

/**
 * Reads one result of the last metaprofiler_scan().
 * The text is the profile key, which you can pass to metaprofiler_load().
 *
 * @param index    0 to scan() - 1
 *
 * @return         true on success, false if the index is out of range
 */
native bool:metaprofiler_scan_get(index, out[], len);

/**
 * Prints one whole profile to the server console (values + last lines of each log).
 * Useful for debugging.
 */
native metaprofiler_dump(MPProfile:profile);

/**
 * Prints the module state to the server console: key mode, folder,
 * log limits, counters and a table of the profiles loaded in memory.
 */
native metaprofiler_diag();

// ============================================================
//  STOCKS  (small shortcuts, they only use the natives above)
// ============================================================

/**
 * Adds to a counter of a connected player in one call.
 * Example: metaprofiler_count(id, "Stats", "jumps");
 *
 * @return         The new value, or 0 if the player has no profile
 */
stock metaprofiler_count(id, const section[], const key[], amount = 1)
{
	new MPProfile:h = metaprofiler_get_handle(id);
	if (h == MP_INVALID)
		return 0;

	return metaprofiler_add_int(h, section, key, amount);
}

/**
 * Reads a whole number from a connected player's profile in one call.
 * Example: new kills = metaprofiler_player_int(id, "Stats", "kills");
 *
 * @return         The stored number, or def if the player has no profile or the key is missing
 */
stock metaprofiler_player_int(id, const section[], const key[], def = 0)
{
	new MPProfile:h = metaprofiler_get_handle(id);
	if (h == MP_INVALID)
		return def;

	return metaprofiler_get_int(h, section, key, def);
}
