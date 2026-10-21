# Game data files

`mkgame` writes a game directory; `scepterd` reads and updates it.  Every
file sits directly in the game directory, which has no subdirectories.
The game is moving from raw struct files to one SQLite database,
`game.db` (#71): it exists from #147 on, and each later stage moves one
kind of file into it.  The binary struct files start with a header, so a
server never reads a file it wasn't built for.

## Files

| File | Holds | Magic |
|---|---|---|
| `game.db` | the game database (see below) | |
| `edungeon` | rooms (`RoomType`), 10 per segment | `SCEPDUNG` |
| `dbobj` | room and chest contents (`ObjRec`), a 1009-slot hash table | `SCEPDBOB` |
| `dbmon` | room monsters (`MonRec`), a 1009-slot hash table | `SCEPDBMN` |
| `eperson` | saved characters (`UserFRec`) | `SCEPPERS` |
| `mlist`, `mlist.hd` | monster templates (`MonsterType`) | `SCEPMLST` |
| `olist`, `olist.hd` | object templates (`ObjectType`) | `SCEPOLST` |
| `dscrpt`, `dscrpt.hd` | description records (10 lines of 80 characters) | `SCEPDESC` |

The DM editor updates both copies of the templates and descriptions.
`efile` (game settings and news), `special` (special rooms) and `errlog`
are text files without headers.  The hourly update writes `efile.tmp`
and renames it over `efile`, so a failed write leaves the old file (#17).

`dbobj.new` and `dbmon.new` always hold 1009 records.  A record's slot is
`(room × 73 + type) mod 1009`, or the next slot along that isn't taken.
In `dbobj.new`, type 0 is a room's objects, 1–8 the contents of its first to
eighth chest, and 9–16 the inventory of its first to eighth monster.
`dbmon.new` uses type 0 only.  The first field, `Room`, is 0 for a slot
never used, which ends a search, and -1 for a deleted record, which doesn't,
so records stored past it can still be found (#64).

## The game database

`game.db` is a SQLite database in WAL mode (`gamedb.c`).  Its schema
version is `PRAGMA user_version`; this build reads version `DBVersion`
(`gamedb.h`), currently 1.

- **`mkgame`** creates it with the whole schema, empty for now.  The
  game directory must already exist and be empty.
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

-- ---------- settings (EFILE) and the start/revive/jail rooms (SPECIAL) ----------

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
	-- the kind's own fields (union tObject): set for that kind, NULL otherwise
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
	CHECK ((kind = 'weapon') = (min_hp IS NOT NULL AND max_hp IS NOT NULL
		AND strikes IS NOT NULL AND wea_plus IS NOT NULL AND weap_type IS NOT NULL)),
	CHECK ((kind = 'shield') = (sh_plus IS NOT NULL AND sh_hits IS NOT NULL)),
	CHECK ((kind = 'armor') = (arm_plus IS NOT NULL AND arm_hits IS NOT NULL)),
	CHECK ((kind = 'coins') = (multiplier IS NOT NULL)),
	CHECK ((kind = 'scroll') = (spell IS NOT NULL)),
	CHECK ((kind = 'chest') = (closed IS NOT NULL AND trap IS NOT NULL
		AND locked IS NOT NULL AND num_inside IS NOT NULL)),
	CHECK ((kind = 'door') = (d_to_where IS NOT NULL AND d_closed IS NOT NULL
		AND d_spring IS NOT NULL AND d_toll IS NOT NULL AND d_trap IS NOT NULL
		AND d_locked IS NOT NULL)),
	CHECK ((kind = 'keys') = (unlock IS NOT NULL AND num_uses IS NOT NULL)),
	CHECK ((kind = 'device') = (m_spell IS NOT NULL AND num_charges IS NOT NULL
		AND m_level IS NOT NULL)),
	CHECK ((kind = 'teleport') = (t_to_where IS NOT NULL AND t_active_rm IS NOT NULL)),
	CHECK ((kind = 'ring') = (r_spell IS NOT NULL AND r_level IS NOT NULL))
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

-- Each room's role and greeting (SPECIAL); a room with neither has no row.
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

## Header

Each binary file starts with 32 bytes, written byte by byte so they read the
same on every machine:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic: names the kind of file (table above) |
| 8 | 4 | format version, unsigned little-endian; currently 2 |
| 12 | 4 | record size in bytes, unsigned little-endian |
| 16 | 16 | reserved, zero |

The records follow the header.  They are still raw C structs (#5), so their
layout depends on the compiler and ABI.  The record size in the header
catches a build whose structs differ.

## Versions

`DataVersion` in `scepter.h` is the format version.  Bump it whenever any
record layout changes, and add a conversion step to `tools/upgradegame.c`.

`scepterd` and `fightsim` check every file before starting.  They refuse the
whole game if any file:

- is missing or unreadable;
- has no header or the wrong magic;
- has a different version, older or newer;
- has a record size that doesn't match the build.

Each problem is reported with the file's name.  The server never converts
files itself, so you can always go back to the previous binary with the
backup that `upgradegame` made.

| Version | Change |
|---|---|
| 0 | No header.  Every game made before headers existed. |
| 1 | Adds the header; the records are unchanged. |
| 2 | Player records (`UserSave`) gain `PWHash`, a bcrypt hash (#82). |

## Upgrading a game

Stop the server, then run `upgradegame` as the user that owns the game:

```sh
upgradegame -n GAMEDIR    # report what would change
upgradegame GAMEDIR
```

`upgradegame` checks every file first and changes nothing if any of them
can't be upgraded (for example, a newer version or a foreign file).  It then:

1. copies the game directory to `GAMEDIR.backup-YYYYMMDD-HHMMSS`;
2. rewrites each old file to a temporary file, flushes it to disk, and
   renames it over the original, keeping the owner and mode;
3. checks the result as `scepterd` would.

Each file is converted straight to the current version.  From version 1
to 2 only the player file's records change: each gets a locked `PWHash`
(`*`), which no password matches, since the server no longer accepts the
old hash (#50).  A master DM unlocks a character by giving it a temporary
password: `*EDIT`, then `MODIFY PLAYER name PW=password`.  The password is
the rest of the line, so `PW=` comes last; it may contain spaces, and it
must be 8 to 72 characters long, as at `CHANGEPW`.  The player then chooses a real one with `CHANGEPW`.  The
other files just get the new header.

If it stops part way, run it again: files that are already current are left
alone.  To undo an upgrade, stop the server and put the backup back in place
of the game directory.

`make check-data` tests all of this.

## Dumping a game

`dumpgame GAMEDIR` (`make dumpgame`) prints everything a game has saved as
text, one record per line with each field named as in `scepter.h`:
settings, the encounter and treasure tables, special rooms, descriptions,
templates, rooms and what is in them, and characters.  It reads the game
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
