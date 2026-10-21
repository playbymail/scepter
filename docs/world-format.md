# World file format

A world file is the text source for a Scepter world. `mkgame` compiles it
into the data files for a new game:

```sh
make
mkdir -p games/one                            # an empty directory
./mkgame var/alpha/alpha.world games/one      # create a game in it
./scepterd games/one                          # play it
```

`mkgame` never writes the world file, so one world can seed any number of
games. Each game keeps its own state in its directory:

| File | Contents |
|---|---|
| `game.db` | the game database: monster and object templates, description text, rooms and what is in them, and player characters (none in a new game); the files below move into it (#71) |
| `efile` | notice, news, obituaries, run counter, monster speed, encounter and treasure tables |
| `special` | start, revive and jail rooms; shop roles, greetings, stock, bar staggers, training rooms |
| `errlog` | server error log, made when something is first logged |

The directory must exist and be empty before `mkgame` runs; `mkgame`
never creates a directory and never overwrites or deletes anything.  To
make a game again, empty the directory or use a new one.

Every world file has a JSON copy beside it, `var/NAME/world.json`. The
world file is the source; `tools/worldjson.py` converts in both directions,
and `make check-worlds` fails if the two copies differ. The tool writes the
world file in a canonical form, so it doesn't keep comments you add by
hand.  Write design notes in a `README.md` beside the world instead, as
`var/havenford/README.md` does.

```sh
python3 tools/worldjson.py to-json var/alpha/alpha.world var/alpha/world.json
```

## Syntax

```ini
# comment (a line starting with # or ;)
[block id]
key = value
```

- Blocks can come in any order, except that a nested block such as
  `[room 7 / object 1]` must follow `[room 7]`.
- Values run to the end of the line. There is no quoting, so description
  text can contain `#`, `/` and `"`.
- Booleans are `yes` or `no`.
- Lists are separated by spaces.
- **A key you leave out is zero, `no` or empty.** The one exception is
  `article`, which defaults to `a`. Unlike the in-game editor, `mkgame`
  does not fill in prototype defaults; set each field you need.
- `desc = R:L` refers to line `L` of `[description R]`.

## `[world]`

| Key | Meaning |
|---|---|
| `name` | world name |
| `monster-speed` | seconds between random monster checks |
| `notice` | the notice shown at login |
| `news-1` … `news-5` | lines in the newspaper |
| `start-room` | where new characters start (default 1) |
| `revive-room` | where dead characters revive (default: the start room) |
| `jail-room` | where guards take a player who attacks a monster with `assistance` (default: the start room). This is disabled in the original source (`USEJAIL` in `cmd2.c`/`cmd3.c`), so it currently has no effect. |

## `[area N]`, N = 1…255

An area is a part of the world that a map draws together, such as a town
or a dungeon.

| Key | Meaning |
|---|---|
| `name` | required; printable ASCII, at most 60 characters |

## `[description R]`, R = 1…255

There are ten lines per record, with keys `1` to `10`. Each value is at
most 79 characters, in the engine's phrase format:

```
N/phrase one/phrase two/
```

- `N` is the number of phrases; one is chosen at random.
- Text between `#` marks is omitted in brief mode.
- A `+` makes the description continue onto the next line.

## `[room N]`, N = 1…1999

| Key | Meaning |
|---|---|
| `desc` | `R:L` |
| `north` `south` `east` `west` `up` `down` `out` | destination room |
| `encounter-table` | `[encounter N]` used for random monsters |
| `encounter-rate` | 1–255: a random encounter happens on 1 check in N, so higher numbers are rarer; 0 means never |
| `safe` | players cannot be attacked here |
| `anti-magic` | magic does not work here |
| `dark` | the room is dark |
| `notify-dm` | DMs are told when a player enters |
| `area` | the `[area N]` the room belongs to |
| `x` `y` `z` | the room's position in its area, each -999…999 |

A room with an `area` is placed on the map at `x y z`, and a key you leave
out is 0. The axes are Mudlet's: `x` grows east, `y` grows north and `z`
grows up, so `north` from (0,0,0) normally leads to (0,1,0) and `up` to
(0,0,1). Exits don't have to match the positions, since not every world is
a grid. A room without an `area` isn't placed, and `x`, `y` or `z` on it
is an error. `mkgame` warns when two rooms in an area share a position.

`mkgame` checks areas and positions but doesn't store them in the game
yet. They will be stored with the rooms once game data moves to SQLite
(#71), for maps such as Mudlet's (#80).

These keys make a room special:

| Key | Meaning |
|---|---|
| `role` | `bar`, `newsstand`, `pawnshop`, `store` or `smith`; a world can have several rooms with the same role |
| `greeting` | a line shown to anyone who enters, at most 250 characters |
| `stock` | for a store, up to 20 object template numbers. CATALOG lists them in this order and `BUY n` buys the nth. |
| `staggers` | for a bar, up to 20 rooms where a drunk may stagger to (default: the start room) |
| `trains` | classes trained here: `fighter`, `thief`, `magic-user`, `dm`, `cleric`, `paladin`, `ranger`, `barbarian` |

Rooms contain up to 8 objects, `[room N / object K]`, and up to 8
monsters, `[room N / monster K]`, with K = 1…8. A chest's contents are
`[room N / object K / object J]`. Chests cannot contain other chests.

An object or monster placed in a room disappears the first time the last
player leaves that room, unless it is `permanent = yes`. Objects inside a
chest are kept.

## `[monster N]`, N = 1…200, and room monsters

| Key | Meaning |
|---|---|
| `name` | up to 20 characters, containing `,` and `*`; for example, `rat,giant *` displays as "giant rat" |
| `article` | `a`, `an`, `some`, `the` or `none` |
| `desc` | `R:L` |
| `level` | 0–25 |
| `hits`, `max-hits` | 0–2500 |
| `ac` | -25–25 |
| `attack-speed` | -10–10 |
| `parley` | 0–30, the reaction to PARLEY |
| `treasure-table` | `[treasure N]` dropped on death |

Flags (`yes`/`no`): `defend`, `block`, `follow`, `guard`, `attack-last`,
`slow-react`, `fast-react`, `moral-react`, `immoral-react`, `nice`, `flee`,
`assistance`, `casts-spells`, `permanent`, `magic`, `invisible`,
`regenerate`, `drain`, `poison`, `anti-magic`, `undead`, `summon-help`,
`unique`, `watch`.

A room monster may carry up to 8 objects, `[room N / monster K / object J]`.
They drop when the monster dies. The inventory is saved with the room;
the contents of a chest a monster carries are not.

## `[object N]`, N = 1…200, and room objects

These keys apply to every kind:

| Key | Meaning |
|---|---|
| `name` | as for monsters |
| `article` | as for monsters |
| `kind` | required, and must come before the kind's own keys |
| `weight` | 0–1000 |
| `price` | 0–16000 |
| `desc` | `R:L` |

Flags: `carry`, `magic`, `permanent`, `invisible`, `cursed`.  `cursed`
is stored but has no effect in the game, as in the original.

| `kind` | Keys |
|---|---|
| `weapon` | `min-damage`, `max-damage` (0–127), `strikes` (0–511), `plus` (-5–10), `skill` (`sharp`, `thrust`, `blunt` or `pole`) |
| `armor`, `shield` | `plus` (-5–10), `hits` (0–511) |
| `coins` | `multiplier` (1–127) |
| `scroll` | `spell` |
| `ring` | `spell`, `level` (0–25) |
| `device` | `spell`, `charges` (0–127), `level` (0–25) |
| `chest` | `closed`, `trap` (0–50), `lock` (0–1023) |
| `door` | `to`, `closed`, `trap` (0–50), `lock` (0–1023), `spring`, `toll` |
| `keys` | `unlocks` (matches a `lock`), `uses` (0–127) |
| `portal` | `to` |
| `teleport` | `to`, `active-room` |
| `treasure`, `card`, `misc` | none |

Spells: `vigor`, `heal`, `fireball`, `lightning`, `hurt`, `curepoison`,
`disintegra`, `befuddle`, `teleport`, `wish`, `passdoor`, `enchant`,
`bless`, `protection`, `curse`, `poison`, `intoxicate`, `psi-blast`,
`polymorph`, `summon`, `animate`, `charm`, `illuminate`.

## `[encounter N]` and `[treasure N]`, N = 1…200

```ini
[encounter 1]
monsters = 1 1 3 2 5      # up to 8 monster template numbers

[treasure 1]
objects = 4 1 5           # up to 8 object template numbers
```

Repeat an entry to make it more likely. The two tables are picked
differently:

- **Encounters** pick one entry at random from those before the first `0`.
- **Treasure** picks one of all 8 slots at random, and an empty slot gives
  nothing. A table with fewer entries therefore drops less often.
