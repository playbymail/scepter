# Game data files

`mkgame` writes a game directory; `scepterd` reads and updates it.  Every
file sits directly in the game directory, which has no subdirectories.
The game's state is in one SQLite database, `game.db` (#71), which
replaced the raw struct files of the QNX version (#5).

## Files

| File | Holds |
|---|---|
| `game.db` | the game database (see below): the whole game |
| `errlog` | the server's error log (text), made when something is first logged |

While the server runs, SQLite keeps `game.db-wal` and `game.db-shm` beside
the database; a clean stop removes them.

## The game database

`game.db` is a SQLite database in WAL mode (`gamedb.c`).  Its schema
version is `PRAGMA user_version`; this build reads version `DBVersion`
(`gamedb.h`), currently 4.

- **`mkgame`** creates it with the whole schema and fills it from the
  world: templates, descriptions, rooms and their contents, settings,
  tables and special rooms; `players` starts empty.  The game directory
  must already exist and be empty.
- **Rooms:** `rooms` holds every room of every segment, 0 to
  `NumSegs * 10 - 1`, and the server counts the segments from it; a gap
  is refused at start-up.  A segment swap saves its ten rooms in one
  transaction, and the hourly update saves every loaded segment in one.
- **Room contents:** each save replaces all of a room's objects (with
  their chests' contents) or all its monsters (with their inventories).
  A chest's contents are keyed by the chest's slot in the room and go
  with it.  The engine keeps at most 8 things in each list; chests inside
  chests, and what a monster's chests hold, aren't saved (#44).  This
  replaces the 1009-slot hash tables of `dbobj` and `dbmon` (#64).
- **Login limits:** `login_hosts` and `login_names` hold the counts behind
  the limits on wrong passwords (docs/running.md, #93).  A row is deleted
  once it can no longer matter: an address a day after its last refusal
  (and 10 minutes after its last failure), a character an hour after its
  last failure.
- **The world:** `world` holds the id, name and revision of the world the
  game was made from, and `world_history` a row for each time it was made,
  updated or adopted (#138; updating is #137).  A game made before
  migration 4 has no `world` row: `scepterd` logs "world unknown" at
  start-up, and `*WORLD` says so.
- **DM edits:** the world data an update replaces (rooms, descriptions,
  monster and object templates, encounter and treasure table rows) has
  `created_at` and `updated_at`.  `mkgame` sets both to the time in its
  `made` row.  The game's own saves leave them alone; the DM editor moves
  `updated_at` forward whenever it changes a row, always past the last make
  or update.  So the rows whose `updated_at` is later than the last
  `world_history` time are exactly what DMs have changed since; `*WORLD`
  lists them.  Room contents, the notice, news and other settings aren't
  marked.  Rows from before migration 4 have 0 for both: unknown.
- **DM ranks:** `rank_changes` records every change of a character's DM
  rank (#139): when, by whom, and whether in the game's editor or with
  `dmadmin` (docs/running.md).  A row names the character, so it outlives
  a character that is removed.  The rank itself is the `ssj`, `assoc` and
  `master` columns of `players` (see below).
- **Characters:** `players` has one row per saved character, named in
  capitals as `CapAlfa()` folds it, and `player_objects` its first 10
  objects, in the order they were carried (#155), with what it wears.
  `id` is the character's seniority: it is given when the character is
  first saved and never reused, even after a delete; `USERS` lists by it.
  A save is one transaction.  A rename keeps the id, and a name another
  character has is refused (#164).
- **Settings:** the notice, news, obituaries, task state, run counter and
  monster speed are saved whenever a DM or the game changes them, and an
  `ELIST` or `TLIST` edit saves its row.  The hourly update (`UpdateF`)
  saves every loaded segment of rooms, the settings and both tables in one
  transaction, so a failure leaves the last whole update (#17).  The
  special rooms are written only by `mkgame`.
- **Rows:** `dbstore.c` turns each saved struct into a row and back.  The
  DM editor's changes to a template or a description are one transaction
  each.
- **`scepterd`**, `fightsim`, `dmtest` and `dumpgame` refuse a game whose
  database is missing, read-only, not a SQLite database, damaged
  (`PRAGMA quick_check`), not in WAL mode, or of another version.  An older
  version says to run `upgradegame`; a newer one says a newer `scepterd`
  is needed.
- **Settings:** every connection sets `foreign_keys=ON`, a 5-second busy
  timeout and `synchronous=FULL`.  Commits are rare (a player's `SAVE`, a
  segment swap, the hourly update), so syncing each one costs little, and a
  save the game reports has reached the disk.
- **Paths:** no database function creates a directory.  A missing,
  invalid or unwritable directory is an error, and a function that fails
  part way removes only the files it made.
- **Backups** (`DBBackup`) go to a new file in a directory that already
  exists, and are consistent even while the server runs.  `DBRestore`
  replaces `game.db` with a backup while the server is stopped.  Players
  first saved after the backup are lost with it, and their ids may be
  given out again.
- **Migrations** (`DBMigrate`, for `upgradegame` from #153) back up to
  `game.db.backup-YYYYMMDD-HHMMSS` in the game directory, apply the missing
  migrations in one transaction, and check the result.  An applied
  migration is never edited; every schema change is a new one.
- **Invalid data is refused.**  The tables are `STRICT`, and `CHECK`s
  limit flags to 0 and 1, enums to their names, and an object kind's own
  fields to objects of that kind.  A save that breaks them fails, and the
  server reports it and stops rather than store it (#71).
- **Not stored:** pointers, fields that matter only during play, the old
  password hash `PW`, and `MonsterType`'s unused `Time` and `Filler`.  The
  engine's limits (8 things per container, 10 objects per character, the
  number of segments) are the engine's, not the schema's.

Migration 1, with each column's struct field.  `<object columns>` and
`<monster columns>` mark where the tables after `object_templates` and
`monster_templates` repeat those tables' columns, from `name` on.

```sql
-- Migration 1: the whole game.  PRAGMA user_version = 1 afterwards.

-- ---------- settings (once efile) and the start/revive/jail rooms (once special) ----------

CREATE TABLE settings (
	id				INTEGER PRIMARY KEY CHECK (id = 1),	-- one row
	notice			TEXT NOT NULL,		-- Notice
	task_closed		TEXT NOT NULL CHECK (task_closed IN ('O', 'C', 'T')),	-- TaskClosed: open, closed, testers only
	runs			INTEGER NOT NULL,	-- NumRun
	monster_speed	INTEGER NOT NULL,	-- MSpeed
	start_room		INTEGER NOT NULL,	-- StartRoom
	revive_room		INTEGER NOT NULL,	-- ReviveRoom
	jail_room		INTEGER NOT NULL	-- JailRoom
) STRICT;

CREATE TABLE news (
	line			INTEGER PRIMARY KEY CHECK (line >= 1),	-- NewsBuf[line - 1]
	text			TEXT NOT NULL
) STRICT;

CREATE TABLE obituaries (
	rank			INTEGER PRIMARY KEY CHECK (rank >= 1),	-- index + 1
	lvl				INTEGER NOT NULL,	-- TLvl
	name			TEXT NOT NULL,		-- TName
	class			TEXT NOT NULL,		-- TClass (free text, as shown)
	text			TEXT NOT NULL		-- TBuf
) STRICT;

-- EncIndex(encounter, slot - 1) and ObjIndex(treasure, slot - 1).  A missing row is 0.
CREATE TABLE encounters (
	encounter		INTEGER NOT NULL CHECK (encounter >= 1),
	slot			INTEGER NOT NULL CHECK (slot >= 1),
	monster			INTEGER NOT NULL,	-- MList template number
	PRIMARY KEY (encounter, slot)
) STRICT, WITHOUT ROWID;

CREATE TABLE treasures (
	treasure		INTEGER NOT NULL CHECK (treasure >= 1),
	slot			INTEGER NOT NULL CHECK (slot >= 1),
	object			INTEGER NOT NULL,	-- OList template number
	PRIMARY KEY (treasure, slot)
) STRICT, WITHOUT ROWID;

-- ---------- descriptions (dscrpt) ----------

-- Record N exists when its lines do; mkgame writes all StrPerSeg lines of each record, '' when empty.
CREATE TABLE descriptions (
	record			INTEGER NOT NULL CHECK (record >= 0),
	line			INTEGER NOT NULL CHECK (line >= 1),	-- DescBuf[line - 1]
	text			TEXT NOT NULL,		-- up to the first NUL, at most 80 bytes
	PRIMARY KEY (record, line)
) STRICT, WITHOUT ROWID;

-- ---------- monster and object templates (mlist, olist) ----------

CREATE TABLE monster_templates (
	number			INTEGER PRIMARY KEY CHECK (number >= 0),
	-- <monster columns>
	name			TEXT NOT NULL,		-- Name
	article			TEXT NOT NULL CHECK (article IN ('a', 'an', 'some', 'the', 'none')),
	desc_rec		INTEGER NOT NULL,	-- DescRec
	desc_code		INTEGER NOT NULL,	-- DescCode
	atk_speed		INTEGER NOT NULL,	-- AtkSpeed
	ac				INTEGER NOT NULL,	-- AC
	hits			INTEGER NOT NULL,	-- Hits
	max_hits		INTEGER NOT NULL,	-- MaxHits
	lvl				INTEGER NOT NULL,	-- Lvl
	num				INTEGER NOT NULL,	-- Num
	m_react			INTEGER NOT NULL,	-- MReact
	m_parley		INTEGER NOT NULL,	-- MParley
	which_obj		INTEGER NOT NULL,	-- WhichObj: treasure table row
	defend			INTEGER NOT NULL CHECK (defend IN (0, 1)),
	block			INTEGER NOT NULL CHECK (block IN (0, 1)),
	follow			INTEGER NOT NULL CHECK (follow IN (0, 1)),
	guard			INTEGER NOT NULL CHECK (guard IN (0, 1)),
	atk_last_aggr	INTEGER NOT NULL CHECK (atk_last_aggr IN (0, 1)),
	slow_react		INTEGER NOT NULL CHECK (slow_react IN (0, 1)),
	fast_react		INTEGER NOT NULL CHECK (fast_react IN (0, 1)),
	invisible		INTEGER NOT NULL CHECK (invisible IN (0, 1)),
	regenerate		INTEGER NOT NULL CHECK (regenerate IN (0, 1)),
	drain			INTEGER NOT NULL CHECK (drain IN (0, 1)),
	poison			INTEGER NOT NULL CHECK (poison IN (0, 1)),
	anti_magic		INTEGER NOT NULL CHECK (anti_magic IN (0, 1)),
	undead			INTEGER NOT NULL CHECK (undead IN (0, 1)),
	moral_react		INTEGER NOT NULL CHECK (moral_react IN (0, 1)),
	flee			INTEGER NOT NULL CHECK (flee IN (0, 1)),
	assistance		INTEGER NOT NULL CHECK (assistance IN (0, 1)),
	mon_spells		INTEGER NOT NULL CHECK (mon_spells IN (0, 1)),
	top				INTEGER NOT NULL CHECK (top IN (0, 1)),
	permanent		INTEGER NOT NULL CHECK (permanent IN (0, 1)),
	magic			INTEGER NOT NULL CHECK (magic IN (0, 1)),
	summon_help		INTEGER NOT NULL CHECK (summon_help IN (0, 1)),
	immoral_react	INTEGER NOT NULL CHECK (immoral_react IN (0, 1)),
	is_unique		INTEGER NOT NULL CHECK (is_unique IN (0, 1)),	-- Unique (a keyword in SQL)
	watch			INTEGER NOT NULL CHECK (watch IN (0, 1)),
	nice			INTEGER NOT NULL CHECK (nice IN (0, 1))
	-- </monster columns>
) STRICT;

CREATE TABLE object_templates (
	number			INTEGER PRIMARY KEY CHECK (number >= 0),
	-- <object columns>
	name			TEXT NOT NULL,		-- Name
	article			TEXT NOT NULL CHECK (article IN ('a', 'an', 'some', 'the', 'none')),
	weight			INTEGER NOT NULL,
	price			INTEGER NOT NULL,
	desc_code		INTEGER NOT NULL,	-- DescCode
	desc_rec		INTEGER NOT NULL,	-- DescRec
	carry			INTEGER NOT NULL CHECK (carry IN (0, 1)),
	magic			INTEGER NOT NULL CHECK (magic IN (0, 1)),
	permanent		INTEGER NOT NULL CHECK (permanent IN (0, 1)),
	invisible		INTEGER NOT NULL CHECK (invisible IN (0, 1)),
	cursed			INTEGER NOT NULL CHECK (cursed IN (0, 1)),
	kind			TEXT NOT NULL CHECK (kind IN ('portal', 'treasure', 'weapon', 'armor',
						'shield', 'coins', 'scroll', 'chest', 'door', 'device', 'teleport',
						'keys', 'card', 'ring', 'misc', 'dummy')),	-- ObClass
	-- the kind's own fields (union tObject): set for that kind, NULL for every other
	to_where		INTEGER,			-- Portal.ToWhere
	min_hp			INTEGER,			-- Weap.MinHp
	max_hp			INTEGER,			-- Weap.MaxHp
	strikes			INTEGER,			-- Weap.Strikes
	wea_plus		INTEGER,			-- Weap.WeaPlus
	weap_type		TEXT CHECK (weap_type IN ('sharp', 'thrust', 'blunt', 'pole')),	-- Weap.WeapType
	sh_plus			INTEGER,			-- Shield.ShPlus
	sh_hits			INTEGER,			-- Shield.ShHits
	arm_plus		INTEGER,			-- Armor.ArmPlus
	arm_hits		INTEGER,			-- Armor.ArmHits
	multiplier		INTEGER,			-- Coins.Multiplier
	spell			INTEGER,			-- Scroll.Spell
	closed			INTEGER CHECK (closed IN (0, 1)),	-- Chest.Closed
	trap			INTEGER,			-- Chest.Trap
	locked			INTEGER,			-- Chest.Locked
	num_inside		INTEGER,			-- Chest.NumInside
	d_to_where		INTEGER,			-- Door.DToWhere
	d_closed		INTEGER CHECK (d_closed IN (0, 1)),	-- Door.DClosed
	d_spring		INTEGER CHECK (d_spring IN (0, 1)),	-- Door.DSpring
	d_toll			INTEGER CHECK (d_toll IN (0, 1)),	-- Door.DToll
	d_trap			INTEGER,			-- Door.DTrap
	d_locked		INTEGER,			-- Door.DLocked
	unlock			INTEGER,			-- Keys.UnLock
	num_uses		INTEGER,			-- Keys.NumUses
	m_spell			INTEGER,			-- MagDevice.MSpell
	num_charges		INTEGER,			-- MagDevice.NumCharges
	m_level			INTEGER,			-- MagDevice.MLevel
	t_to_where		INTEGER,			-- Teleport.TToWhere
	t_active_rm		INTEGER,			-- Teleport.TActiveRm
	r_spell			INTEGER,			-- Ring.RSpell
	r_level			INTEGER,			-- Ring.RLevel
	CHECK ((kind = 'portal') = (to_where IS NOT NULL)),
	CHECK ((kind = 'weapon') = (min_hp IS NOT NULL)),
	CHECK ((kind = 'weapon') = (max_hp IS NOT NULL)),
	CHECK ((kind = 'weapon') = (strikes IS NOT NULL)),
	CHECK ((kind = 'weapon') = (wea_plus IS NOT NULL)),
	CHECK ((kind = 'weapon') = (weap_type IS NOT NULL)),
	CHECK ((kind = 'shield') = (sh_plus IS NOT NULL)),
	CHECK ((kind = 'shield') = (sh_hits IS NOT NULL)),
	CHECK ((kind = 'armor') = (arm_plus IS NOT NULL)),
	CHECK ((kind = 'armor') = (arm_hits IS NOT NULL)),
	CHECK ((kind = 'coins') = (multiplier IS NOT NULL)),
	CHECK ((kind = 'scroll') = (spell IS NOT NULL)),
	CHECK ((kind = 'chest') = (closed IS NOT NULL)),
	CHECK ((kind = 'chest') = (trap IS NOT NULL)),
	CHECK ((kind = 'chest') = (locked IS NOT NULL)),
	CHECK ((kind = 'chest') = (num_inside IS NOT NULL)),
	CHECK ((kind = 'door') = (d_to_where IS NOT NULL)),
	CHECK ((kind = 'door') = (d_closed IS NOT NULL)),
	CHECK ((kind = 'door') = (d_spring IS NOT NULL)),
	CHECK ((kind = 'door') = (d_toll IS NOT NULL)),
	CHECK ((kind = 'door') = (d_trap IS NOT NULL)),
	CHECK ((kind = 'door') = (d_locked IS NOT NULL)),
	CHECK ((kind = 'keys') = (unlock IS NOT NULL)),
	CHECK ((kind = 'keys') = (num_uses IS NOT NULL)),
	CHECK ((kind = 'device') = (m_spell IS NOT NULL)),
	CHECK ((kind = 'device') = (num_charges IS NOT NULL)),
	CHECK ((kind = 'device') = (m_level IS NOT NULL)),
	CHECK ((kind = 'teleport') = (t_to_where IS NOT NULL)),
	CHECK ((kind = 'teleport') = (t_active_rm IS NOT NULL)),
	CHECK ((kind = 'ring') = (r_spell IS NOT NULL)),
	CHECK ((kind = 'ring') = (r_level IS NOT NULL))
	-- </object columns>
) STRICT;

-- ---------- rooms (edungeon) ----------

-- Every room of every segment, 0 to NumSegs * 10 - 1, empty ones included.
CREATE TABLE rooms (
	room			INTEGER PRIMARY KEY CHECK (room >= 0),
	north			INTEGER NOT NULL,	-- Adjoin[0]; 0: no exit
	south			INTEGER NOT NULL,	-- Adjoin[1]
	east			INTEGER NOT NULL,	-- Adjoin[2]
	west			INTEGER NOT NULL,	-- Adjoin[3]
	up				INTEGER NOT NULL,	-- Adjoin[4]
	down			INTEGER NOT NULL,	-- Adjoin[5]
	out				INTEGER NOT NULL,	-- Out
	desc_rec		INTEGER NOT NULL,	-- DescRec
	desc_code		INTEGER NOT NULL,	-- DescCode
	which_encounter	INTEGER NOT NULL,	-- WhichEncounter: encounter table row
	encounter_time	INTEGER NOT NULL,	-- EncounterTime
	last_dir		INTEGER NOT NULL,	-- LastDir
	notify_dm		INTEGER NOT NULL CHECK (notify_dm IN (0, 1)),
	safe			INTEGER NOT NULL CHECK (safe IN (0, 1)),
	anti_magic		INTEGER NOT NULL CHECK (anti_magic IN (0, 1)),
	dark			INTEGER NOT NULL CHECK (dark IN (0, 1))
) STRICT;

-- Each room's role and greeting (once special); a room with neither has no row.
CREATE TABLE room_specials (
	room			INTEGER PRIMARY KEY REFERENCES rooms (room),
	role			TEXT NOT NULL CHECK (role IN ('none', 'bar', 'newsstand', 'pawnshop',
						'store', 'smith')),
	greeting		TEXT				-- NULL: none
) STRICT;

CREATE TABLE room_stock (
	room			INTEGER NOT NULL REFERENCES rooms (room),
	slot			INTEGER NOT NULL CHECK (slot >= 1),	-- StockItem(room, slot)
	object			INTEGER NOT NULL,	-- OList template number
	PRIMARY KEY (room, slot)
) STRICT, WITHOUT ROWID;

CREATE TABLE room_staggers (
	room			INTEGER NOT NULL REFERENCES rooms (room),
	slot			INTEGER NOT NULL CHECK (slot >= 1),	-- StaggerItem(room, slot)
	to_room			INTEGER NOT NULL,
	PRIMARY KEY (room, slot)
) STRICT, WITHOUT ROWID;

CREATE TABLE room_trains (
	room			INTEGER NOT NULL REFERENCES rooms (room),
	class			TEXT NOT NULL CHECK (class IN ('fighter', 'thief', 'magic-user', 'dm',
						'cleric', 'paladin', 'ranger', 'barbarian')),
	PRIMARY KEY (room, class)
) STRICT, WITHOUT ROWID;

-- ---------- room contents (dbobj, dbmon) ----------

-- Each save of a room deletes its rows and inserts them again, in one transaction.
CREATE TABLE room_objects (
	room			INTEGER NOT NULL REFERENCES rooms (room),
	slot			INTEGER NOT NULL CHECK (slot >= 1),	-- place in RmObjectTail's list
	-- <object columns>
	PRIMARY KEY (room, slot)
) STRICT;

CREATE TABLE chest_objects (
	room			INTEGER NOT NULL,
	chest			INTEGER NOT NULL,	-- the chest's slot in room_objects
	slot			INTEGER NOT NULL CHECK (slot >= 1),	-- place in Chest.ObjectTail's list
	-- <object columns>
	PRIMARY KEY (room, chest, slot),
	FOREIGN KEY (room, chest) REFERENCES room_objects (room, slot) ON DELETE CASCADE
) STRICT;

CREATE TABLE room_monsters (
	room			INTEGER NOT NULL REFERENCES rooms (room),
	slot			INTEGER NOT NULL CHECK (slot >= 1),	-- place in RmMonsterTail's list
	-- <monster columns>
	PRIMARY KEY (room, slot)
) STRICT;

CREATE TABLE monster_objects (
	room			INTEGER NOT NULL,
	monster			INTEGER NOT NULL,	-- the monster's slot in room_monsters
	slot			INTEGER NOT NULL CHECK (slot >= 1),	-- place in its ObjectTail's list
	-- <object columns>
	PRIMARY KEY (room, monster, slot),
	FOREIGN KEY (room, monster) REFERENCES room_monsters (room, slot) ON DELETE CASCADE
) STRICT;

-- ---------- characters (eperson) ----------

CREATE TABLE players (
	id				INTEGER PRIMARY KEY AUTOINCREMENT,	-- seniority: never reused
	name			TEXT NOT NULL UNIQUE CHECK (name <> '' AND name = upper(name)),	-- Name, folded by CapAlfa
	lvl				INTEGER NOT NULL,
	class			TEXT NOT NULL CHECK (class IN ('fighter', 'thief', 'magic-user', 'dm',
						'cleric', 'paladin', 'ranger', 'barbarian')),	-- Class
	hits			INTEGER NOT NULL,
	max_hits		INTEGER NOT NULL,
	fatigue			INTEGER NOT NULL,
	max_fatigue		INTEGER NOT NULL,
	magic			INTEGER NOT NULL,
	max_magic		INTEGER NOT NULL,
	experience		INTEGER NOT NULL,
	room			INTEGER NOT NULL,	-- RmCode, a room number
	str				INTEGER NOT NULL,
	int				INTEGER NOT NULL,
	dex				INTEGER NOT NULL,
	pty				INTEGER NOT NULL,
	con				INTEGER NOT NULL,
	last_access		INTEGER NOT NULL,	-- LastAccess, packed month << 7 | day
	money			INTEGER NOT NULL,
	sex				TEXT NOT NULL CHECK (sex IN ('male', 'female')),
	s_sharp			INTEGER NOT NULL,	-- SSharp
	s_thrust		INTEGER NOT NULL,	-- SThrust
	s_blunt			INTEGER NOT NULL,	-- SBlunt
	s_long			INTEGER NOT NULL,	-- SLong (pole)
	assoc			INTEGER NOT NULL,	-- Assoc
	spells			INTEGER NOT NULL,	-- Spells, a bit mask
	ssj				INTEGER NOT NULL CHECK (ssj IN (0, 1)),
	master			INTEGER NOT NULL CHECK (master IN (0, 1)),
	play_tester		INTEGER NOT NULL CHECK (play_tester IN (0, 1)),
	a_guild			INTEGER NOT NULL CHECK (a_guild IN (0, 1)),
	t_guild			INTEGER NOT NULL CHECK (t_guild IN (0, 1)),
	evil			INTEGER NOT NULL CHECK (evil IN (0, 1)),
	invisible		INTEGER NOT NULL CHECK (invisible IN (0, 1)),
	non_existant	INTEGER NOT NULL CHECK (non_existant IN (0, 1)),
	skill_new		INTEGER NOT NULL CHECK (skill_new IN (0, 1)),
	spell_new		INTEGER NOT NULL CHECK (spell_new IN (0, 1)),
	poisoned		INTEGER NOT NULL CHECK (poisoned IN (0, 1)),
	hidden			INTEGER NOT NULL CHECK (hidden IN (0, 1)),
	it				INTEGER NOT NULL CHECK (it IN (0, 1)),
	suck			INTEGER NOT NULL CHECK (suck IN (0, 1)),
	brief			INTEGER NOT NULL CHECK (brief IN (0, 1)),
	echo			INTEGER NOT NULL CHECK (echo IN (0, 1)),
	mes_block		INTEGER NOT NULL CHECK (mes_block IN (0, 1)),
	dead			INTEGER NOT NULL CHECK (dead IN (0, 1)),
	plague			INTEGER NOT NULL CHECK (plague IN (0, 1)),
	pw_hash			TEXT NOT NULL		-- PWHash: bcrypt; '*' or '': locked
) STRICT;

CREATE TABLE player_objects (
	player			INTEGER NOT NULL REFERENCES players (id) ON DELETE CASCADE,
	slot			INTEGER NOT NULL CHECK (slot >= 1),	-- place in ObjectTail's list, as saved
	worn			TEXT CHECK (worn IN ('weapon', 'armor', 'shield', 'ring-left', 'ring-right')),
						-- NWeap, NArm, NShield, NRingL, NRingR; NULL: carried
	-- <object columns>
	PRIMARY KEY (player, slot)
) STRICT;

CREATE UNIQUE INDEX player_worn ON player_objects (player, worn) WHERE worn IS NOT NULL;
```

Migration 2, the login limits (#93).  Times are seconds since 1970.

```sql
CREATE TABLE login_hosts (
	host			TEXT PRIMARY KEY,	-- an IPv4 address, or an IPv6 /64 as "2001:db8:1:2::/64"
	fails			INTEGER NOT NULL CHECK (fails >= 0),	-- wrong passwords since start
	blocks			INTEGER NOT NULL CHECK (blocks >= 0),	-- refusals, for the doubling
	start			INTEGER NOT NULL,	-- when this count of failures began
	until			INTEGER NOT NULL	-- the end of its refusal; 0 if never refused
) STRICT, WITHOUT ROWID;

CREATE TABLE login_names (
	name			TEXT PRIMARY KEY CHECK (name <> '' AND name = upper(name)),	-- a character, in capitals
	fails			INTEGER NOT NULL CHECK (fails >= 0),	-- wrong passwords, from anywhere
	last			INTEGER NOT NULL	-- the last of them
) STRICT, WITHOUT ROWID;
```

Migration 3, the history of DM ranks (#139).  A rank is `none`, `associate`
(`ssj` and a non-zero `assoc`), `dm` (`ssj` alone) or `master` (`ssj` and
`master`).  The `assoc` number only labels an associate (`Assoc DM 3`);
every associate is restricted in the same way.

```sql
CREATE TABLE rank_changes (
	id				INTEGER PRIMARY KEY,	-- in the order the changes were made
	time			INTEGER NOT NULL,	-- seconds since 1970
	player			TEXT NOT NULL CHECK (player <> '' AND player = upper(player)),	-- the character, in capitals
	old_rank		TEXT CHECK (old_rank IN ('none', 'associate', 'dm', 'master')),	-- NULL: a new character
	new_rank		TEXT CHECK (new_rank IN ('none', 'associate', 'dm', 'master')),	-- NULL: removed
	by_whom			TEXT NOT NULL CHECK (by_whom <> ''),	-- a DM character (in capitals), or an account on the host
	via				TEXT NOT NULL CHECK (via IN ('game', 'dmadmin')),
	CHECK (old_rank IS NOT NULL OR new_rank IS NOT NULL)
) STRICT;
```

Migration 4, the world (#138).  Times are seconds since 1970.

```sql
CREATE TABLE world (
	id				INTEGER PRIMARY KEY CHECK (id = 1),	-- one row, once the world is known
	world_id		TEXT NOT NULL CHECK (length(world_id) = 36 AND world_id = lower(world_id)),	-- [world] id
	name			TEXT NOT NULL,		-- [world] name
	revision		TEXT NOT NULL CHECK (length(revision) = 64 AND revision = lower(revision))	-- SHA-256 of the world file
) STRICT;

CREATE TABLE world_history (
	id				INTEGER PRIMARY KEY,	-- in the order it happened
	time			INTEGER NOT NULL,
	event			TEXT NOT NULL CHECK (event IN ('made', 'updated', 'adopted')),
	revision		TEXT NOT NULL CHECK (length(revision) = 64),	-- the world's revision then
	schema_version	INTEGER NOT NULL CHECK (schema_version >= 4)	-- the DBVersion that wrote it
) STRICT;

-- rooms, descriptions, monster_templates and object_templates each gain:
ALTER TABLE rooms ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0;	-- 0: from before migration 4
ALTER TABLE rooms ADD COLUMN updated_at INTEGER NOT NULL DEFAULT 0;	-- moved on by DM edits only

-- a table row is stored only as its non-zero entries, so its times are kept apart
CREATE TABLE encounter_times (
	encounter		INTEGER PRIMARY KEY CHECK (encounter >= 1),
	created_at		INTEGER NOT NULL DEFAULT 0,
	updated_at		INTEGER NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE treasure_times (
	treasure		INTEGER PRIMARY KEY CHECK (treasure >= 1),
	created_at		INTEGER NOT NULL DEFAULT 0,
	updated_at		INTEGER NOT NULL DEFAULT 0
) STRICT;
```

## Schema versions and upgrading

`DBVersion` in `gamedb.h` is the schema version this build reads, kept in
the database as `PRAGMA user_version`.  `scepterd` refuses any other: an
older database says to run `upgradegame`, a newer one that it needs a newer
`scepterd`.  The server never migrates a database itself, so the previous
binary can always go back to the backup `upgradegame` made.

To change the schema, add migration `DBVersion + 1` to `Migration[]` in
`gamedb.c` and raise `DBVersion`.  Never edit a migration a deployed game
has already applied.

| Version | Change |
|---|---|
| 1 | The whole game (#147 to #152). |
| 2 | The login limits, `login_hosts` and `login_names` (#93). |
| 3 | The history of DM ranks, `rank_changes` (#139). |
| 4 | The world a game was made from, its history, and `created_at`/`updated_at` on world data (#138). |

Stop the server, then run `upgradegame` as the user that owns the game:

```sh
upgradegame -n GAMEDIR    # report what would change
upgradegame GAMEDIR
```

`upgradegame` (`DBMigrate`):

1. backs `game.db` up to `game.db.backup-YYYYMMDD-HHMMSS` in the game
   directory;
2. applies every missing migration in one transaction;
3. checks the result (`quick_check`, `foreign_key_check`, the version).

If any step fails, the database is left as it was, beside its backup.  A
current database is left alone.  To undo an upgrade, stop the server and
restore the backup (`DBRestore`): copy it over `game.db` and remove any
`game.db-wal` and `game.db-shm` left beside it.

`make check-data` tests all of this, and migrates a whole game with
`dbtest2`, a test build of `dbtest` with one extra, test-only migration.

A game directory from before the database, with `ram/`, `dat/` or struct
files and no `game.db`, can't be converted (#71): `scepterd` refuses it and
says to make a new game with `mkgame`.

## Locked characters

A character whose `pw_hash` is `*` or empty is locked: no password opens
it (#50).  A master DM unlocks one by giving it a temporary password:
`*EDIT`, then `MODIFY PLAYER name PW=password`.  The password is the rest
of the line, so `PW=` comes last; it may contain spaces, and it must be 8
to 72 characters long, as at `CHANGEPW`.  The player then chooses a real
one with `CHANGEPW`.  With the server stopped, `dmadmin GAMEDIR password
NAME` does the same from outside the game, for any character, including a
master DM locked out of their own (docs/running.md).

## Dumping a game

`dumpgame GAMEDIR` (`make dumpgame`) prints everything a game has saved as
text, one record per line with each field named as in `scepter.h`:
settings, the encounter and treasure tables, special rooms, descriptions,
templates, rooms and what is in them, characters, the login limits, the
changes of DM rank, and the world with its history and DM edits (without
their times, which change every run).  It reads the game
through the engine's own functions, as `scepterd` would, and changes
nothing.  Fields that matter only during play are left out, and a
password hash is shown only as `present`, `locked` or `empty`.

`make check-dump` (`tools/dumptest.py`) dumps three games and compares
them with `tools/dumps/` byte for byte: fresh games of `alpha` and
`havenford`, and an `alpha` game in use, which `dmtest GAMEDIR 146` fills
with objects, chests, monsters, characters and editor changes.  It also
fails if a saved struct field is missing from the dumps.  The move to
SQLite (#71) must keep these dumps exactly as they are.  After a change to
a world file, or a deliberate change to what is saved, rewrite them with
`python3 -I tools/dumptest.py --update` and say why in the pull request.
