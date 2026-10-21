/*
 * The game database, GAMEDIR/game.db (#71, docs/data-files.md).
 *
 * One SQLite database holds a game.  PRAGMA user_version is its schema
 * version: mkgame creates it at DBVersion with DBCreate(), scepterd and
 * the tools open only that version with DBOpen(), and upgradegame brings
 * an older one up to date with DBMigrate().  Migration N turns version
 * N - 1 into version N; an applied migration is never edited, so a
 * schema change is always a new one.
 *
 * The database runs in WAL mode with synchronous=FULL: commits are rare (a
 * player's SAVE, a segment swap, the hourly update), so syncing each one
 * costs little, and a save the game reports has reached the disk.
 *
 * None of these functions creates a directory.  A missing, invalid or
 * unwritable directory is reported and nothing is made; a function that
 * fails part way removes the files it made, and only those.  Every error
 * goes to DBReport(), stderr unless the caller sets it.
 *
 * This file uses no game state, so mkgame and upgradegame link it alone.
 */
#include <errno.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gamedb.h"

#define	FALSE	0
#define	TRUE	1

#define	MaxCached	256		/* statements DBPrepare() can keep */
#define	BusyMs		5000	/* how long to wait for another connection */

static void	ToStderr(const char *Msg);
static void	NoPersistentWAL(sqlite3 *db);

sqlite3		*GameDB;
void		(*DBReport)(const char *Msg) = ToStderr;

static sqlite3_stmt	**Cached[MaxCached];
static int			NCached;
static int			Registered;		/* DBClose() runs at exit */

/* ---------- migrations ---------- */

/*
 * Migration 1: the whole game, empty.  The column comments are in
 * docs/data-files.md.  Mig1Object and Mig1Monster are the object and
 * monster columns shared by several tables; like the migration, they must
 * never change.
 */
#define	Mig1Object \
	"	name			TEXT NOT NULL,\n" \
	"	article			TEXT NOT NULL CHECK (article IN ('a', 'an', 'some', 'the', 'none')),\n" \
	"	weight			INTEGER NOT NULL,\n" \
	"	price			INTEGER NOT NULL,\n" \
	"	desc_code		INTEGER NOT NULL,\n" \
	"	desc_rec		INTEGER NOT NULL,\n" \
	"	carry			INTEGER NOT NULL CHECK (carry IN (0, 1)),\n" \
	"	magic			INTEGER NOT NULL CHECK (magic IN (0, 1)),\n" \
	"	permanent		INTEGER NOT NULL CHECK (permanent IN (0, 1)),\n" \
	"	invisible		INTEGER NOT NULL CHECK (invisible IN (0, 1)),\n" \
	"	cursed			INTEGER NOT NULL CHECK (cursed IN (0, 1)),\n" \
	"	kind			TEXT NOT NULL CHECK (kind IN ('portal', 'treasure', 'weapon', 'armor',\n" \
	"						'shield', 'coins', 'scroll', 'chest', 'door', 'device', 'teleport',\n" \
	"						'keys', 'card', 'ring', 'misc', 'dummy')),\n" \
	"	to_where		INTEGER,\n" \
	"	min_hp			INTEGER,\n" \
	"	max_hp			INTEGER,\n" \
	"	strikes			INTEGER,\n" \
	"	wea_plus		INTEGER,\n" \
	"	weap_type		TEXT CHECK (weap_type IN ('sharp', 'thrust', 'blunt', 'pole')),\n" \
	"	sh_plus			INTEGER,\n" \
	"	sh_hits			INTEGER,\n" \
	"	arm_plus		INTEGER,\n" \
	"	arm_hits		INTEGER,\n" \
	"	multiplier		INTEGER,\n" \
	"	spell			INTEGER,\n" \
	"	closed			INTEGER CHECK (closed IN (0, 1)),\n" \
	"	trap			INTEGER,\n" \
	"	locked			INTEGER,\n" \
	"	num_inside		INTEGER,\n" \
	"	d_to_where		INTEGER,\n" \
	"	d_closed		INTEGER CHECK (d_closed IN (0, 1)),\n" \
	"	d_spring		INTEGER CHECK (d_spring IN (0, 1)),\n" \
	"	d_toll			INTEGER CHECK (d_toll IN (0, 1)),\n" \
	"	d_trap			INTEGER,\n" \
	"	d_locked		INTEGER,\n" \
	"	unlock			INTEGER,\n" \
	"	num_uses		INTEGER,\n" \
	"	m_spell			INTEGER,\n" \
	"	num_charges		INTEGER,\n" \
	"	m_level			INTEGER,\n" \
	"	t_to_where		INTEGER,\n" \
	"	t_active_rm		INTEGER,\n" \
	"	r_spell			INTEGER,\n" \
	"	r_level			INTEGER,\n" \
	"	CHECK ((kind = 'portal') = (to_where IS NOT NULL)),\n" \
	"	CHECK ((kind = 'weapon') = (min_hp IS NOT NULL AND max_hp IS NOT NULL\n" \
	"		AND strikes IS NOT NULL AND wea_plus IS NOT NULL AND weap_type IS NOT NULL)),\n" \
	"	CHECK ((kind = 'shield') = (sh_plus IS NOT NULL AND sh_hits IS NOT NULL)),\n" \
	"	CHECK ((kind = 'armor') = (arm_plus IS NOT NULL AND arm_hits IS NOT NULL)),\n" \
	"	CHECK ((kind = 'coins') = (multiplier IS NOT NULL)),\n" \
	"	CHECK ((kind = 'scroll') = (spell IS NOT NULL)),\n" \
	"	CHECK ((kind = 'chest') = (closed IS NOT NULL AND trap IS NOT NULL\n" \
	"		AND locked IS NOT NULL AND num_inside IS NOT NULL)),\n" \
	"	CHECK ((kind = 'door') = (d_to_where IS NOT NULL AND d_closed IS NOT NULL\n" \
	"		AND d_spring IS NOT NULL AND d_toll IS NOT NULL AND d_trap IS NOT NULL\n" \
	"		AND d_locked IS NOT NULL)),\n" \
	"	CHECK ((kind = 'keys') = (unlock IS NOT NULL AND num_uses IS NOT NULL)),\n" \
	"	CHECK ((kind = 'device') = (m_spell IS NOT NULL AND num_charges IS NOT NULL\n" \
	"		AND m_level IS NOT NULL)),\n" \
	"	CHECK ((kind = 'teleport') = (t_to_where IS NOT NULL AND t_active_rm IS NOT NULL)),\n" \
	"	CHECK ((kind = 'ring') = (r_spell IS NOT NULL AND r_level IS NOT NULL))\n"

#define	Mig1Monster \
	"	name			TEXT NOT NULL,\n" \
	"	article			TEXT NOT NULL CHECK (article IN ('a', 'an', 'some', 'the', 'none')),\n" \
	"	desc_rec		INTEGER NOT NULL,\n" \
	"	desc_code		INTEGER NOT NULL,\n" \
	"	atk_speed		INTEGER NOT NULL,\n" \
	"	ac				INTEGER NOT NULL,\n" \
	"	hits			INTEGER NOT NULL,\n" \
	"	max_hits		INTEGER NOT NULL,\n" \
	"	lvl				INTEGER NOT NULL,\n" \
	"	num				INTEGER NOT NULL,\n" \
	"	m_react			INTEGER NOT NULL,\n" \
	"	m_parley		INTEGER NOT NULL,\n" \
	"	which_obj		INTEGER NOT NULL,\n" \
	"	defend			INTEGER NOT NULL CHECK (defend IN (0, 1)),\n" \
	"	block			INTEGER NOT NULL CHECK (block IN (0, 1)),\n" \
	"	follow			INTEGER NOT NULL CHECK (follow IN (0, 1)),\n" \
	"	guard			INTEGER NOT NULL CHECK (guard IN (0, 1)),\n" \
	"	atk_last_aggr	INTEGER NOT NULL CHECK (atk_last_aggr IN (0, 1)),\n" \
	"	slow_react		INTEGER NOT NULL CHECK (slow_react IN (0, 1)),\n" \
	"	fast_react		INTEGER NOT NULL CHECK (fast_react IN (0, 1)),\n" \
	"	invisible		INTEGER NOT NULL CHECK (invisible IN (0, 1)),\n" \
	"	regenerate		INTEGER NOT NULL CHECK (regenerate IN (0, 1)),\n" \
	"	drain			INTEGER NOT NULL CHECK (drain IN (0, 1)),\n" \
	"	poison			INTEGER NOT NULL CHECK (poison IN (0, 1)),\n" \
	"	anti_magic		INTEGER NOT NULL CHECK (anti_magic IN (0, 1)),\n" \
	"	undead			INTEGER NOT NULL CHECK (undead IN (0, 1)),\n" \
	"	moral_react		INTEGER NOT NULL CHECK (moral_react IN (0, 1)),\n" \
	"	flee			INTEGER NOT NULL CHECK (flee IN (0, 1)),\n" \
	"	assistance		INTEGER NOT NULL CHECK (assistance IN (0, 1)),\n" \
	"	mon_spells		INTEGER NOT NULL CHECK (mon_spells IN (0, 1)),\n" \
	"	top				INTEGER NOT NULL CHECK (top IN (0, 1)),\n" \
	"	permanent		INTEGER NOT NULL CHECK (permanent IN (0, 1)),\n" \
	"	magic			INTEGER NOT NULL CHECK (magic IN (0, 1)),\n" \
	"	summon_help		INTEGER NOT NULL CHECK (summon_help IN (0, 1)),\n" \
	"	immoral_react	INTEGER NOT NULL CHECK (immoral_react IN (0, 1)),\n" \
	"	is_unique		INTEGER NOT NULL CHECK (is_unique IN (0, 1)),\n" \
	"	watch			INTEGER NOT NULL CHECK (watch IN (0, 1)),\n" \
	"	nice			INTEGER NOT NULL CHECK (nice IN (0, 1))\n"

static const char	Migration1[] =
	"CREATE TABLE settings (\n"
	"	id				INTEGER PRIMARY KEY CHECK (id = 1),\n"
	"	notice			TEXT NOT NULL,\n"
	"	task_closed		TEXT NOT NULL CHECK (task_closed IN ('O', 'C', 'T')),\n"
	"	runs			INTEGER NOT NULL,\n"
	"	monster_speed	INTEGER NOT NULL,\n"
	"	start_room		INTEGER NOT NULL,\n"
	"	revive_room		INTEGER NOT NULL,\n"
	"	jail_room		INTEGER NOT NULL\n"
	") STRICT;\n"
	"CREATE TABLE news (\n"
	"	line			INTEGER PRIMARY KEY CHECK (line >= 1),\n"
	"	text			TEXT NOT NULL\n"
	") STRICT;\n"
	"CREATE TABLE obituaries (\n"
	"	rank			INTEGER PRIMARY KEY CHECK (rank >= 1),\n"
	"	lvl				INTEGER NOT NULL,\n"
	"	name			TEXT NOT NULL,\n"
	"	class			TEXT NOT NULL,\n"
	"	text			TEXT NOT NULL\n"
	") STRICT;\n"
	"CREATE TABLE encounters (\n"
	"	encounter		INTEGER NOT NULL CHECK (encounter >= 1),\n"
	"	slot			INTEGER NOT NULL CHECK (slot >= 1),\n"
	"	monster			INTEGER NOT NULL,\n"
	"	PRIMARY KEY (encounter, slot)\n"
	") STRICT, WITHOUT ROWID;\n"
	"CREATE TABLE treasures (\n"
	"	treasure		INTEGER NOT NULL CHECK (treasure >= 1),\n"
	"	slot			INTEGER NOT NULL CHECK (slot >= 1),\n"
	"	object			INTEGER NOT NULL,\n"
	"	PRIMARY KEY (treasure, slot)\n"
	") STRICT, WITHOUT ROWID;\n"
	"CREATE TABLE descriptions (\n"
	"	record			INTEGER NOT NULL CHECK (record >= 0),\n"
	"	line			INTEGER NOT NULL CHECK (line >= 1),\n"
	"	text			TEXT NOT NULL,\n"
	"	PRIMARY KEY (record, line)\n"
	") STRICT, WITHOUT ROWID;\n"
	"CREATE TABLE monster_templates (\n"
	"	number			INTEGER PRIMARY KEY CHECK (number >= 0),\n"
	Mig1Monster
	") STRICT;\n"
	"CREATE TABLE object_templates (\n"
	"	number			INTEGER PRIMARY KEY CHECK (number >= 0),\n"
	Mig1Object
	") STRICT;\n"
	"CREATE TABLE rooms (\n"
	"	room			INTEGER PRIMARY KEY CHECK (room >= 0),\n"
	"	north			INTEGER NOT NULL,\n"
	"	south			INTEGER NOT NULL,\n"
	"	east			INTEGER NOT NULL,\n"
	"	west			INTEGER NOT NULL,\n"
	"	up				INTEGER NOT NULL,\n"
	"	down			INTEGER NOT NULL,\n"
	"	out				INTEGER NOT NULL,\n"
	"	desc_rec		INTEGER NOT NULL,\n"
	"	desc_code		INTEGER NOT NULL,\n"
	"	which_encounter	INTEGER NOT NULL,\n"
	"	encounter_time	INTEGER NOT NULL,\n"
	"	last_dir		INTEGER NOT NULL,\n"
	"	notify_dm		INTEGER NOT NULL CHECK (notify_dm IN (0, 1)),\n"
	"	safe			INTEGER NOT NULL CHECK (safe IN (0, 1)),\n"
	"	anti_magic		INTEGER NOT NULL CHECK (anti_magic IN (0, 1)),\n"
	"	dark			INTEGER NOT NULL CHECK (dark IN (0, 1))\n"
	") STRICT;\n"
	"CREATE TABLE room_specials (\n"
	"	room			INTEGER PRIMARY KEY REFERENCES rooms (room),\n"
	"	role			TEXT NOT NULL CHECK (role IN ('none', 'bar', 'newsstand', 'pawnshop',\n"
	"						'store', 'smith')),\n"
	"	greeting		TEXT\n"
	") STRICT;\n"
	"CREATE TABLE room_stock (\n"
	"	room			INTEGER NOT NULL REFERENCES rooms (room),\n"
	"	slot			INTEGER NOT NULL CHECK (slot >= 1),\n"
	"	object			INTEGER NOT NULL,\n"
	"	PRIMARY KEY (room, slot)\n"
	") STRICT, WITHOUT ROWID;\n"
	"CREATE TABLE room_staggers (\n"
	"	room			INTEGER NOT NULL REFERENCES rooms (room),\n"
	"	slot			INTEGER NOT NULL CHECK (slot >= 1),\n"
	"	to_room			INTEGER NOT NULL,\n"
	"	PRIMARY KEY (room, slot)\n"
	") STRICT, WITHOUT ROWID;\n"
	"CREATE TABLE room_trains (\n"
	"	room			INTEGER NOT NULL REFERENCES rooms (room),\n"
	"	class			TEXT NOT NULL CHECK (class IN ('fighter', 'thief', 'magic-user', 'dm',\n"
	"						'cleric', 'paladin', 'ranger', 'barbarian')),\n"
	"	PRIMARY KEY (room, class)\n"
	") STRICT, WITHOUT ROWID;\n"
	"CREATE TABLE room_objects (\n"
	"	room			INTEGER NOT NULL REFERENCES rooms (room),\n"
	"	slot			INTEGER NOT NULL CHECK (slot >= 1),\n"
	Mig1Object",\n"
	"	PRIMARY KEY (room, slot)\n"
	") STRICT;\n"
	"CREATE TABLE chest_objects (\n"
	"	room			INTEGER NOT NULL,\n"
	"	chest			INTEGER NOT NULL,\n"
	"	slot			INTEGER NOT NULL CHECK (slot >= 1),\n"
	Mig1Object",\n"
	"	PRIMARY KEY (room, chest, slot),\n"
	"	FOREIGN KEY (room, chest) REFERENCES room_objects (room, slot) ON DELETE CASCADE\n"
	") STRICT;\n"
	"CREATE TABLE room_monsters (\n"
	"	room			INTEGER NOT NULL REFERENCES rooms (room),\n"
	"	slot			INTEGER NOT NULL CHECK (slot >= 1),\n"
	Mig1Monster",\n"
	"	PRIMARY KEY (room, slot)\n"
	") STRICT;\n"
	"CREATE TABLE monster_objects (\n"
	"	room			INTEGER NOT NULL,\n"
	"	monster			INTEGER NOT NULL,\n"
	"	slot			INTEGER NOT NULL CHECK (slot >= 1),\n"
	Mig1Object",\n"
	"	PRIMARY KEY (room, monster, slot),\n"
	"	FOREIGN KEY (room, monster) REFERENCES room_monsters (room, slot) ON DELETE CASCADE\n"
	") STRICT;\n"
	"CREATE TABLE players (\n"
	"	id				INTEGER PRIMARY KEY AUTOINCREMENT,\n"
	"	name			TEXT NOT NULL UNIQUE CHECK (name <> '' AND name = upper(name)),\n"
	"	lvl				INTEGER NOT NULL,\n"
	"	class			TEXT NOT NULL CHECK (class IN ('fighter', 'thief', 'magic-user', 'dm',\n"
	"						'cleric', 'paladin', 'ranger', 'barbarian')),\n"
	"	hits			INTEGER NOT NULL,\n"
	"	max_hits		INTEGER NOT NULL,\n"
	"	fatigue			INTEGER NOT NULL,\n"
	"	max_fatigue		INTEGER NOT NULL,\n"
	"	magic			INTEGER NOT NULL,\n"
	"	max_magic		INTEGER NOT NULL,\n"
	"	experience		INTEGER NOT NULL,\n"
	"	room			INTEGER NOT NULL,\n"
	"	str				INTEGER NOT NULL,\n"
	"	int				INTEGER NOT NULL,\n"
	"	dex				INTEGER NOT NULL,\n"
	"	pty				INTEGER NOT NULL,\n"
	"	con				INTEGER NOT NULL,\n"
	"	last_access		INTEGER NOT NULL,\n"
	"	money			INTEGER NOT NULL,\n"
	"	sex				TEXT NOT NULL CHECK (sex IN ('male', 'female')),\n"
	"	s_sharp			INTEGER NOT NULL,\n"
	"	s_thrust		INTEGER NOT NULL,\n"
	"	s_blunt			INTEGER NOT NULL,\n"
	"	s_long			INTEGER NOT NULL,\n"
	"	assoc			INTEGER NOT NULL,\n"
	"	spells			INTEGER NOT NULL,\n"
	"	ssj				INTEGER NOT NULL CHECK (ssj IN (0, 1)),\n"
	"	master			INTEGER NOT NULL CHECK (master IN (0, 1)),\n"
	"	play_tester		INTEGER NOT NULL CHECK (play_tester IN (0, 1)),\n"
	"	a_guild			INTEGER NOT NULL CHECK (a_guild IN (0, 1)),\n"
	"	t_guild			INTEGER NOT NULL CHECK (t_guild IN (0, 1)),\n"
	"	evil			INTEGER NOT NULL CHECK (evil IN (0, 1)),\n"
	"	invisible		INTEGER NOT NULL CHECK (invisible IN (0, 1)),\n"
	"	non_existant	INTEGER NOT NULL CHECK (non_existant IN (0, 1)),\n"
	"	skill_new		INTEGER NOT NULL CHECK (skill_new IN (0, 1)),\n"
	"	spell_new		INTEGER NOT NULL CHECK (spell_new IN (0, 1)),\n"
	"	poisoned		INTEGER NOT NULL CHECK (poisoned IN (0, 1)),\n"
	"	hidden			INTEGER NOT NULL CHECK (hidden IN (0, 1)),\n"
	"	it				INTEGER NOT NULL CHECK (it IN (0, 1)),\n"
	"	suck			INTEGER NOT NULL CHECK (suck IN (0, 1)),\n"
	"	brief			INTEGER NOT NULL CHECK (brief IN (0, 1)),\n"
	"	echo			INTEGER NOT NULL CHECK (echo IN (0, 1)),\n"
	"	mes_block		INTEGER NOT NULL CHECK (mes_block IN (0, 1)),\n"
	"	dead			INTEGER NOT NULL CHECK (dead IN (0, 1)),\n"
	"	plague			INTEGER NOT NULL CHECK (plague IN (0, 1)),\n"
	"	pw_hash			TEXT NOT NULL\n"
	") STRICT;\n"
	"CREATE TABLE player_objects (\n"
	"	player			INTEGER NOT NULL REFERENCES players (id) ON DELETE CASCADE,\n"
	"	slot			INTEGER NOT NULL CHECK (slot >= 1),\n"
	"	worn			TEXT CHECK (worn IN ('weapon', 'armor', 'shield', 'ring-left', 'ring-right')),\n"
	Mig1Object",\n"
	"	PRIMARY KEY (player, slot)\n"
	") STRICT;\n"
	"CREATE UNIQUE INDEX player_worn ON player_objects (player, worn) WHERE worn IS NOT NULL;\n";
static const char	*Migration[DBVersion + 1] = { NULL, Migration1 };

/* ---------- reporting ---------- */

static void ToStderr(const char *Msg)
{
	fprintf(stderr, "%s\n", Msg);
}

static void Report(const char *Fmt, ...)
{
	char	Msg[1024];
	va_list	Args;

	va_start(Args, Fmt);
	vsnprintf(Msg, sizeof(Msg), Fmt, Args);
	va_end(Args);
	DBReport(Msg);
}

/* Report the last error on db, saying what was being done to Path */
static void ReportDB(sqlite3 *db, const char *Path, const char *Doing)
{
	Report("%s: %s: %s", Path, Doing, db ? sqlite3_errmsg(db) : "out of memory");
}

/* ---------- paths ---------- */

/* Dir must exist, be a directory, and be writable; never created here */
static int CheckDir(const char *Dir)
{
	struct stat	st;

	if (stat(Dir, &st))
	{
		Report("%s: %s", Dir, strerror(errno));
		return FALSE;
	}
	if (!S_ISDIR(st.st_mode))
	{
		Report("%s: not a directory", Dir);
		return FALSE;
	}
	if (access(Dir, W_OK | X_OK))
	{
		Report("%s: directory is not writable: %s", Dir, strerror(errno));
		return FALSE;
	}
	return TRUE;
}

/* The directory part of Path, as CheckDir() wants it */
static void DirOf(const char *Path, char *Dir, size_t Len)
{
	const char	*Slash = strrchr(Path, '/');

	if (!Slash)
		snprintf(Dir, Len, ".");
	else
	if (Slash == Path)
		snprintf(Dir, Len, "/");
	else
		snprintf(Dir, Len, "%.*s", (int)(Slash - Path), Path);
}

static int Exists(const char *Path)
{
	struct stat	st;

	return !lstat(Path, &st) || errno != ENOENT;
}

/* Path and its -wal and -shm files must all be absent */
static int Absent(const char *Path)
{
	char	Side[1200];

	if (Exists(Path))
	{
		Report("%s: already exists", Path);
		return FALSE;
	}
	snprintf(Side, sizeof(Side), "%s-wal", Path);
	if (Exists(Side))
	{
		Report("%s: already exists", Side);
		return FALSE;
	}
	snprintf(Side, sizeof(Side), "%s-shm", Path);
	if (Exists(Side))
	{
		Report("%s: already exists", Side);
		return FALSE;
	}
	return TRUE;
}

/* Remove a database file this module made, with its -wal and -shm files */
static void Remove(const char *Path)
{
	char	Side[1200];

	unlink(Path);
	snprintf(Side, sizeof(Side), "%s-wal", Path);
	unlink(Side);
	snprintf(Side, sizeof(Side), "%s-shm", Path);
	unlink(Side);
}

static void GamePath(const char *Dir, char *Path, size_t Len)
{
	snprintf(Path, Len, "%s/%s", Dir, DBName);
}

/* ---------- small helpers ---------- */

static int Exec(sqlite3 *db, const char *Path, const char *Sql)
{
	char	*Err = NULL;

	if (sqlite3_exec(db, Sql, NULL, NULL, &Err) != SQLITE_OK)
	{
		Report("%s: %s", Path, Err ? Err : sqlite3_errmsg(db));
		sqlite3_free(Err);
		return FALSE;
	}
	return TRUE;
}

/* The text of the first row of a one-column query, in Out */
static int Query(sqlite3 *db, const char *Path, const char *Sql, char *Out, size_t Len)
{
	sqlite3_stmt	*St;
	int				Rc;

	*Out = '\0';
	if (sqlite3_prepare_v2(db, Sql, -1, &St, NULL) != SQLITE_OK)
	{
		ReportDB(db, Path, "cannot read");
		return FALSE;
	}
	if ((Rc = sqlite3_step(St)) == SQLITE_ROW)
	{
		const unsigned char	*Text = sqlite3_column_text(St, 0);

		snprintf(Out, Len, "%s", Text ? (const char *)Text : "");
		Rc = sqlite3_step(St);
	}
	if (Rc != SQLITE_DONE)
	{
		ReportDB(db, Path, "cannot read");
		sqlite3_finalize(St);
		return FALSE;
	}
	sqlite3_finalize(St);
	return TRUE;
}

static int UserVersion(sqlite3 *db, const char *Path, int *Version)
{
	char	Text[32];

	if (!Query(db, Path, "PRAGMA user_version", Text, sizeof(Text)))
		return FALSE;
	*Version = atoi(Text);
	return TRUE;
}

/* quick_check and foreign_key_check both find nothing wrong */
static int Healthy(sqlite3 *db, const char *Path)
{
	char	Text[256];

	if (!Query(db, Path, "PRAGMA quick_check", Text, sizeof(Text)))
		return FALSE;
	if (strcmp(Text, "ok"))
	{
		Report("%s: database is damaged: %s", Path, Text);
		return FALSE;
	}
	if (!Query(db, Path, "PRAGMA foreign_key_check", Text, sizeof(Text)))
		return FALSE;
	if (*Text)
	{
		Report("%s: database is damaged: a row in %s refers to a missing row",
			   Path, Text);
		return FALSE;
	}
	return TRUE;
}

/*
 * Open the database at Path, which must exist.  Unless ReadOnly, a file
 * that can only be read is refused.  ReadOnly callers never write, but a
 * writable file is still opened for writing: a read-only connection can't
 * open a WAL database whose -shm file is gone, as it is after a clean
 * close.
 */
static sqlite3 *Open(const char *Path, int ReadOnly)
{
	sqlite3		*db = NULL;
	struct stat	st;
	int			Writable;

	if (stat(Path, &st))
	{
		Report("%s: %s", Path, strerror(errno));
		return NULL;
	}
	if (!S_ISREG(st.st_mode))
	{
		Report("%s: not a file", Path);
		return NULL;
	}
	Writable = !access(Path, W_OK);
	if (!ReadOnly && !Writable)
	{
		Report("%s: database is read-only: %s", Path, strerror(errno));
		return NULL;
	}
	if (sqlite3_open_v2(Path, &db, Writable ? SQLITE_OPEN_READWRITE
						: SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
	{
		ReportDB(db, Path, "cannot open");
		sqlite3_close(db);
		return NULL;
	}
	if (!ReadOnly && sqlite3_db_readonly(db, "main"))
	{
		Report("%s: database is read-only", Path);
		sqlite3_close(db);
		return NULL;
	}
	sqlite3_busy_timeout(db, BusyMs);
	NoPersistentWAL(db);
	return db;
}

/*
 * Remove the -wal and -shm files when the last connection closes, as
 * SQLite does by default; Apple's build keeps them.
 */
static void NoPersistentWAL(sqlite3 *db)
{
	int		Off = 0;

	sqlite3_file_control(db, "main", SQLITE_FCNTL_PERSIST_WAL, &Off);
}

/* Settings every connection that writes needs */
static int Configure(sqlite3 *db, const char *Path)
{
	return Exec(db, Path, "PRAGMA foreign_keys = ON;"
						  "PRAGMA synchronous = FULL;");
}

/* Apply migrations From + 1 .. DBVersion in one transaction */
static int Migrate(sqlite3 *db, const char *Path, int From)
{
	char	Sql[64];
	int		V;

	if (!Exec(db, Path, "BEGIN IMMEDIATE"))
		return FALSE;
	for (V = From + 1; V <= DBVersion; V++)
		if (!Exec(db, Path, Migration[V]))
		{
			Report("%s: migration %d failed; nothing was changed", Path, V);
			Exec(db, Path, "ROLLBACK");
			return FALSE;
		}
	snprintf(Sql, sizeof(Sql), "PRAGMA user_version = %d", DBVersion);
	if (!Exec(db, Path, Sql) || !Exec(db, Path, "COMMIT"))
	{
		Exec(db, Path, "ROLLBACK");
		return FALSE;
	}
	return TRUE;
}

/* Copy database From (open) to the new file To, which this removes on failure */
static int Copy(sqlite3 *From, const char *FromPath, const char *To)
{
	sqlite3			*Dest = NULL;
	sqlite3_backup	*Bk;
	int				Rc;

	if (sqlite3_open_v2(To, &Dest, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
						NULL) != SQLITE_OK)
	{
		ReportDB(Dest, To, "cannot create");
		sqlite3_close(Dest);
		Remove(To);
		return FALSE;
	}
	NoPersistentWAL(Dest);
	if (!(Bk = sqlite3_backup_init(Dest, "main", From, "main")))
	{
		ReportDB(Dest, To, "cannot copy");
		sqlite3_close(Dest);
		Remove(To);
		return FALSE;
	}
	Rc = sqlite3_backup_step(Bk, -1);
	sqlite3_backup_finish(Bk);
	if (Rc != SQLITE_DONE)
	{
		Report("%s: cannot copy %s: %s", To, FromPath, sqlite3_errstr(Rc));
		sqlite3_close(Dest);
		Remove(To);
		return FALSE;
	}
	if (!Exec(Dest, To, "PRAGMA journal_mode = WAL") || sqlite3_close(Dest) != SQLITE_OK)
	{
		Report("%s: cannot finish the copy", To);
		Remove(To);
		return FALSE;
	}
	return TRUE;
}

/* ---------- the module's functions ---------- */

/*
 * Make an empty game database at the current version in directory Dir,
 * which must already exist; Dir/game.db must not.
 */
int DBCreate(const char *Dir)
{
	sqlite3	*db = NULL;
	char	Path[1100], Mode[16];

	if (!CheckDir(Dir))
		return FALSE;
	GamePath(Dir, Path, sizeof(Path));
	if (!Absent(Path))
		return FALSE;
	if (sqlite3_open_v2(Path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
						NULL) != SQLITE_OK)
	{
		ReportDB(db, Path, "cannot create");
		sqlite3_close(db);
		Remove(Path);
		return FALSE;
	}
	NoPersistentWAL(db);
	if (Query(db, Path, "PRAGMA journal_mode = WAL", Mode, sizeof(Mode))
	&&	strcmp(Mode, "wal"))
	{
		Report("%s: cannot use WAL mode", Path);
		*Mode = '\0';
	}
	if (strcmp(Mode, "wal") || !Configure(db, Path) || !Migrate(db, Path, 0))
	{
		sqlite3_close(db);
		Remove(Path);
		return FALSE;
	}
	if (sqlite3_close(db) != SQLITE_OK)
	{
		Report("%s: cannot close", Path);
		Remove(Path);
		return FALSE;
	}
	return TRUE;
}

/*
 * Open Dir/game.db as GameDB, for the game.  A database that is missing,
 * read-only, damaged, not in WAL mode or of another version is refused.
 */
int DBOpen(const char *Dir)
{
	sqlite3	*db;
	char	Path[1100], Mode[16];
	int		Version;

	if (!CheckDir(Dir))
		return FALSE;
	GamePath(Dir, Path, sizeof(Path));
	if (!(db = Open(Path, FALSE)))
		return FALSE;
	if (!UserVersion(db, Path, &Version))
	{
		sqlite3_close(db);
		return FALSE;
	}
	if (Version != DBVersion)
	{
		Report(Version < DBVersion
			   ? "%s: schema version %d; this build reads version %d, so run"
				 " upgradegame on the game directory"
			   : "%s: schema version %d; this build reads version %d, so it"
				 " needs a newer scepterd", Path, Version, DBVersion);
		sqlite3_close(db);
		return FALSE;
	}
	if (Query(db, Path, "PRAGMA journal_mode", Mode, sizeof(Mode))
	&&	strcmp(Mode, "wal"))
	{
		Report("%s: not in WAL mode", Path);
		*Mode = '\0';
	}
	if (strcmp(Mode, "wal") || !Configure(db, Path) || !Healthy(db, Path))
	{
		sqlite3_close(db);
		return FALSE;
	}
	DBClose();
	GameDB = db;
	if (!Registered)
	{
		atexit(DBClose);	/* a clean close removes the -wal and -shm files */
		Registered = TRUE;
	}
	return TRUE;
}

/* Close GameDB, finalizing every statement DBPrepare() kept */
void DBClose(void)
{
	int		I;

	for (I = 0; I < NCached; I++)
	{
		sqlite3_finalize(*Cached[I]);
		*Cached[I] = NULL;
	}
	NCached = 0;
	if (GameDB && sqlite3_close(GameDB) != SQLITE_OK)
		ReportDB(GameDB, DBName, "cannot close");
	GameDB = NULL;
}

/* The schema version of Dir/game.db, read without changing anything */
int DBVersionOf(const char *Dir, int *Version)
{
	sqlite3	*db;
	char	Path[1100];
	int		Ok;

	GamePath(Dir, Path, sizeof(Path));
	if (!(db = Open(Path, TRUE)))
		return FALSE;
	Ok = UserVersion(db, Path, Version);
	sqlite3_close(db);
	return Ok;
}

/*
 * Copy Dir/game.db to To, a new file in a directory that already exists.
 * The copy is consistent even while the server is running.
 */
int DBBackup(const char *Dir, const char *To)
{
	sqlite3	*db;
	char	Path[1100], ToDir[1100];
	int		Ok;

	DirOf(To, ToDir, sizeof(ToDir));
	if (!CheckDir(ToDir) || !Absent(To))
		return FALSE;
	GamePath(Dir, Path, sizeof(Path));
	if (!(db = Open(Path, TRUE)))
		return FALSE;
	Ok = Copy(db, Path, To);
	sqlite3_close(db);
	return Ok;
}

/*
 * Replace Dir/game.db with a copy of the backup From, with the server
 * stopped.  From must be a sound game database this build can read or
 * upgrade.  Players saved for the first time since the backup lose their
 * ids, which new players may then be given again.
 */
int DBRestore(const char *From, const char *Dir)
{
	sqlite3	*db;
	char	Path[1100], Tmp[1200], Side[1200];
	int		Version;

	if (!CheckDir(Dir))
		return FALSE;
	if (!(db = Open(From, TRUE)))
		return FALSE;
	if (!UserVersion(db, From, &Version) || !Healthy(db, From))
	{
		sqlite3_close(db);
		return FALSE;
	}
	if (Version < 1 || Version > DBVersion)
	{
		Report("%s: schema version %d; this build can restore versions 1 to %d",
			   From, Version, DBVersion);
		sqlite3_close(db);
		return FALSE;
	}
	GamePath(Dir, Path, sizeof(Path));
	snprintf(Tmp, sizeof(Tmp), "%s.restore", Path);
	if (!Absent(Tmp) || !Copy(db, From, Tmp))
	{
		sqlite3_close(db);
		return FALSE;
	}
	sqlite3_close(db);

	/* the old database's log must not be applied to the restored one */
	snprintf(Side, sizeof(Side), "%s-wal", Path);
	if (unlink(Side) && errno != ENOENT)
	{
		Report("%s: %s", Side, strerror(errno));
		Remove(Tmp);
		return FALSE;
	}
	snprintf(Side, sizeof(Side), "%s-shm", Path);
	unlink(Side);
	if (rename(Tmp, Path))
	{
		Report("%s: cannot replace: %s", Path, strerror(errno));
		Remove(Tmp);
		return FALSE;
	}
	return TRUE;
}

/*
 * Bring Dir/game.db up to DBVersion: back it up to
 * Dir/game.db.backup-YYYYMMDD-HHMMSS, apply the missing migrations in one
 * transaction, and check the result.  DryRun only says what would happen.
 */
int DBMigrate(const char *Dir, int DryRun)
{
	sqlite3	*db;
	char	Path[1100], To[1200], Stamp[32];
	time_t	Now = time(NULL);
	int		Version;

	if (!CheckDir(Dir))
		return FALSE;
	GamePath(Dir, Path, sizeof(Path));
	if (!(db = Open(Path, DryRun)))
		return FALSE;
	if (!UserVersion(db, Path, &Version))
	{
		sqlite3_close(db);
		return FALSE;
	}
	if (Version > DBVersion)
	{
		Report("%s: schema version %d is newer than this build's %d", Path,
			   Version, DBVersion);
		sqlite3_close(db);
		return FALSE;
	}
	if (Version == DBVersion)
	{
		printf("%s: already at schema version %d\n", Path, Version);
		sqlite3_close(db);
		return TRUE;
	}
	if (DryRun)
	{
		printf("%s: schema version %d, would migrate to %d\n", Path, Version,
			   DBVersion);
		sqlite3_close(db);
		return TRUE;
	}

	strftime(Stamp, sizeof(Stamp), "%Y%m%d-%H%M%S", localtime(&Now));
	snprintf(To, sizeof(To), "%s.backup-%s", Path, Stamp);
	if (!Absent(To) || !Copy(db, Path, To))
	{
		sqlite3_close(db);
		return FALSE;
	}
	printf("%s: backed up to %s\n", Path, To);
	if (!Configure(db, Path) || !Migrate(db, Path, Version) || !Healthy(db, Path))
	{
		sqlite3_close(db);
		return FALSE;
	}
	sqlite3_close(db);
	printf("%s: migrated from schema version %d to %d\n", Path, Version, DBVersion);
	return TRUE;
}

int DBBegin(void)
{
	return Exec(GameDB, DBName, "BEGIN IMMEDIATE");
}

int DBCommit(void)
{
	return Exec(GameDB, DBName, "COMMIT");
}

void DBRollback(void)
{
	if (GameDB && !sqlite3_get_autocommit(GameDB))
		Exec(GameDB, DBName, "ROLLBACK");
}

/*
 * The statement Sql on GameDB, ready for its parameters.  It is prepared
 * once, into *Cache, and reset each time after that; DBClose() frees it.
 */
sqlite3_stmt *DBPrepare(sqlite3_stmt **Cache, const char *Sql)
{
	if (*Cache)
	{
		sqlite3_reset(*Cache);
		sqlite3_clear_bindings(*Cache);
		return *Cache;
	}
	if (NCached == MaxCached)
	{
		Report("%s: too many prepared statements", DBName);
		return NULL;
	}
	if (sqlite3_prepare_v3(GameDB, Sql, -1, SQLITE_PREPARE_PERSISTENT, Cache,
						   NULL) != SQLITE_OK)
	{
		ReportDB(GameDB, DBName, "cannot prepare a statement");
		return NULL;
	}
	Cached[NCached++] = Cache;
	return *Cache;
}
