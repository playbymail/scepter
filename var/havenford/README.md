# Havenford

Havenford is the beta test world and the setting for the tutorials. A safe
city sits in the middle, and four regions lead out of it, each testing a
different part of the game.

```sh
./mkgame var/havenford/havenford.world games/havenford
./scepterd games/havenford
```

`havenford.world` is the source; `world.json` is its JSON copy, and
`make check-worlds` fails unless the two agree.  After editing the world:

```sh
python3 tools/worldjson.py to-json var/havenford/havenford.world var/havenford/world.json
```

`to-world` rewrites the world file in canonical form, dropping hand-written
comments, so notes about the design belong in this file.

## Starting out

New characters start in room 1 with 200 shillings and nothing else.
Room 1 is also where dead characters revive and where jailed ones go.
There is no free equipment: buy a weapon and armour at the Provisioner
(room 11).

## The city (rooms 1–22, 38–42)

Every room in the city is safe: there are no monsters and no fighting.

| Room | Place | Role |
|---|---|---|
| 1 | Havenford Refuge | start, revive and jail room |
| 2 | Havenford Square | hub |
| 3 | Guild Street | training halls |
| 4 | Hall of Blades | trains fighters and barbarians |
| 5 | Scout Lodge | trains rangers |
| 6 | Academy of Sparks | trains magic-users; the Observatory is west |
| 7 | Lantern Chapel | trains clerics and paladins; the Graveward is west |
| 8 | Shadow School | trains thieves |
| 9 | Hall of Stewards | trains DMs |
| 11 | Provisioner | store: dagger, practice sword, leather armour, wooden shield, healing scroll, coins |
| 12 | Smithy | smith (`REPAIR`) |
| 13 | River Inn | bar (`BUY`); drunks stagger to rooms 1, 2, 10 or 13 |
| 14 | Notice Kiosk | newsstand (`BUY` reads the paper) |
| 15 | Exchange | pawnshop (`PAWN`); the road to Lockspire Keep runs south |
| 17 | North Gate | direction sign |
| 22 | Frontier Warning Post | warning sign; the wilds start north |

## The north road (rooms 23–37)

Monsters start at room 23, north of the warning post.  They get stronger as
the road climbs to the watchtower:

| Rooms | Monsters | Levels |
|---|---|---|
| 23–27 | field rat, restless crow, stray dog, red fox | 1–2 |
| 28–32, 37 | red fox, highway thief, brittle skeleton, grey wolf | 2–3 |
| 33–36 (watchtower) | armored raider, tower sentinel, bandit captain | 4–5 |

## The Graveward (rooms 43–57)

The Graveward is undead country, west of the Lantern Chapel (room 7).
Rooms 43–47 are a safe approach; the warning sign is in room 47.

| Rooms | Area | Monsters | Levels |
|---|---|---|---|
| 48–50 | cemetery | shambling skeleton, restless spirit | 1–2 |
| 51–53, 55 | mausoleum | restless spirit, grave guardian | 2–3 |
| 54, 57 | deep crypt | grave guardian, bone knight, crypt wight | 3–5 |
| 56 | overlook | none: a safe detour north of room 49 | — |

Treasure gets richer deeper in.

## Lockspire Keep (rooms 58–78)

The keep is for practising thief skills.  It has locks and traps but no
monsters.  To reach it, go south from the pawnshop (room 15) and west along
the causeway; the warning board is in room 60.  The stairs are never
locked.  Behind each locked door is a side room with a locked, trapped
chest:

| Floor | Hall | Behind its doors | Traps |
|---|---|---|---|
| ground | 63 | 64 (brass), 76 (needle) | 0–3 |
| second | 67 | 68 (records), 77 (armory) | 5–12 |
| third | 71 | 72 (jewel), 78 (artificer) | 11–17 |
| roof | 74 | 75 (crowning) | 18–19; trap damage can kill |

A door's lock number names its mechanism: it says which key fits, not how
hard the lock is to pick.  Each chest has a different lock from the door
in front of it.

To refer to the second of two doors in a room, use `door 2`: `door` alone
means the first one listed.

## The Aether Observatory (rooms 79–94)

The Observatory is for magic-users.  Go west from the Academy of Sparks
(room 6).  Rooms 79–83 and 91–93 are safe; the warning sign is in room 83.

| Rooms | Monsters | Levels |
|---|---|---|
| 84–85 | flickering mote, glass sprite | 1–2 |
| 86–88 | glass sprite, spark elemental, mirror shade | 2–3 |
| 89–90 | mirror shade, prismatic guardian, aether warden | 3–5 |

These side rooms show room flags and objects:

| Room | Shows |
|---|---|
| 91 | spell library; the lessons sign points to the lantern |
| 92 | anti-magic: every spell fails, even ones safe rooms allow |
| 93 | workshop, with a heal scroll; the scroll is gone once someone takes it |
| 94 | dark: `LOOK` shows nothing, but `USE LANTERN` shows the room once to everyone in it |

The lantern in room 94 is a magic device that casts ILLUMINATE.  The
spell fails in safe rooms, so room 94 is not safe: players can
fight each other there and attack spells work, though no monsters
ever appear.  A device used where it stands spends no charges, so
the lantern never runs out, and it can't be carried off.  Players
can't see it in the dark, so they have to be told its name; the lessons
sign does that.

## Encounters

None of the monsters drains, poisons, casts spells, reacts fast or follows
a fleeing player.  None attacks first.  The field rat, restless crow and
red fox never fight back and flee when hit; the rest fight whoever
attacks them.

`encounter-rate` is one chance in N each second that a player is in the
room, so a lower number means monsters appear more often.  Rates fall from
100–110 at the edge of each region to 40–45 at its far end.  Players
who walk straight through rarely meet anything: at a rate of 100,
monsters appear about once every 100 seconds.

## Drawing the map

`world-map.html` is the interactive map: open it in a browser.  It is
drawn from the world file and isn't updated when the world changes.  There
is no map generator in the repository; to redraw the map, give this prompt
to Claude Code in this repository:

> Draw an interactive map of the Havenford world as a single HTML page.
> Read every fact from `var/havenford/havenford.world` (format:
> `docs/world-format.md`) with a script; hand-copy nothing but the layout
> below.  Room names come from each room's description sentence ("You are
> in the River Inn." becomes "River Inn").  Expand object and monster names
> as `ExpandName` does: `sign,aether *` is "aether sign".
>
> Draw one SVG with every room as a numbered square on a grid, at the
> room's `x` and `y` from the world file (`y` grows north, so draw it
> upward), with its `area` as its region.  Rooms above or below the ground
> (`z` not 0) would share a square with the room under them, so shift them
> on the flat map, and check that no two rooms share a square:
>
> - Lockspire Keep: draw each floor two rows south of the one below it, and
>   label the rows ground, 2nd floor, 3rd floor and roof.
> - The Graveward crypt (`z` −1): one row south.
> - The watchtower loft (36): one column east.
> - The observatory tower (88–90): one row north for each level.
>
> Bend the link from 6 to 79 through the empty square at (0,3), so it doesn't
> look like a link from 6 to 7.
>
> Draw each exit once: solid for paths, dotted for up and down, and dashed
> in another colour for doors (`kind = door` objects with `to`, which lead
> into the keep's chest rooms).  Mark a one-way exit with an arrow.  Tint
> each region's squares a different colour and label the regions.  Fill a
> room by danger: safe; or levels 1–3 from its encounter table (tables 1,
> 4 and 7 are level 1; 2, 5 and 8 level 2; 3, 6 and 9 level 3).  Draw dark
> room 94 filled with ink and anti-magic room 92 with a dotted border.
> Mark trainers, shops and services, and rooms with signs, and ring the
> start room.
>
> Clicking a room, on the map or in its exit list, opens a side panel with
> its number, region, description, greeting, and exits with their true
> directions.  The panel also shows sign texts, other objects, the
> monsters its table can produce with their levels, and its danger.
> `encounter-rate` N means one chance in N per second, so show it that
> way, not as a percentage.  Add a route finder: two room pickers and a
> breadth-first search over exits and doors, which shows the moves as
> `n e u` (a door as `[brass door]`), lists the steps, and highlights the
> route.  Open it on the River Inn (13) to the Frontier Warning Post (22).
> Below the map, add a legend and an index of rooms grouped by region.
>
> Support light and dark themes, and keep the page usable at phone width:
> the map scrolls inside its own box.  Before publishing, run the route
> search on 13 → 22 (eight moves: `n n e n n n n n`) and 1 → 75 (through
> the crowning door).  Publish it as an artifact named "Havenford Map".

Give new rooms an `area` and coordinates in the world file.  Two exits
don't match the coordinates: west from 6 leads to 79, four squares north
of where west would be, and south from 91 leads to 94, which is east of it.
A room's square is taken in each case.
