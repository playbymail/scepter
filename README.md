# Sceptish

Sceptish is a multi-user role-playing game for macOS and Linux, played with a
MUD client or `telnet`.  It is a modern implementation inspired by *Scepter
of Goth* (also known as *Milieu*), an early multi-user fantasy role-playing
game; David Wheeler's [history of Scepter of
Goth](https://dwheeler.com/scepter-of-goth/scepter-of-goth.html) tells its
story.

## History

Sceptish is not the original Scepter of Goth, nor a restoration of it: it is
our own implementation based on the set of QNX C files once circulated as
Scepter of Goth's source.  The QNX source is probably descended from John
Ryan's independent C translation of Alan Klietz's *Milieu*, subsequently ported
to QNX by Francis Keys for *Ghostwheel*/*Wintermute* and preserved by Shawn P. Stanley.

Klietz has [stated](https://github.com/shelches/scepter/issues/4#issue-1581465382)
that the QNX source is not the original 1984 QNX source of the game, but a partial
reimplementation written without access to it.

**We maintain Sceptish, and we alone are responsible for its code, changes,
behaviour, bugs and other shortcomings.**

## This repository

- `qnx/` holds the QNX files exactly as they were received.  Nothing in it
  is ever changed; it is the reference for how those files behaved.
- The top level is Sceptish, ported from them: `scepterd` (the server, with
  its own telnet front end), `mkgame` (builds a game from a world file),
  `upgradegame` (brings a game's database up to a newer schema) and
  `dmadmin` (makes and manages a game's DMs).  `mkgame -u` updates a game
  to a new revision of its world.

## Build

You need a C compiler, `make`, Python 3 for the tests, libxcrypt for
password hashing, and SQLite for the game database:

| System | Install |
|---|---|
| Ubuntu or Debian | `apt-get install build-essential libcrypt-dev libsqlite3-dev python3` |
| macOS | Xcode command line tools (they include SQLite), then `brew install libxcrypt` |

```sh
make
mkdir -p games/alpha                     # mkgame needs an empty directory
./mkgame var/alpha/alpha.world games/alpha
./scepterd games/alpha
```

Then connect a MUD client or `telnet` to `127.0.0.1` port 4000.

## Documentation

- [docs/running.md](docs/running.md): running a server, connecting, tests
- [docs/world-format.md](docs/world-format.md): writing world files
- [docs/data-files.md](docs/data-files.md): game data files and upgrading
- [docs/deploy.md](docs/deploy.md): the live game on the droplet
- [docs/how-to/update-an-online-character.md](docs/how-to/update-an-online-character.md):
  raise a character's level and give them equipment, as a DM
- [var/havenford/README.md](var/havenford/README.md): Havenford, the beta
  test and tutorial world

## Departures from the QNX files

Sceptish tries to play like the QNX files.  Bug fixes, tracked as GitHub
issues, change behaviour only where the QNX code crashed, corrupted memory
or could be abused.

**Passwords are the first deliberate break with the QNX files** (#82).
They stored a weighted sum of a password's capitalised letters.  It ignored case, allowed only one word of up to 10 letters, and
let many different passwords open the same character.  Sceptish instead:

- stores a bcrypt hash made by the system's libxcrypt, the game's first
  dependency beyond the C library;
- takes the whole line as the password, with leading and trailing spaces
  removed: 8 to 72 characters, spaces allowed, capitals different from
  small letters;
- asks for the old password and the new one twice in `CHANGEPW`;
- limits wrong passwords, at login and in `CHANGEPW`, for each address and
  each character.

The old hash is no longer accepted (#50): a locked character takes no
password until a master DM, or `dmadmin`, sets a new one
(docs/data-files.md).
