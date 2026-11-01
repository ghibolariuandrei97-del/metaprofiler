#include <amxmodx>
#include <metaprofiler>

#define PLUGIN  "MetaProfiler Collector"
#define VERSION "1.2.0"
#define AUTHOR  "you"

#define AUTOSAVE_INTERVAL 10.0   // seconds between save steps
#define SAVES_PER_STEP    6      // files written per step (spreads the disk writes)

// extra sections used by this plugin
#define MP_SEC_LAST      "Last"
#define MP_SEC_WEAPONS   "Weapons"

new MPProfile:g_hProfile[33];
new g_iJoinTime[33];      // when the current session of this profile started
new g_iStreak[33];        // current kill streak (best streak is saved in the profile)

public plugin_init()
{
	register_plugin(PLUGIN, VERSION, AUTHOR);

	// profile key: MPKEY_NAME (name.ini), MPKEY_AUTHID or MPKEY_IP
	metaprofiler_set_keymode(MPKEY_NAME);

	// chat
	register_clcmd("say",      "hook_say");
	register_clcmd("say_team", "hook_say_team");
	register_clcmd("say /profile", "cmd_myprofile");

	// gameplay
	register_event("DeathMsg", "ev_death", "a");

	// admin / test commands (server console or rcon)
	register_srvcmd("mp_diag",    "cmd_diag");
	register_srvcmd("mp_show",    "cmd_show");      // mp_show <name>
	register_srvcmd("mp_scan",    "cmd_scan");      // mp_scan [filter]
	register_srvcmd("mp_compare", "cmd_compare");   // mp_compare <nameA> <nameB> <stat>
	register_srvcmd("mp_save",    "cmd_save");

	set_task(AUTOSAVE_INTERVAL, "task_autosave", 0, _, _, "b");
}

public plugin_end()
{
	// map change: close every session so playtime is stored, then flush
	for (new id = 1; id <= get_maxplayers(); id++)
	{
		if (g_hProfile[id] != MP_INVALID)
		{
			metaprofiler_player_disconnect(id);
			g_hProfile[id] = MP_INVALID;
		}
	}
	metaprofiler_save_all();
}

public task_autosave()
{
	// writes a few changed files at a time instead of everything at once
	metaprofiler_save_step(SAVES_PER_STEP);
}

// ============================================================
//  Helpers
// ============================================================

bool:is_valid_id(id)
{
	return (1 <= id <= get_maxplayers());
}

/* Makes sure every counter exists, so the file shows all stats from the first visit */
init_defaults(MPProfile:h)
{
	new const stats[][] =
	{
		"kills", "deaths", "headshots", "teamkills", "suicides",
		"best_streak", "chat_messages", "commands"
	};

	for (new i = 0; i < sizeof stats; i++)
	{
		if (!metaprofiler_has(h, MP_SEC_STATS, stats[i]))
			metaprofiler_set_int(h, MP_SEC_STATS, stats[i], 0);
	}

	if (!metaprofiler_has(h, MP_SEC_STATS, "kd"))
		metaprofiler_set_float(h, MP_SEC_STATS, "kd", 0.0);
}

/* K/D = kills / deaths (if deaths = 0, K/D = kills) */
update_kd(MPProfile:h)
{
	new kills  = metaprofiler_get_int(h, MP_SEC_STATS, "kills");
	new deaths = metaprofiler_get_int(h, MP_SEC_STATS, "deaths");

	metaprofiler_set_float(h, MP_SEC_STATS, "kd", deaths > 0 ? float(kills) / float(deaths) : float(kills));
}

timestamp(out[], len)
{
	format_time(out, len, "%Y-%m-%d %H:%M:%S");
}

/* 3725 seconds -> "1h 2m" */
format_playtime(seconds, out[], len)
{
	if (seconds < 0)
		seconds = 0;

	new h = seconds / 3600;
	new m = (seconds % 3600) / 60;

	if (h > 0)
		formatex(out, len, "%dh %dm", h, m);
	else
		formatex(out, len, "%dm %ds", m, seconds % 60);
}

/* Starts tracking a profile for this player (called on join and when the profile changes) */
bind_player(id, MPProfile:h)
{
	g_hProfile[id]  = h;
	g_iJoinTime[id] = get_systime();
	g_iStreak[id]   = 0;

	if (h != MP_INVALID)
		init_defaults(h);
}

// ============================================================
//  Connect / disconnect / name change
// ============================================================

public client_putinserver(id)
{
	g_hProfile[id] = MP_INVALID;

	if (is_user_bot(id) || is_user_hltv(id))
		return;

	new name[32], authid[35], ip[32], map[32];
	get_user_name(id, name, charsmax(name));
	get_user_authid(id, authid, charsmax(authid));
	get_user_ip(id, ip, charsmax(ip), 1);		// 1 = without port
	get_mapname(map, charsmax(map));

	new MPProfile:h = metaprofiler_player_connect(id, name, authid, ip);
	bind_player(id, h);

	if (h == MP_INVALID)
		return;

	metaprofiler_set(h, MP_SEC_PROFILE, "last_map", map);
	metaprofiler_log(h, MPLOG_GAMEPLAY, "joined map %s", map);
}

public client_disconnected(id)
{
	if (g_hProfile[id] == MP_INVALID)
		return;

	metaprofiler_player_disconnect(id);
	g_hProfile[id] = MP_INVALID;
	g_iStreak[id]  = 0;
}

public client_infochanged(id)
{
	if (g_hProfile[id] == MP_INVALID || !is_user_connected(id))
		return;

	new newname[32];
	get_user_info(id, "name", newname, charsmax(newname));

	// the module compares with the stored name and only acts on a real change
	new MPProfile:h = metaprofiler_player_rename(id, newname);

	// name mode: the player moved to another profile -> new session for it
	if (h != g_hProfile[id])
		bind_player(id, h);
}

// ============================================================
//  Chat
// ============================================================

public hook_say(id)
{
	return log_chat(id, false);
}

public hook_say_team(id)
{
	return log_chat(id, true);
}

log_chat(id, bool:team)
{
	if (g_hProfile[id] == MP_INVALID)
		return PLUGIN_CONTINUE;

	new msg[192];
	read_args(msg, charsmax(msg));
	remove_quotes(msg);
	trim(msg);

	if (!msg[0])
		return PLUGIN_CONTINUE;

	// commands (/login pass, !ban ...) are counted but NEVER stored: they can contain passwords
	if (msg[0] == '/' || msg[0] == '!' || msg[0] == '.')
	{
		metaprofiler_add_int(g_hProfile[id], MP_SEC_STATS, "commands");
		return PLUGIN_CONTINUE;
	}

	metaprofiler_add_int(g_hProfile[id], MP_SEC_STATS, "chat_messages");
	metaprofiler_log(g_hProfile[id], MPLOG_CHAT, "%s%s", team ? "(team) " : "", msg);
	return PLUGIN_CONTINUE;
}

// ============================================================
//  Gameplay: kills, deaths, streaks, last entries
// ============================================================

public ev_death()
{
	new killer = read_data(1);
	new victim = read_data(2);
	new hs     = read_data(3);
	new weapon[24];
	read_data(4, weapon, charsmax(weapon));

	new now[24], map[32];
	timestamp(now, charsmax(now));
	get_mapname(map, charsmax(map));

	new kname[32], vname[32];
	if (is_valid_id(killer)) get_user_name(killer, kname, charsmax(kname));
	if (is_valid_id(victim)) get_user_name(victim, vname, charsmax(vname));

	new bool:suicide = (killer == victim || !is_valid_id(killer));

	// ---------------- victim side ----------------
	if (is_valid_id(victim) && g_hProfile[victim] != MP_INVALID)
	{
		new MPProfile:hv = g_hProfile[victim];

		metaprofiler_add_int(hv, MP_SEC_STATS, "deaths");
		g_iStreak[victim] = 0;

		if (suicide)
		{
			metaprofiler_add_int(hv, MP_SEC_STATS, "suicides");
			metaprofiler_set(hv, MP_SEC_LAST, "last_death_by", "(suicide / world)");
			metaprofiler_log(hv, MPLOG_GAMEPLAY, "died (%s)", weapon);
		}
		else
		{
			metaprofiler_set(hv, MP_SEC_LAST, "last_death_by", kname);
			metaprofiler_log(hv, MPLOG_GAMEPLAY, "killed by %s with %s%s", kname, weapon, hs ? " (headshot)" : "");
		}

		metaprofiler_set(hv, MP_SEC_LAST, "last_death_weapon", weapon);
		metaprofiler_set_int(hv, MP_SEC_LAST, "last_death_headshot", hs ? 1 : 0);
		metaprofiler_set(hv, MP_SEC_LAST, "last_death_map", map);
		metaprofiler_set(hv, MP_SEC_LAST, "last_death_at", now);

		update_kd(hv);
	}

	// ---------------- killer side ----------------
	if (suicide || g_hProfile[killer] == MP_INVALID)
		return;

	new MPProfile:hk = g_hProfile[killer];

	if (get_user_team(killer) == get_user_team(victim))
	{
		metaprofiler_add_int(hk, MP_SEC_STATS, "teamkills");
		metaprofiler_set(hk, MP_SEC_LAST, "last_teamkill_victim", vname);
		metaprofiler_set(hk, MP_SEC_LAST, "last_teamkill_at", now);
		metaprofiler_log(hk, MPLOG_GAMEPLAY, "TEAMKILLED %s with %s", vname, weapon);
		return;
	}

	metaprofiler_add_int(hk, MP_SEC_STATS, "kills");
	metaprofiler_add_int(hk, MP_SEC_WEAPONS, weapon);

	if (hs)
		metaprofiler_add_int(hk, MP_SEC_STATS, "headshots");

	// streak
	g_iStreak[killer]++;
	if (g_iStreak[killer] > metaprofiler_get_int(hk, MP_SEC_STATS, "best_streak"))
		metaprofiler_set_int(hk, MP_SEC_STATS, "best_streak", g_iStreak[killer]);

	// last kill
	metaprofiler_set(hk, MP_SEC_LAST, "last_kill_victim", vname);
	metaprofiler_set(hk, MP_SEC_LAST, "last_kill_weapon", weapon);
	metaprofiler_set_int(hk, MP_SEC_LAST, "last_kill_headshot", hs ? 1 : 0);
	metaprofiler_set(hk, MP_SEC_LAST, "last_kill_map", map);
	metaprofiler_set(hk, MP_SEC_LAST, "last_kill_at", now);

	metaprofiler_log(hk, MPLOG_GAMEPLAY, "killed %s with %s%s", vname, weapon, hs ? " (headshot)" : "");

	update_kd(hk);
}

// ============================================================
//  Player command: say /profile
// ============================================================

public cmd_myprofile(id)
{
	new MPProfile:h = g_hProfile[id];
	if (h == MP_INVALID)
		return PLUGIN_CONTINUE;

	new kills   = metaprofiler_get_int(h, MP_SEC_STATS, "kills");
	new deaths  = metaprofiler_get_int(h, MP_SEC_STATS, "deaths");
	new hs      = metaprofiler_get_int(h, MP_SEC_STATS, "headshots");
	new tk      = metaprofiler_get_int(h, MP_SEC_STATS, "teamkills");
	new best    = metaprofiler_get_int(h, MP_SEC_STATS, "best_streak");
	new visits  = metaprofiler_get_int(h, MP_SEC_PROFILE, "connects");
	new Float:kd = metaprofiler_get_float(h, MP_SEC_STATS, "kd");

	// stored playtime is updated on disconnect, so add the current session
	new session = get_systime() - g_iJoinTime[id];
	new total   = metaprofiler_get_int(h, MP_SEC_PROFILE, "total_playtime") + session;

	new tbuf[24], sbuf[24];
	format_playtime(total, tbuf, charsmax(tbuf));
	format_playtime(session, sbuf, charsmax(sbuf));

	client_print(id, print_chat, "[Profile] Kills: %d | Deaths: %d | K/D: %.2f | HS: %d | TK: %d", kills, deaths, kd, hs, tk);
	client_print(id, print_chat, "[Profile] Best streak: %d | Total time: %s (this session: %s) | Visits: %d", best, tbuf, sbuf, visits);

	new victim[32], killer[32], weapon[24], at[24];

	if (metaprofiler_get(h, MP_SEC_LAST, "last_kill_victim", victim, charsmax(victim)))
	{
		metaprofiler_get(h, MP_SEC_LAST, "last_kill_weapon", weapon, charsmax(weapon));
		metaprofiler_get(h, MP_SEC_LAST, "last_kill_at", at, charsmax(at));
		client_print(id, print_chat, "[Profile] Last kill: %s (%s) at %s", victim, weapon, at);
	}

	if (metaprofiler_get(h, MP_SEC_LAST, "last_death_by", killer, charsmax(killer)))
	{
		metaprofiler_get(h, MP_SEC_LAST, "last_death_weapon", weapon, charsmax(weapon));
		metaprofiler_get(h, MP_SEC_LAST, "last_death_at", at, charsmax(at));
		client_print(id, print_chat, "[Profile] Last death: by %s (%s) at %s", killer, weapon, at);
	}

	return PLUGIN_HANDLED;
}

// ============================================================
//  Server commands (test / admin tools)
// ============================================================

public cmd_diag()
{
	metaprofiler_diag();
	return PLUGIN_HANDLED;
}

public cmd_save()
{
	metaprofiler_save_all();
	server_print("[MetaProfiler] all profiles saved");
	return PLUGIN_HANDLED;
}

public cmd_show()
{
	new name[32];
	read_argv(1, name, charsmax(name));

	new MPProfile:h = metaprofiler_load(name);
	if (h == MP_INVALID)
	{
		server_print("[MetaProfiler] no profile found for '%s'", name);
		return PLUGIN_HANDLED;
	}

	metaprofiler_dump(h);
	return PLUGIN_HANDLED;
}

public cmd_scan()
{
	new filter[32], entry[64];
	read_argv(1, filter, charsmax(filter));

	new count = metaprofiler_scan(filter);
	server_print("[MetaProfiler] %d profile(s) on disk%s", count, filter[0] ? " matching filter" : "");

	for (new i = 0; i < count && i < 50; i++)
	{
		metaprofiler_scan_get(i, entry, charsmax(entry));
		server_print("  %2d. %s", i + 1, entry);
	}
	return PLUGIN_HANDLED;
}

// mp_compare <nameA> <nameB> <stat>   (example: mp_compare john mike kills)
public cmd_compare()
{
	new a[32], b[32], stat[32];
	read_argv(1, a, charsmax(a));
	read_argv(2, b, charsmax(b));
	read_argv(3, stat, charsmax(stat));

	new MPProfile:ha = metaprofiler_load(a);
	new MPProfile:hb = metaprofiler_load(b);

	if (ha == MP_INVALID || hb == MP_INVALID)
	{
		server_print("[MetaProfiler] one of the profiles was not found");
		return PLUGIN_HANDLED;
	}

	new va = metaprofiler_get_int(ha, MP_SEC_STATS, stat);
	new vb = metaprofiler_get_int(hb, MP_SEC_STATS, stat);
	new cmp = metaprofiler_compare_int(ha, hb, MP_SEC_STATS, stat);

	server_print("[MetaProfiler] %s: %s = %d | %s = %d  ->  %s", stat, a, va, b, vb,
		cmp > 0 ? a : (cmp < 0 ? b : "TIE"));
	server_print("[MetaProfiler] differing keys in [Stats]: %d", metaprofiler_diff(ha, hb, MP_SEC_STATS));
	return PLUGIN_HANDLED;
}
