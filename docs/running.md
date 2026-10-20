# Running the server

`scepterd` runs one game.  Players connect to it with a MUD client
(TinTin++, Mudlet, MUSHclient) or plain `telnet`.

## Build and make a game

```sh
make
./mkgame var/alpha/alpha.world games/alpha
```

`mkgame` compiles a world file (see [world-format.md](world-format.md)) into a
game directory.  The server reads and writes the files in that directory.
A newer `scepterd` may need the game converted first with
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

## Tests

| Command             | What it checks                                         |
|---------------------|--------------------------------------------------------|
| `make check-net`    | the telnet front end, byte by byte, over raw sockets   |
| `make check-tintin` | a TinTin++ session (skipped if `tt++` is not installed)|
| `make check-worlds` | that each world file matches its JSON copy             |
| `make check-data`   | data file headers and `upgradegame` (docs/data-files.md)|
| `make check-security` | that players can't crash, hang or corrupt the server (issues labelled `security`) |

See [deploy.md](deploy.md) for the production server.

`check-net` reports sanitizer errors in the server's output, so it is also
useful with a sanitizer build:

```sh
make clean
make CFLAGS='-g -O1 -fsanitize=address,undefined' check-net
```

`check-security` needs such a build; add `-ftrivial-auto-var-init=pattern`
so that an uninitialized pointer crashes instead of happening to be NULL:

```sh
make clean
make CFLAGS='-g -O1 -fsanitize=address,undefined -ftrivial-auto-var-init=pattern' check-security
```
