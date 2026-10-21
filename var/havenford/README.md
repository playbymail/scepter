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

There is no map generator in the repository.  To redraw the map, give this
prompt to Claude Code in this repository:

> Draw an interactive map of the Havenford world as a single HTML page.
> Read every fact from `var/havenford/havenford.world` (format:
> `docs/world-format.md`) with a script; hand-copy nothing but the layout
> below.  Room names come from each room's description sentence ("You are
> in the River Inn." becomes "River Inn").  Expand object and monster names
> as `ExpandName` does: `sign,aether *` is "aether sign".
>
> Draw one SVG with every room as a numbered square on a grid.  The world
> isn't a consistent grid, so use this layout (x grows east, y grows
> south), and check that no two rooms share a square:
>
> - City: 1 (0,0), 2 (1,0), 3 (1,−1), 4 (−1,−1), 5 (2,−1), 6 (1,−2),
>   7 (−1,−2), 8 (−1,−3), 9 (2,−2), 10 (1,1), 11 (2,1), 12 (0,1),
>   13 (1,2), 14 (2,2), 15 (0,2), 16 (3,0), 17 (3,−1), 18 (3,−2),
>   19 (3,−3), 20 (4,−3), 21 (3,−4), 22 (3,−5), 38 (2,−3), 39 (1,−3),
>   40 (−1,−4), 41 (4,0), 42 (4,−1).
> - North road: 23–31 run north from (3,−6) to (3,−14); 32 (2,−11),
>   37 (1,−11), 33 (3,−15), 34 (3,−16), 35 (3,−17), 36 (4,−17).
> - Graveward: 43–51 run west from (−2,−2) to (−10,−2); 56 (−8,−3),
>   52 (−10,−1), 53 (−11,−1), 55 (−11,0), 54 (−12,−1), 57 (−13,−1).
> - Lockspire Keep, one floor below another: 58–63 run west from (0,3) to
>   (−5,3); 65 (−6,3), 64 (−5,4), 76 (−5,2); 66 (−6,5), 67 (−7,5),
>   69 (−8,5), 68 (−7,6), 77 (−7,4); 70 (−8,7), 71 (−9,7), 73 (−10,7),
>   72 (−9,8), 78 (−9,6); 74 (−10,9), 75 (−11,9).  Label the rows
>   ground, 2nd floor, 3rd floor and roof.
> - Aether Observatory: 79 (0,−6), 80 (−1,−6), 93 (−1,−5), 81 (−1,−7),
>   91 (0,−7), 94 (1,−7), 82 (−2,−7), 92 (−2,−6), 83 (−3,−7), 84 (−3,−8),
>   85 (−3,−9), 86 (−4,−9), 87–90 run north from (−4,−10) to (−4,−13).
>   Bend the link from 6 to 79 through (0,−3), so it doesn't look like a
>   link from 6 to 7.
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

Update the layout in the prompt when rooms are added or moved.
