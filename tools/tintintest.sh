#!/bin/sh
# Play scepterd with a real MUD client: TinTin++ (tt++), run headless.
#
#	tools/tintintest.sh [SCEPTERD [MKGAME [WORLD]]]
#
# Creates a character through the login prompts, talks, moves and quits,
# then checks TinTin++'s session log.  `make check-tintin` runs this; it is
# skipped if tt++ is not installed.

SCEPTERD=${1:-./scepterd}
MKGAME=${2:-./mkgame}
WORLD=${3:-var/alpha/alpha.world}
TT=${TT:-$(command -v tt++ || echo /opt/homebrew/bin/tt++)}

if [ ! -x "$TT" ]; then
	echo "skip: tt++ not found"
	exit 0
fi

SCEPTERD=$(cd "$(dirname "$SCEPTERD")" && pwd)/$(basename "$SCEPTERD")
TMP=$(mktemp -d)
trap 'kill $PID 2>/dev/null; rm -rf "$TMP"' EXIT

"$MKGAME" "$WORLD" "$TMP/game" >/dev/null 2>&1 || exit 1
PORT=$(python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
"$SCEPTERD" -l 127.0.0.1:$PORT "$TMP/game" 2>"$TMP/server.log" &
PID=$!
sleep 1

# A #log set at startup would log only TinTin++'s startup session.
cd "$TMP" && "$TT" -G -H -e "
#event {SESSION CONNECTED} {#log overwrite out.log};
#event {SESSION DISCONNECTED} {#end};
#action {Enter your name?} {Tintin};
#action {Class?} {fighter};
#action {Stats?} {15, 7, 9, 10, 9};
#action {Skill?} {sharp};
#action {Enter access password?} {secretpw};
#action {male or female?} {male};
#action {Please welcome Tintin} {say \"hello\";who;n;s;quit};
#action {Are you sure?} {yes};
#delay {20} {#end};
#session s 127.0.0.1 $PORT" </dev/null >tt.out 2>&1

status=0
for want in 'Enter your name? ' 'Please welcome Tintin, the Fighter.' \
	' 1   0:0.  Tintin' 'Obvious exits are north and south.' \
	'Are you sure? ' 'End Scepter.'
do
	if grep -q -- "$want" out.log; then
		echo "ok   $want"
	else
		echo "FAIL $want"
		status=1
	fi
done
if grep -q "$(printf '\r')" out.log; then
	echo "FAIL carriage returns left in the log"
	status=1
fi
[ $status = 0 ] || cat out.log
exit $status
