# Update a character who is online

This guide raises a logged-in character's level and stats, and gives them
equipment, using the DM editor. The example makes Hyramn a 5th-level
character with a full set of magic gear.

You need:

- a DM character (one made with `dmadmin`, see [running.md](../running.md#dms));
- the character you're changing, here Hyramn, logged in and playing.

Every command below is typed by the DM. Hyramn types only the commands in
step 4.

## 1. Open the editor

```
*edit
```

The prompt becomes `Edit command?`. Everything until `quit` goes to the
editor.

## 2. Set the level and stats

```
modify player Hyramn lv=5 mv=90 vi=90 mf=90 fa=90 mm=18 ma=18 ss=90
```

| Key | Sets | Here |
|---|---|---|
| `lv=` | level (0 to 25) | 5 |
| `mv=` / `vi=` | maximum and current vitality | 90/90 |
| `mf=` / `fa=` | maximum and current fatigue | 90/90 |
| `mm=` / `ma=` | maximum and current magic | 18/18 |
| `ss=` | skill with sharp weapons, in percent | 70% |

The game answers `Player modified.`

**Weapon skills stop at 70%.** A skill is kept in steps of 10%, at most
7, in the editor and in training alike, so `ss=90` sets 70%, the most a
character can have. The other skills are `ts=` (thrust), `bs=` (blunt) and
`ps=` (pole).

The level changes nothing else: experience counts from the start of the
level, and the stats are what you set.

## 3. Give the equipment

Each `create object` line makes a new object in Hyramn's pack. The `x` is a
placeholder the command needs. Write the name in double quotes, as a noun,
a comma, then the rest of the name with `*` where the noun goes:
`"sword,silver *"` reads "a silver sword", and `"ring,* of piety"` reads
"a ring of piety". Put `ty=` before the keys that belong to that type.

A magic silver sword (2 to 9 damage, +3 to hit, 100 strikes):

```
create object x Hyramn na="sword,silver *" ty=weapon ma=t we=5 va=200 mi=2 mh=9 sl=100 wp=3 wt=sharp
```

Magic plate armor (+4, 200 hits). `ar=some` makes it read "some plate
armor":

```
create object x Hyramn na="armor,plate *" ar=some ty=armor ma=t we=25 va=800 af=4 ah=200
```

A magic plate shield (+3, 150 hits):

```
create object x Hyramn na="shield,plate *" ty=shield ma=t we=10 va=400 sf=3 sh=150
```

A magic ring of piety, casting BLESS (spell 13) at 5th level:

```
create object x Hyramn na="ring,* of piety" ty=ring ma=t we=1 va=1500 sp=13 lv=5
```

Each one answers `Object created.` and `Object modified.`

| Key | Sets |
|---|---|
| `ma=t` | magic. A magic monster can only be hurt by a magic weapon. |
| `we=` / `va=` | weight in pounds, value in shillings |
| `mi=` / `mh=` / `sl=` / `wp=` / `wt=` | a weapon's least and most damage, strikes left, bonus to hit, and kind (`sharp`, `thrust`, `blunt` or `pole`) |
| `af=` / `ah=` | armor's protection and hits |
| `sf=` / `sh=` | a shield's protection and hits |
| `sp=` / `lv=` | a ring's spell and level |

A ring's spell has no effect in the game yet (#179); a player sees it with
`IDENTIFY ring`.

## 4. Check, then have the player save

```
display player Hyramn
quit
```

The display should show:

```
Hyramn, the fifth level Fighter in room 1/1.
With 90/90 vitality pts, 90/90 fatigue pts, and 18/18 magic pts.
...
Weapon skills:  Sharp-70%, Thrust-0%, Blunt-0%, Pole-0%.
...
You are carrying 41 lbs of items:
   1) A magic ring of piety
   2) A magic plate shield (+3)
   3) Some magic plate armor (+4)
   4) A magic silver sword (+3)
```

The editor doesn't tell Hyramn anything, so tell them (for example with
`*send Hyramn ...`). Hyramn puts the equipment on and saves:

```
wield sword
wear armor
wear shield
wear ring left
save
```

**The player must save.** The editor changes the character in play only.
They are stored when Hyramn types `SAVE` (`Character file updated.`), logs
off, or the server stops. If the server stops without saving them, a crash
for instance, the changes are lost. A character below 2nd level is never
saved.

## A character who is offline

To change a character who isn't logged in, add `file` before the name in
each command:

```
modify file Hyramn lv=5 mv=90 vi=90 mf=90 fa=90 mm=18 ma=18 ss=90
create object x file Hyramn na="sword,silver *" ty=weapon ma=t we=5 va=200 mi=2 mh=9 sl=100 wp=3 wt=sharp
display file Hyramn
```

These change the saved character straight away, so no one needs to save.
Don't use `file` for a character who is logged in: when they next save,
the copy in play overwrites the change.
