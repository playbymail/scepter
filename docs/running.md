# Running the server

`scepterd` runs one game.  Players connect to it with a MUD client
(TinTin++, Mudlet, MUSHclient) or plain `telnet`.

## Build and make a game

The build needs libxcrypt, for password hashing, and SQLite, for the game
database: `libcrypt-dev` and `libsqlite3-dev` on Ubuntu or Debian; on
macOS, `brew install libxcrypt`, and the SDK's SQLite (see the README).

```sh
make
mkdir -p games/alpha
./mkgame var/alpha/alpha.world games/alpha
```

`mkgame` compiles a world file (see [world-format.md](world-format.md)) into a
game directory, which must already exist and be empty: `mkgame` never
creates a directory, and refuses one that holds anything, hidden files
included.  The server reads and writes the files in that directory.
To try changes to a world file on an existing game, keeping its
characters, update it with `./mkgame -u var/alpha/alpha.world games/alpha`
while the server is stopped (`-n` first, to see what would change; see
[data-files.md](data-files.md)).
A newer `scepterd` may need the game's database upgraded first with
`./upgradegame games/alpha`; it says so when it refuses to start (see
[data-files.md](data-files.md)).

## Start the server

```sh
./scepterd games/alpha
```

By default the server listens on `127.0.0.1:4000`, so only this machine can
connect.  Use `-l [HOST:]PORT` to change the address:

| Command                                 | Listens on                       |
|-----------------------------------------|----------------------------------|
| `./scepterd -l 5000 games/alpha`        | port 5000 on 127.0.0.1           |
| `./scepterd -l 0.0.0.0:4000 games/alpha`| port 4000 on every IPv4 address  |
| `./scepterd -l :4000 games/alpha`       | port 4000 on every address       |
| `./scepterd -l '[::1]:4000' games/alpha`| port 4000 on the IPv6 loopback   |

Telnet is not encrypted: passwords cross the network in plain text.  Before
listening on a public address, put the server behind something that encrypts
the connection.

`-o` sends Mudlet's mapper opaque room ids instead of room numbers (see
[GMCP](#gmcp)).

The server stays in the foreground and logs connections to standard error.
To stop it, press Ctrl-C or send `SIGTERM`.  It saves every player, tells
each one `End - aborted`, and exits.

At most 12 players can connect at once.  Anyone else gets `Too many users`
and is disconnected.

## Connect

TinTin++:

```
#session scepter 127.0.0.1 4000
```

Mudlet or MUSHclient: make a new profile or world with host `127.0.0.1` and
port `4000`.

### Mudlet's mapper

Mudlet's map can be drawn from GMCP `Room.Info` (see [GMCP](#gmcp)) by
`tools/mudlet-mapper.lua`:

1. Tick Settings → General → Enable GMCP.
2. Uninstall `generic_mapper` in Toolbox → Package Manager.  It maps from
   the room text, can't find a prompt here, and prints its help at every
   `look`.
3. Under Scripts, Add Item, paste in `tools/mudlet-mapper.lua`, and save.
4. Open the map and walk around.

Each room is added under its `num`, so revisiting a room finds it again.  A
room's north, south, east, west, up and down exits add their rooms one step
away as placeholders, which fill in as you visit them.  `out` and
`go <thing>` exits are linked once both of their rooms have been visited.
Rooms are coloured by `environment`.  A room reached by `out`, `go` or a
teleport has no direction to be placed by, so it goes two squares north-east
of the last room; drag it where it belongs.

`telnet` (not installed by default on recent macOS; `brew install telnet`):

```sh
telnet 127.0.0.1 4000
```

The game asks for your name.  A new name creates a character: you choose a
class, attributes, weapon skill, password and sex.  Characters are saved when
they reach second level.  Type `stop` at the name prompt to leave.

A password is the whole line you type, without leading and trailing spaces:
8 to 72 characters, spaces allowed, and capital letters differ from small
ones.  `CHANGEPW` asks for the old password, then the new one twice.  A
wrong password at login disconnects you.  Wrong passwords at login and wrong
old passwords at `CHANGEPW` count together.  The counts are kept in the
game database, so a restart doesn't forget them, and there is no limit to
how many addresses or characters they track (#93):

- five wrong passwords from one address within 10 minutes refuse that
  address for 10 minutes, then 20, 40, 80 and at most 120 for each further
  five; a day without a refusal starts again at 10.  An IPv6 address counts
  as its whole /64, since one subscriber is normally given a /64;
- ten wrong passwords for one character, from anywhere, allow only one try
  every 30 seconds until the right one is given (or an hour passes).

Counts that can no longer matter are deleted as new wrong passwords come in.

While a limit applies, `CHANGEPW` is refused too, without asking for the old
password.

Every wrong password is logged to the game's `errlog`, with the address.

## What players see

- Questions such as `Enter your name?` end without a newline and are marked
  with telnet `GA`, so MUD clients can tell a prompt from other output.
- At the command level there is no prompt; type a command and press Enter.
- Commands are capped at 254 characters.  Control characters and empty lines
  are ignored.
- The server refuses every telnet option except GMCP (below), so the client
  echoes what you type and sends whole lines.
- While you are answering a question, messages from other players wait until
  you have answered.
- A player whose connection drops is logged off, and the others are told so.

## GMCP

GMCP (Generic MUD Communication Protocol, telnet option 201) carries data
for the client rather than for the player, such as what a mapper needs.
Mudlet and MUSHclient use it; plain `telnet` and TinTin++ don't, and see no
difference.

Negotiation:

- When a connection opens, the server sends `IAC WILL GMCP`.
- The server sends GMCP only after the client answers `IAC DO GMCP`.
- `IAC DONT GMCP` turns GMCP off and gets no reply.  A later `IAC DO GMCP`
  turns it back on, and the server agrees with `IAC WILL GMCP`.
- `IAC WILL GMCP` from the client is refused with `IAC DONT GMCP`, as for
  every other option.
- The client's `Core.Hello` and `Core.Supports.Set` are read and ignored;
  the server doesn't reply to them, or to any other GMCP the client sends.

Each GMCP message is `IAC SB GMCP <package> <json> IAC SE`.  Like text, it is
held while the player answers a question.  The server sends:

- `Core.Goodbye`, just before it closes a connection.  Its data is a JSON
  string: the closing text, or `"Goodbye."` if there is none.
- `Room.Info`, for Mudlet's mapper, whenever a player's room changes and on
  `LOOK` (below).

### Room.Info

`Room.Info` follows the IRE convention that Mudlet mapping scripts read
from `gmcp.Room.Info`:

```
Room.Info {"num": 2, "name": "North road", "area": "alpha",
  "environment": "plain", "exits": {"n": 6, "s": 1}}
```

| Field | Holds |
|---|---|
| `num` | the room's number in the world file; with `scepterd -o`, an opaque id |
| `name` | the room's `name`, or for a room without one, the first sentence of its brief description, without the full stop |
| `area` | the world's name (areas aren't in the game yet) |
| `environment` | the room's role (`bar`, `newsstand`, `pawnshop`, `store`, `smith`), else `safe`, `anti-magic`, `dark` (only DMs are sent dark rooms) or `plain` |
| `exits` | `n` `s` `e` `w` `u` `d` and `out`: the obvious exits the room's text lists.  Then each door or portal the player can see, as the command that takes it (`"go gate": 26`), closed or locked doors included |

It is sent whenever a player's room changes: on login, every move
(including `go`), following a leader, staggering from the bar, teleports,
death and revival, and DM moves.  A player who can't see in a dark room
is sent nothing there, just as the text tells them nothing, so a mapper
can't place them.  DMs see dark rooms, so they are sent `Room.Info` there.

With `scepterd -o`, `num` and the exits are opaque ids from 1 to 2^30: a
keyed permutation of the room number (a four-round Feistel network on
SHA-256), so no two rooms share an id.  A room has the same id for every
player and across restarts, but the ids give away neither room numbers nor
their order.  The key is `roomkey` in the game directory, made by the first
`scepterd -o` (docs/data-files.md).  Keep it with the game: a new key gives
every room a new id, and players' maps stop matching.

Inside the server, GMCP is another kind of message in the protocol between
the game and the player I/O layer (`ipc.c`): `5<package> <json>`, which the
game makes with `QGmcp(term, package, json)`.  The telnet front end
(`telnet.c`) sends it only to connections that accepted GMCP.  The console
harness and `fightsim` ignore it.

## Console mode

`./scepterd -c games/alpha` plays one character on standard input and output
instead of listening.  End-of-file stops the server.  It is meant for testing
and scripts, for example:

```sh
printf 'Tester\nfighter\n15, 7, 9, 10, 9\nsharp\nsecret\nmale\nwho\n' |
	./scepterd -c games/alpha
```

## DMs

A new game has no DMs, and the login prompts can't make one.  `dmadmin`
manages them from outside the game (#139):

```sh
./dmadmin games/alpha create Boss master     # asks for the password twice
./dmadmin games/alpha list
./dmadmin games/alpha rank Boss dm           # master, dm, associate or none
./dmadmin games/alpha password Boss          # a new password, for any character
./dmadmin games/alpha remove Boss            # deletes a DM character
./dmadmin games/alpha history
```

- **Stop the server first** for `create`, `rank`, `password` and `remove`:
  the game saves a character who is playing over a change made here.
  `list` and `history` only read, and may run at any time.
- **Ranks:** a master DM may change ranks and passwords; a DM may use the
  DM commands and editor; an associate DM may use fewer of them.  The
  number after an associate (`Assoc DM 3`) is only a label.
- **`create`** makes a 2nd-level character of the DM class in the start
  room.  The name is 1 to 10 letters, and saved in capitals.
- **Passwords** are read from the terminal, twice and not echoed, or else
  from the first line of standard input, never from the command line.
  They follow the game's rules: 8 to 72 characters, without the spaces at
  either end.
- **The last master DM** can't be demoted or removed, by `dmadmin` or in the
  game, so there is always someone who can reset passwords.
- **The world:** `*WORLD` shows which world and revision the game was made
  from, its history, and the rooms, descriptions, templates and table rows
  DMs have changed since (docs/data-files.md).
- **History:** every change of rank is recorded with the time and who made
  it: the DM character, in the game's editor (`MODIFY PLAYER` or `MODIFY
  FILE` with `ZZ=` or `AS=`, `DELETE FILE`), or the account that ran
  `dmadmin` (`$SUDO_USER` under sudo, otherwise the login name).

To change a character's level, stats or equipment in the game, see
[how-to/update-an-online-character.md](how-to/update-an-online-character.md).

A DM made this way also logs in through the console harness, for testing
the DM commands:

```sh
printf 'boss password\n' | ./dmadmin games/alpha create Boss master
printf 'Boss\nboss password\n*edit\ndisplay player boss\nquit\n' |
	./scepterd -c games/alpha
```

## Tests

| Command             | What it checks                                         |
|---------------------|--------------------------------------------------------|
| `make check-net`    | the telnet front end, byte by byte, over raw sockets   |
| `make check-tintin` | a TinTin++ session (skipped if `tt++` is not installed)|
| `make check-worlds` | that each world file matches its JSON copy, and mkgame's checks on areas and coordinates |
| `make check-data`   | `mkgame`'s game directory, the game database and `upgradegame` (docs/data-files.md)|
| `make check-update` | `mkgame -u`, updating a game to a new revision of its world (`tools/updatetest.py`) |
| `make check-dmadmin` | `dmadmin`, and a DM it makes playing the editor in the console harness (`tools/dmadmintest.py`) |
| `make check-dump`   | that whole games dump exactly as the golden dumps in `tools/dumps/` (docs/data-files.md) |
| `make check-security` | that players can't crash, hang or corrupt the server, and that bad DM edits or data can't either (issues labelled `security`; `tools/securitytest.py`, `tools/dmtest.c`) |

See [deploy.md](deploy.md) for the production server.

`check-net` reports sanitizer errors in the server's output, so it is also
useful with a sanitizer build:

```sh
make clean
make CFLAGS='-g -O1 -fno-common -fsanitize=address,undefined' check-net
```

`-fno-common` matters: without it, clang leaves the global message buffers
(`B1`, `B2`, ...) as common symbols, which the address sanitizer doesn't
guard, and an overflow out of one goes unreported.

`check-security` needs such a build; add `-ftrivial-auto-var-init=pattern`
so that an uninitialized pointer crashes instead of happening to be NULL:

```sh
make clean
make CFLAGS='-g -O1 -fno-common -fsanitize=address,undefined -ftrivial-auto-var-init=pattern' check-security
```
