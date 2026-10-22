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
- The server refuses every telnet option, so the client echoes what you type
  and sends whole lines.
- While you are answering a question, messages from other players wait until
  you have answered.
- A player whose connection drops is logged off, and the others are told so.

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
