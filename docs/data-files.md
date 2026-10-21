# Game data files

`mkgame` writes a game directory; `scepterd` reads and updates it.  The
binary files start with a header, so a server never reads a file it wasn't
built for.

## Files

| File | Holds | Magic |
|---|---|---|
| `ram/edungeon.new` | rooms (`RoomType`), 10 per segment | `SCEPDUNG` |
| `ram/dbobj.new` | room and chest contents (`ObjRec`), a 1009-slot hash table | `SCEPDBOB` |
| `ram/dbmon.new` | room monsters (`MonRec`), a 1009-slot hash table | `SCEPDBMN` |
| `ram/eperson.new` | saved characters (`UserFRec`) | `SCEPPERS` |
| `ram/mlist.new`, `dat/mlist.new` | monster templates (`MonsterType`) | `SCEPMLST` |
| `ram/olist.new`, `dat/olist.new` | object templates (`ObjectType`) | `SCEPOLST` |
| `ram/dscrpt`, `dat/dscrpt` | description records (10 lines of 80 characters) | `SCEPDESC` |

The DM editor updates both the `ram/` and `dat/` copies.  `dat/efile`
(game settings and news), `dat/special` (special rooms) and `dat/errlog`
are text files without headers.  The hourly update writes `dat/efile.tmp`
and renames it over `dat/efile`, so a failed write leaves the old file (#17).

`dbobj.new` and `dbmon.new` always hold 1009 records.  A record's slot is
`(room × 73 + type) mod 1009`, or the next slot along that isn't taken.
In `dbobj.new`, type 0 is a room's objects, 1–8 the contents of its first to
eighth chest, and 9–16 the inventory of its first to eighth monster.
`dbmon.new` uses type 0 only.  The first field, `Room`, is 0 for a slot
never used, which ends a search, and -1 for a deleted record, which doesn't,
so records stored past it can still be found (#64).

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
