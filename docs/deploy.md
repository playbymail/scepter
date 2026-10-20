# Deploying to the droplet

The game runs on the DigitalOcean droplet (`ssh tsogira`) as
`scepter.service`.  Players connect with a MUD client or telnet to
`sceptish.pbbgaming.com` port 4000.  That name and `tsogira.pbbgaming.com`
both point at the droplet; the second is the Tsogira web app's site.

The droplet also runs two Go web applications, `dolpher` (127.0.0.1:8080)
and `tsogira` (127.0.0.1:8081), behind Caddy on ports 80 and 443.  The
Scepter server doesn't share anything with them: it has its own user,
directory and unit, and Caddy isn't involved.  Caddy proxies HTTP, and
telnet isn't HTTP.

| What          | Where                                         |
|---------------|-----------------------------------------------|
| Binaries      | `/usr/local/bin/scepterd`, `/usr/local/bin/mkgame` |
| World file    | `/usr/local/share/scepter/alpha.world`        |
| Game          | `/var/lib/scepter/alpha` (owned by `scepter`) |
| Unit          | `/etc/systemd/system/scepter.service` (from `deploy/`) |
| Logs          | `journalctl -u scepter`                       |

## Build

The droplet has no compiler.  Build in a container that matches its Ubuntu
release (26.04, x86-64), so the binaries link against the same glibc:

```sh
mkdir -p out
container run --rm --arch amd64 --dns 1.1.1.1 -v "$PWD":/src:ro -v "$PWD/out":/out \
	docker.io/library/ubuntu:26.04 sh -c '
	apt-get update -qq && apt-get install -y -qq build-essential python3 >/dev/null &&
	cp -r /src /w && cd /w && make clean && make && make check-net &&
	cp scepterd mkgame /out/'
```

## Upgrade

```sh
scp out/scepterd out/mkgame tsogira:/tmp/
ssh tsogira 'install -m 0755 /tmp/scepterd /tmp/mkgame /usr/local/bin/ &&
	rm /tmp/scepterd /tmp/mkgame && systemctl restart scepter'
```

The restart saves every player and tells them `End - aborted`, so they have
to reconnect.  The game directory is kept.

## First install

These steps were done once, on 2026-10-20:

```sh
scp out/scepterd out/mkgame var/alpha/alpha.world deploy/scepter.service tsogira:/tmp/
ssh tsogira
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

`mkgame` won't overwrite an existing game unless you pass `-f`, and with
`-f` it deletes every saved character.  Only change the world on purpose.

## Firewall

Port 4000/TCP must be open to the world.  The DigitalOcean cloud firewall
is managed from the control panel.  Telnet isn't encrypted, so passwords
cross the network in plain text.
