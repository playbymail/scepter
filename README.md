# Scepter of Goth

Scepter of Goth (also known as Milieu) is a multi-user role-playing game
written in C.  This version was ported from Pascal to C in the mid-1980s to
run on QNX.  Thanks to Shawn P. Stanley for supplying the source.

This repository ports that game to macOS and Linux:

- `qnx/` holds the QNX sources exactly as they were received.  Nothing in it
  is ever changed; it is the reference for how the game behaved.
- The top level is the port: `scepterd` (the server, with its own telnet
  front end), `mkgame` (builds a game from a world file) and `upgradegame`
  (converts a game to a newer data format).

## Build

You need a C compiler, `make`, Python 3 for the tests, and libxcrypt for
password hashing:

| System | Install |
|---|---|
| Ubuntu or Debian | `apt-get install build-essential libcrypt-dev python3` |
| macOS | Xcode command line tools, then `brew install libxcrypt` |

```sh
make
./mkgame var/alpha/alpha.world games/alpha
./scepterd games/alpha
```

Then connect a MUD client or `telnet` to `127.0.0.1` port 4000.

## Documentation

- [docs/running.md](docs/running.md): running a server, connecting, tests
- [docs/world-format.md](docs/world-format.md): writing world files
- [docs/data-files.md](docs/data-files.md): game data files and upgrading
- [docs/deploy.md](docs/deploy.md): the live game on the droplet

## Departures from the original

The port tries to play like the QNX game.  Bug fixes, tracked as GitHub
issues, change behaviour only where the original crashed, corrupted memory
or could be abused.

**Passwords are the first deliberate break with the original source**
(#82).  The original stored a weighted sum of a password's capitalised
letters.  It ignored case, allowed only one word of up to 10 letters, and
let many different passwords open the same character.  The port instead:

- stores a bcrypt hash made by the system's libxcrypt, the game's first
  dependency beyond the C library;
- takes the whole line as the password, with leading and trailing spaces
  removed: 8 to 72 characters, spaces allowed, capitals different from
  small letters;
- asks for the old password and the new one twice in `CHANGEPW`;
- limits wrong passwords, at login and in `CHANGEPW`, for each address and
  each character.

The player record gained a field for the hash, so the data format is now
version 2.  `upgradegame` converts older games, keeping a backup; nothing
converts a game back.  A character saved before the change logs in once
more with its old password, which is then hashed exactly as typed.
