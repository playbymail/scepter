# World file format

A world file is the text source for a Scepter world. `mkgame` compiles it
into the data files for a new game:

```sh
make
./mkgame var/alpha/alpha.world games/one      # create a game
./scepterd games/one                          # play it
```

`mkgame` never writes the world file, so one world can seed any number of
games. Each game keeps its own state in its directory:

| File | Contents |
|---|---|
| `dat/efile` | notice, news, obituaries, run counter, monster speed, encounter and treasure tables |
| `ram/edungeon.new` | rooms |
| `ram/dbobj.new`, `ram/dbmon.new` | objects and monsters currently in rooms |
| `ram/mlist.new`, `ram/olist.new` | monster and object templates (`dat/` holds a second copy) |
| `ram/dscrpt` | description text (`dat/` holds a second copy) |
| `ram/eperson.new` | player characters (empty in a new game) |
| `dat/errlog` | server error log |

`mkgame` refuses to write into a directory that already holds a game,
unless you pass `-f`.

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
| `revive-room` | not supported yet: the engine always revives dead characters in room 17 |

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

The keys `greeting`, `role` (`bar`, `store`, `pawnshop`, `smith` or
`newsstand`), `stock`, `staggers` and `trains` are accepted, but `mkgame`
warns that the engine ignores them. The engine fixes these rooms by
number:

| Room | Role |
|---|---|
| 1 | start |
| 3 | bar (staggering drunks land in rooms 1–10) |
| 4 | newsstand |
| 5 | jail |
| 8 | pawnshop |
| 10 | store (sells objects 1–20) |
| 17 | revive |
| 18 | smith |
| 20 | trains fighters, barbarians and DMs |
| 21–25 | train paladins, clerics, thieves, magic-users and rangers |

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

A room monster may have inventory blocks, `[room N / monster K / object J]`.
They are parsed, but the engine does not keep monster inventory yet, so
they are ignored with a warning.

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

Flags: `carry`, `magic`, `permanent`, `invisible`, `cursed`.

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
