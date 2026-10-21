# Deploying to the droplet

The game runs on the DigitalOcean droplet (`ssh sceptish`) as
`scepter.service`.  Players connect with a MUD client or telnet to
`sceptish.pbbgaming.com` port 4000.  That name and `tsogira.pbbgaming.com`
both point at the droplet; the second is the Tsogira web app's site.

The droplet also runs two Go web applications, `dolpher` (127.0.0.1:8080)
and `tsogira` (127.0.0.1:8081), behind Caddy on ports 80 and 443.  The
Sceptish server doesn't share anything with them: it has its own user,
directory and unit.  Caddy proxies HTTP, and telnet isn't HTTP, so Caddy
only serves the game's web site (see below).

| What          | Where                                         |
|---------------|-----------------------------------------------|
| Binaries      | `/usr/local/bin/scepterd`, `mkgame`, `upgradegame`, `dmadmin` |
| World file    | `/usr/local/share/scepter/havenford.world`    |
| Game          | `/var/lib/scepter/havenford` (owned by `scepter`) |
| Unit          | `/etc/systemd/system/scepter.service` (from `deploy/`) |
| Logs          | `journalctl -u scepter`                       |
| Web site      | `/usr/local/share/scepter/www` (from `make site`) |
| Caddy site    | in `/etc/caddy/Caddyfile` (from `deploy/Caddyfile.sceptish`) |

## Build

The droplet has no compiler.  Build in a container that matches its Ubuntu
release (26.04, x86-64), so the binaries link against the same glibc:

```sh
mkdir -p out
container run --rm --arch amd64 --dns 1.1.1.1 -v "$PWD":/src:ro -v "$PWD/out":/out \
	docker.io/library/ubuntu:26.04 sh -c '
	apt-get update -qq && apt-get install -y -qq build-essential libcrypt-dev libsqlite3-dev python3 >/dev/null &&
	cp -r /src /w && cd /w && make clean && make && make check-net &&
	cp scepterd mkgame upgradegame dmadmin /out/'
```

`scepterd` links the system's libcrypt (libxcrypt) for password hashing.
Ubuntu always installs its runtime, `libcrypt1`, so the droplet needs no
extra package; the build container needs `libcrypt-dev`.

`scepterd`, `mkgame`, `upgradegame` and `dmadmin` also link the system's SQLite, for
the game database.  The droplet has the runtime, `libsqlite3-0` 3.46.1
(checked 2026-10-21), and the build container installs the same version's
`libsqlite3-dev`, so the binaries are built against the SQLite they run
with.  The macOS SDK's SQLite is newer, so the container build is what
catches a SQLite function the droplet doesn't have.

The droplet also has the `sqlite3` command (package `sqlite3`, installed
2026-10-21), to look inside a game.  Open it read-only, as `scepter`:

```sh
runuser -u scepter -- sqlite3 -readonly /var/lib/scepter/havenford/game.db
```

Change a running game only through the server; offline tools need it
stopped (docs/data-files.md).

## Upgrade

```sh
scp out/scepterd out/mkgame out/upgradegame out/dmadmin sceptish:/tmp/
ssh sceptish
cp -p /usr/local/bin/scepterd /usr/local/bin/mkgame /usr/local/bin/upgradegame \
	/usr/local/bin/dmadmin /usr/local/share/scepter/prev/    # for rolling back
systemctl kill --kill-whom=main --signal=SIGUSR1 scepter    # restart warning
sleep 120
systemctl stop scepter
install -m 0755 /tmp/scepterd /tmp/mkgame /tmp/upgradegame /tmp/dmadmin /usr/local/bin/
rm /tmp/scepterd /tmp/mkgame /tmp/upgradegame /tmp/dmadmin
runuser -u scepter -- upgradegame /var/lib/scepter/havenford
systemctl start scepter
```

Stopping the server saves every player and tells them `End - aborted`, so
they have to reconnect.  Warn them two minutes before (see below): use the
reset warning instead if the upgrade recreates the game.

## Warning players

Two minutes before stopping the server, warn the players.  Signal the
server with what is about to happen, wait two minutes, then stop it:

| Signal | When | The players are told |
|---|---|---|
| `SIGUSR1` | a restart: the game and its characters are kept | `*** The server restarts in 2 minutes. Type SAVE now to keep your progress. Characters below 2nd level can't be saved and will be lost. Reconnect in a minute or two. ***` |
| `SIGUSR2` | a reset: a new game replaces the old one (`mkgame`, a new world, #153) | `*** The game will be RESET in 2 minutes. Every character will be deleted, and saving won't keep it. You'll need to make a new character when it's back. ***` |

```sh
systemctl kill --kill-whom=main --signal=SIGUSR1 scepter    # or SIGUSR2
sleep 120
systemctl stop scepter
```

Ninety seconds after the first warning, the server warns again:
`*** Restarting in 30 seconds. Type SAVE now. ***` or
`*** Reset in 30 seconds. ***`.  Use `SIGUSR2` for anything that loses the
players' characters, even if the old game is kept on disk.  Never announce
a reset as a restart.  The warnings work only for telnet; `scepterd -c`
doesn't handle the signals.

`upgradegame` does nothing if the game's database is already at the new
build's schema version.  When the schema has changed (docs/data-files.md),
it first backs the database up to
`/var/lib/scepter/havenford/game.db.backup-*`.  If it refuses or fails, the
database is as it was.  Reinstall the previous binaries, or fix the
reported problem, before starting the server again.  The new `scepterd`
won't start on a game it can't read; check `journalctl -u scepter`.

To roll back after an upgrade: stop the server, copy the previous binaries
back from `/usr/local/share/scepter/prev/`, copy the backup over
`/var/lib/scepter/havenford/game.db`, and remove any `game.db-wal` and
`game.db-shm` beside it.  Anything saved since the upgrade is lost.

Upgrade history:

| Date | Commit | Format | Backup |
|---|---|---|---|
| 2026-10-20 | `860c2a4` | 0 → 1 (headers, #83) | `/var/lib/scepter/alpha.backup-20261020-212823` (deleted 2026-10-21) |
| 2026-10-21 | `45509f5` | 2 (password hashes, #82); game recreated with `mkgame`, since it held only test data | none |
| 2026-10-21 | `cf44452` | 2; switched from alpha to Havenford (see below) | `/var/lib/scepter/alpha` left in place |
| 2026-10-21 | `dc25fd4` | 2, unchanged (#105, #46, #53, #19, #16) | none |
| 2026-10-21 | `49498be` | 2, unchanged (#117, #49, #128, #41); game recreated with `mkgame -f` for the #128 room text, deleting every character | `/var/lib/scepter/havenford.backup-20261021-163121` |
| 2026-10-21 | `1404c7e` | game database, schema version 1 (#71, #146–#153); a new Havenford game, deleting every character (reset, after the `SIGUSR2` warning) | `/var/lib/scepter/havenford.structs-20261021` (the old struct-file game, untouched) |
| 2026-10-21 | `6e12f8e` | schema 1 → 2: the login limits in the database (#93); characters kept | `/var/lib/scepter/havenford/game.db.backup-20261021-201641` |
| 2026-10-21 | `9e957fc` | schema 2 → 3: the history of DM ranks (#139); `dmadmin` installed, and the first master DM, `MOTHELLO`, made with it; characters kept | `/var/lib/scepter/havenford/game.db.backup-20261021-224954` |

## DMs

`dmadmin` (docs/running.md) makes and manages the game's DMs.  Run it as
`scepter`; it records the account you logged in as.  Stop the server for
any change, with the restart warning if anyone is playing:

```sh
systemctl kill --kill-whom=main --signal=SIGUSR1 scepter    # restart warning
sleep 120
systemctl stop scepter
runuser -u scepter -- dmadmin /var/lib/scepter/havenford create NAME master
systemctl start scepter
```

`list` and `history` don't need the server stopped:

```sh
runuser -u scepter -- dmadmin /var/lib/scepter/havenford list
```

## The move to the game database (#153)

The first deploy with the game database replaces the struct-file game with
a new one, so every character is deleted (#71: reset, not converted).  The
old game directory is kept aside, untouched.  Build as above, then:

```sh
scp out/scepterd out/mkgame out/upgradegame var/havenford/havenford.world sceptish:/tmp/
ssh sceptish
systemctl kill --kill-whom=main --signal=SIGUSR2 scepter    # reset warning
sleep 120
systemctl stop scepter
cp -p /usr/local/bin/scepterd /usr/local/bin/mkgame /usr/local/bin/upgradegame \
	/usr/local/share/scepter/prev/    # for rolling back
install -m 0755 /tmp/scepterd /tmp/mkgame /tmp/upgradegame /usr/local/bin/
install -m 0644 /tmp/havenford.world /usr/local/share/scepter/
mv /var/lib/scepter/havenford /var/lib/scepter/havenford.structs-$(date +%Y%m%d)
install -d -m 0750 -o scepter -g scepter /var/lib/scepter/havenford
runuser -u scepter -- /usr/local/bin/mkgame \
	/usr/local/share/scepter/havenford.world /var/lib/scepter/havenford
systemctl start scepter
rm /tmp/scepterd /tmp/mkgame /tmp/upgradegame /tmp/havenford.world
```

The new `scepterd` refuses the old directory ("make a new game with
mkgame"), so it can't be started on it by mistake.  To roll back: stop the
server, copy the previous binaries back from `/usr/local/share/scepter/prev/`,
move the new `havenford` aside and the `havenford.structs-*` directory back
to `havenford`, and start the server.

## Changing the world

The server ran the alpha reference world until 2026-10-21, when it
switched to Havenford (var/havenford), the beta test world.  To switch
worlds, make a new game next to the old one and point the unit at it.
The old game, and every character in it, is left untouched, but the
players start again, so warn them with the reset warning:

```sh
scp var/havenford/havenford.world deploy/scepter.service sceptish:/tmp/
ssh sceptish
install -m 0644 /tmp/havenford.world /usr/local/share/scepter/
install -d -m 0750 -o scepter -g scepter /var/lib/scepter/havenford
runuser -u scepter -- /usr/local/bin/mkgame \
	/usr/local/share/scepter/havenford.world /var/lib/scepter/havenford
systemctl kill --kill-whom=main --signal=SIGUSR2 scepter    # reset warning
sleep 120
systemctl stop scepter
install -m 0644 /tmp/scepter.service /etc/systemd/system/
systemctl daemon-reload
systemctl start scepter
```

## First install

These steps were done once, on 2026-10-20, with the alpha world:

```sh
scp out/scepterd out/mkgame var/alpha/alpha.world deploy/scepter.service sceptish:/tmp/
ssh sceptish
install -m 0755 /tmp/scepterd /tmp/mkgame /usr/local/bin/
install -d /usr/local/share/scepter
install -m 0644 /tmp/alpha.world /usr/local/share/scepter/
useradd --system --home-dir /var/lib/scepter --no-create-home \
	--shell /usr/sbin/nologin --comment "Scepter server" scepter
install -d -m 0750 -o scepter -g scepter /var/lib/scepter
runuser -u scepter -- /usr/local/bin/mkgame \
	/usr/local/share/scepter/alpha.world /var/lib/scepter/alpha
install -m 0644 /tmp/scepter.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now scepter
```

`mkgame` never creates a directory: make the game directory first, empty,
with `install -d -m 0750 -o scepter -g scepter`, so only `scepter` can read
it.  `mkgame` refuses a directory that holds anything, so it never
overwrites or deletes a game; to replace one, move the old directory
aside.  (Until #147 `mkgame` made the directory itself and took `-f`; the
first install above used that.)

## Web site

`https://sceptish.pbbgaming.com/` is a static page for new players: how
to connect with Mudlet, and the Havenford map at `/map.html`.  The page
is `site/index.html`, and its hero image is `site/avatar.webp`, the
Havenford avatar lightened and resized to 800 pixels.  `make site` builds
the site into `out/site`, with the map copied from
`var/havenford/world-map.html` and the icon made from the avatar.  It
needs ImageMagick (`magick`).

To update the site:

```sh
make site
scp -r out/site sceptish:/tmp/sceptish-site
ssh sceptish
install -d -m 0755 /usr/local/share/scepter/www
install -m 0644 /tmp/sceptish-site/* /usr/local/share/scepter/www/
rm -r /tmp/sceptish-site
```

Caddy serves those files.  Its site block was added once, on 2026-10-21,
after the dolpher and tsogira sites, which stay unchanged:

```sh
scp deploy/Caddyfile.sceptish sceptish:/tmp/
ssh sceptish
cp -p /etc/caddy/Caddyfile /etc/caddy/Caddyfile.bak-$(date +%Y%m%d)
{ echo; cat /tmp/Caddyfile.sceptish; } >> /etc/caddy/Caddyfile
caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile
systemctl reload caddy
rm /tmp/Caddyfile.sceptish
```

Caddy gets the certificate for the name itself, over ports 80 and 443,
which are already open for the other sites.

## Firewall

Port 4000/TCP must be open to the world.  The DigitalOcean cloud firewall
is managed from the control panel.  Telnet isn't encrypted, so passwords
cross the network in plain text.
