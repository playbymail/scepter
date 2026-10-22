#!/usr/bin/env python3
"""Byte-level tests of scepterd's telnet front end (telnet.c).

    python3 -I tools/nettest.py [SCEPTERD [MKGAME [WORLD [DMADMIN]]]]

Builds a game from WORLD (default var/alpha/alpha.world) in a temporary
directory, gives it a DM with DMADMIN, runs SCEPTERD on a free port of
127.0.0.1, and drives it over raw sockets.  It then restarts SCEPTERD with
-o twice, for GMCP's opaque room ids.  Sanitizer reports in the server's stderr fail the run, apart from
the known room.c Adjoin[6] read (#25).  `make check-net` runs this.
"""

import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

IAC, DONT, DO, WONT, WILL, SB, GA, SE = 255, 254, 253, 252, 251, 250, 249, 240
ECHO, SGA, TTYPE, NAWS, GMCP = 1, 3, 24, 31, 201
PROMPT_END = bytes([IAC, GA])

PORT = None
failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1
    return cond


class Client:
    def __init__(self, gmcp=False):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        self.buf = b""
        if gmcp:
            self.send(bytes([IAC, DO, GMCP]))

    def fill(self, timeout):
        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return True
        except ConnectionResetError:
            data = b""
        if not data:
            return False
        self.buf += data
        return True

    def expect(self, pattern, timeout=5.0):
        """Wait for regex pattern (bytes); return everything up to its end."""
        rx = re.compile(pattern, re.S)
        deadline = time.time() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                got, self.buf = self.buf[:m.end()], self.buf[m.end():]
                return got
            left = deadline - time.time()
            if left <= 0 or not self.fill(left):
                return None

    def quiet(self, pattern, wait=1.5):
        """True if pattern does not show up within wait seconds."""
        return self.expect(pattern, wait) is None

    def eof(self, timeout=5.0):
        """Wait for the server to close; return what arrived before."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.fill(deadline - time.time()):
                got, self.buf = self.buf, b""
                return got
        return None

    def send(self, data):
        self.sock.sendall(data if isinstance(data, bytes) else data.encode())

    def line(self, text):
        self.send(text + "\r\n")

    def abort(self):
        """Drop the connection with a reset, as a crashed client would."""
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                             b"\1\0\0\0\0\0\0\0")
        self.sock.close()

    def close(self):
        self.sock.close()


def prompt(text):
    return re.escape(text.encode()) + re.escape(PROMPT_END)


def login(name, all_at_once=False, gmcp=False):
    """Create a new first-level character; return the client in the game."""
    c = Client(gmcp)
    answers = [(prompt("Enter your name? "), name),
               (prompt("Class? "), "fighter"),
               (prompt("Stats? "), "15, 7, 9, 10, 9"),
               (prompt("Skill? "), "sharp"),
               (prompt("Enter access password? "), "secretpw"),
               (prompt("Finally, is your character a male or female? "), "male")]
    if all_at_once:
        c.send("".join(a + "\r\n" for _, a in answers))
        for p, _ in answers:
            if not c.expect(p):
                return None
    else:
        for p, a in answers:
            if not c.expect(p):
                return None
            c.line(a)
    c.welcome = c.expect(rb"Please welcome " + name.encode())
    if not c.welcome:
        return None
    return c


def test_telnet():
    c = Client()
    got = c.expect(prompt("Enter your name? "))
    check(got is not None, "connect: name prompt")
    if not got:
        return
    head = got[:-len(PROMPT_END)]
    check(b"\r\n" in head and not re.search(rb"[^\r]\n", head),
          "output lines end with CRLF")
    check(head.endswith(b"\r\nEnter your name? "),
          "prompt is sent without a newline, then IAC GA")

    c.send(bytes([IAC, DO, ECHO]))
    check(c.expect(re.escape(bytes([IAC, WONT, ECHO]))) is not None,
          "IAC DO ECHO is refused with IAC WONT ECHO")
    c.send(bytes([IAC, WILL, NAWS, IAC, DO, SGA]))
    check(c.expect(re.escape(bytes([IAC, DONT, NAWS]))) is not None,
          "IAC WILL NAWS is refused with IAC DONT NAWS")
    check(c.expect(re.escape(bytes([IAC, WONT, SGA]))) is not None,
          "IAC DO SGA is refused with IAC WONT SGA")
    c.send(bytes([IAC, WONT, ECHO, IAC, DONT, ECHO]))
    check(c.quiet(rb"\xff"), "IAC WONT/DONT get no reply")

    # subnegotiation and IAC IAC are skipped; the name still arrives
    c.send(bytes([IAC, SB, TTYPE, 0]) + b"xterm" + bytes([IAC, SE]))
    c.send(b"\r\n\r\n")		# empty lines are ignored at a prompt
    check(c.quiet(rb"."), "empty lines at a prompt are ignored")
    c.send(b"Sto" + bytes([IAC, IAC]) + b"p\r\n")
    check(c.eof() is not None, "'stop' at the name prompt disconnects")
    c.close()


def gmcp(package, data):
    return bytes([IAC, SB, GMCP]) + package.encode() + b" " + data + bytes([IAC, SE])


GOODBYE = re.escape(gmcp("Core.Goodbye", b'"Goodbye."'))


def test_gmcp():
    """#189: GMCP is offered, used once accepted, and never sent otherwise."""
    c = Client()
    got = c.expect(prompt("Enter your name? "))
    check(got is not None and got.startswith(bytes([IAC, WILL, GMCP])),
          "a new connection is offered IAC WILL GMCP")
    if not got:
        return
    c.send(bytes([IAC, DO, GMCP]))
    check(c.quiet(rb"\xff"), "IAC DO GMCP gets no reply")
    c.send(gmcp("Core.Hello", b'{"client": "nettest", "version": "1"}'))
    c.send(gmcp("Core.Supports.Set", b'[ "Room 1", "Char 1" ]'))
    check(c.quiet(rb"."), "Core.Hello and Core.Supports.Set get no reply")
    c.send(bytes([IAC, WILL, GMCP, IAC, DO, ECHO]))
    check(c.expect(re.escape(bytes([IAC, DONT, GMCP, IAC, WONT, ECHO])))
          is not None, "IAC WILL GMCP and other options are still refused")
    c.line("stop")
    got = c.eof()
    check(got is not None and re.search(GOODBYE, got) is not None,
          "a client that accepted GMCP gets Core.Goodbye")
    c.close()

    c = Client()
    c.expect(prompt("Enter your name? "))
    c.line("stop")
    got = c.eof()
    check(got is not None and bytes([IAC, SB]) not in got,
          "a client that never answered gets no GMCP")
    c.close()

    c = Client()
    c.expect(prompt("Enter your name? "))
    c.send(bytes([IAC, DONT, GMCP]))
    check(c.quiet(rb"\xff"), "IAC DONT GMCP gets no reply")
    c.line("stop")
    got = c.eof()
    check(got is not None and bytes([IAC, SB]) not in got,
          "a client that refused GMCP gets none")
    c.close()

    c = Client()
    c.expect(prompt("Enter your name? "))
    c.send(bytes([IAC, DO, GMCP, IAC, DONT, GMCP]))
    check(c.quiet(rb"\xff"), "IAC DO then DONT GMCP get no reply")
    c.send(bytes([IAC, DO, GMCP]))
    check(c.expect(re.escape(bytes([IAC, WILL, GMCP]))) is not None,
          "IAC DO GMCP after a DONT is agreed to with IAC WILL GMCP")
    c.line("stop")
    got = c.eof()
    check(got is not None and re.search(GOODBYE, got) is not None,
          "and GMCP is sent again")
    c.close()


ROOM_INFO = (re.escape(bytes([IAC, SB, GMCP])) + rb"Room\.Info (.*?)"
             + re.escape(bytes([IAC, SE])))


def room_info(c, timeout=5.0):
    """The next Room.Info c is sent, as a dict, or None."""
    got = c.expect(ROOM_INFO, timeout)
    m = got and re.search(ROOM_INFO, got, re.S)
    return json.loads(m.group(1)) if m else None


def no_room_info(c, wait=1.5):
    got = c.expect(ROOM_INFO, wait)
    return got is None


def quit(c):
    c.line("quit")
    c.expect(prompt("Are you sure? "))
    c.line("yes")
    c.eof()
    c.close()


def room(num, name, env, exits):
    return {"num": num, "name": name, "area": "alpha", "environment": env,
            "exits": exits}


def test_room_info():
    """#190: Room.Info follows a player around alpha."""
    a = login("Gmone", gmcp=True)
    check(a is not None, "Gmone logs in with GMCP")
    if not a:
        return
    # the login sends Room.Info before announcing the player
    m = re.search(ROOM_INFO, a.welcome, re.S)
    info = json.loads(m.group(1)) if m else None
    check(info == room(1, "Village square", "safe",
                       {"n": 2, "s": 4, "e": 3, "w": 5}),
          "Room.Info after login: %s" % info)
    got = a.expect(rb"Obvious exits are north, south, east and west\.\r\n")
    check(got is not None and bytes([IAC, SB]) not in got,
          "and only once")

    b = login("Gmtwo")
    check(b is not None, "Gmtwo logs in without GMCP")
    c = login("Gmthree", gmcp=True)
    check(c is not None, "Gmthree logs in with GMCP")
    if not b or not c:
        return
    c.line("follow gmone")
    check(c.expect(rb"Ok\.\r\n") is not None, "Gmthree follows Gmone")
    b.line("n")
    got = b.expect(rb"Obvious exits are north and south\.\r\n")
    check(got is not None and bytes([IAC, SB]) not in got,
          "a client that refused GMCP gets no Room.Info")

    a.line("n")
    north = room(2, "North road", "plain", {"n": 6, "s": 1})
    info = room_info(a)
    check(info == north, "Room.Info after a move: %s" % info)
    info = room_info(c)
    check(info == north, "Room.Info after following a leader: %s" % info)
    quit(b)
    quit(c)

    a.line("look")
    check(room_info(a) == north, "Room.Info on LOOK")
    a.line("n")
    info = room_info(a)
    check(info == room(6, "Forest edge", "plain",
                       {"n": 8, "s": 2, "e": 9, "go gate": 26}),
          "a door is an exit, taken with GO: %s" % info)
    a.line("n")
    info = room_info(a)
    check(info is not None and info["name"] == "You are deep in the forest",
          "an unnamed room is named from its description: %s"
          % (info and info["name"]))
    for step in ("s", "e", "u"):
        a.line(step)
        info = room_info(a)
    check(info == room(10, "Top of the ruined tower", "plain", {"d": 9}),
          "an invisible portal isn't an exit: %s" % info)
    a.line("go portal")
    info = room_info(a)
    check(info == room(12, "Quiet glade", "anti-magic", {"out": 6}),
          "Room.Info after GO PORTAL: %s" % info)
    a.line("use stones")
    info = room_info(a)
    check(info is not None and info["num"] == 1,
          "Room.Info after a teleport: %s" % info)

    dm = Client(gmcp=True)
    dm.expect(prompt("Enter your name? "))
    dm.line("Boss")
    dm.expect(prompt("Enter access password? "))
    dm.line("boss password")
    check(room_info(dm) == room(1, "Village square", "safe",
                                {"n": 2, "s": 4, "e": 3, "w": 5}),
          "the DM logs in with GMCP")
    dm.line("*edit")
    dm.line("modify player gmone ro=11")
    check(no_room_info(a), "a player moved into a dark room gets no Room.Info")
    a.line("look")
    got = a.expect(rb"too dark to see anything\.\r\n")
    check(got is not None and bytes([IAC, SB]) not in got,
          "nor on LOOK there")
    dm.line("modify player boss ro=11")
    info = room_info(dm)
    check(info == room(11, "Damp cellar", "dark", {"go door": 9}),
          "a DM moved into a dark room gets Room.Info: %s" % info)
    dm.line("end")
    quit(dm)
    quit(a)


def opaque_ids():
    """What two GMCP players are told rooms 1 and 2 are: two lists of
    (num, exits), or None."""
    seen = []
    for name in ("Opone", "Optwo"):
        c = login(name, gmcp=True)
        if not c:
            return None
        m = re.search(ROOM_INFO, c.welcome, re.S)
        c.line("n")
        info = room_info(c)
        quit(c)
        if not m or not info:
            return None
        first = json.loads(m.group(1))
        seen.append([(first["num"], first["exits"]),
                     (info["num"], info["exits"])])
    return seen


def test_two_players():
    a = login("Alpha")
    check(a is not None, "Alpha logs in")
    b = login("Bravo", all_at_once=True)
    check(b is not None, "Bravo logs in with all answers in one packet")
    if not a or not b:
        return None, None
    check(a.expect(rb"### Please welcome Bravo, the Fighter\.\r\n") is not None,
          "Alpha sees Bravo's arrival in the game")

    b.line('say "hello there"')
    check(a.expect(rb"Bravo says,.*hello there") is not None,
          "Alpha hears Bravo say")
    a.line('yell "over here"')
    check(b.expect(rb"Alpha yells,.*over here") is not None,
          "Bravo hears Alpha yell")

    b.send(b"say \"ab\x07c\x01d\x7fe\x80f\x1b\"\r\n")
    check(a.expect(rb"Bravo says,.*abcdef") is not None,
          "control characters are stripped from input")

    # 'say "' and 249 characters make 254, all that fits in the game's C1
    b.line('say "' + "y" * 300 + '"')
    got = a.expect(rb'Bravo says, "[y\r\n]*"\r\n')
    check(got is not None and got.count(b"y") - 1 == 249,
          "input lines are capped at 254 characters")

    b.line("n")
    check(a.expect(rb"Bravo just went north\.") is not None,
          "Alpha sees Bravo leave")
    b.line("s")
    check(a.expect(rb"Bravo just arrived\.") is not None,
          "Alpha sees Bravo arrive")

    a.line("who")
    got = a.expect(rb"Bravo\r\n")
    check(got is not None and b"Alpha" in got, "who lists both players")

    c = Client()
    c.expect(prompt("Enter your name? "))
    c.line("alpha")
    got = c.eof()
    check(got is not None and got.endswith(b"Name already in use!\r\n"),
          "a name in use is refused, then the connection closes")
    c.close()
    return a, b


def test_hangup(a, b):
    b.abort()
    check(a.expect(rb"### Bravo was devoured by US West Communications\.",
                   timeout=5) is not None,
          "an abrupt disconnect logs the player off")
    a.line("who")
    got = a.expect(rb"Alpha\r\n")
    check(got is not None and b"Bravo" not in got,
          "who no longer lists the lost player")
    c = login("Bravo")
    check(c is not None, "the lost player's name can log in again")
    if c:
        c.line("quit")
        c.expect(prompt("Are you sure? "))
        c.line("yes")
        got = c.eof()
        check(got is not None and got.endswith(b"End Scepter.\r\n"),
              "quit sends its text, then closes")
    check(a.expect(rb"### Bravo has quit adventuring\.") is not None,
          "the others hear of the quit")


def test_full(a):
    others = []
    for _ in range(11):
        c = Client()
        if not c.expect(prompt("Enter your name? ")):
            break
        others.append(c)
    check(len(others) == 11, "twelve connections are accepted")
    c = Client()
    got = c.eof()
    check(got == b"Too many users\r\n", "a 13th connection gets 'Too many users'")
    c.close()
    for c in others:
        c.abort()
    time.sleep(1)
    c = Client()
    check(c.expect(prompt("Enter your name? ")) is not None,
          "slots are reused once the connections drop")
    c.close()
    a.line("who")
    got = a.expect(rb"Alpha\r\n")
    check(got is not None and b"logging in" not in got,
          "dropped logins leave no one behind")


def test_warning(server, a):
    """#136: SIGUSR1 and SIGUSR2 warn every player of a restart or a reset."""
    server.send_signal(signal.SIGUSR1)
    got = a.expect(rb"\*\*\* The server restarts in 2 minutes\..*?\*\*\*\r\n")
    check(got is not None and b"Type SAVE now to keep your progress." in got
          and b"Reconnect in a minute or two." in got,
          "SIGUSR1 warns of a restart and tells players to SAVE")
    server.send_signal(signal.SIGUSR2)
    got = a.expect(rb"\*\*\* The game will be RESET in 2 minutes\..*?\*\*\*\r\n")
    check(got is not None and b"Every character will be deleted" in got
          and b"Type SAVE" not in got,
          "SIGUSR2 warns of a reset and doesn't suggest saving")
    a.line("who")
    check(a.expect(rb"Alpha\r\n") is not None, "and the game goes on")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start(scepterd, game, errlog, *opts):
    global PORT
    PORT = free_port()
    server = subprocess.Popen([scepterd, *opts, "-l", "127.0.0.1:%d" % PORT, game],
                              stdin=subprocess.DEVNULL,
                              stdout=subprocess.DEVNULL, stderr=errlog)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", PORT)).close()
            break
        except OSError:
            time.sleep(0.1)
    return server


def stop(server):
    if server.poll() is None:
        server.send_signal(signal.SIGTERM)
    try:
        return server.wait(10)
    except subprocess.TimeoutExpired:
        server.kill()
        return None


def test_opaque(scepterd, game, errlog):
    """#190: with -o, room ids are opaque, and the same for everyone and
    across a restart."""
    runs = []
    for _ in range(2):
        server = start(scepterd, game, errlog, "-o")
        try:
            runs.append(opaque_ids())
        except OSError as e:
            check(False, "the -o server is still there (%s)" % e)
            runs.append(None)
        check(stop(server) == 0, "scepterd -o exits cleanly")
    key = os.path.join(game, "roomkey")
    check(os.path.exists(key) and os.stat(key).st_mode & 0o777 == 0o600,
          "-o makes a room key that only the server can read")
    if not check(None not in runs, "Room.Info with -o"):
        return
    (one, two), other = runs[0][0], runs[0][1]
    check(one[0] not in (1, 2) and two[0] not in (1, 2) and one[0] != two[0],
          "-o sends opaque ids, not room numbers: %d, %d" % (one[0], two[0]))
    check(one[1]["n"] == two[0] and two[1]["s"] == one[0],
          "exits use the same ids")
    check(other == [one, two], "two players get the same ids")
    check(runs[1] == runs[0], "and so does a restart")


def main():
    scepterd = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./scepterd")
    mkgame = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "./mkgame")
    world = os.path.abspath(sys.argv[3] if len(sys.argv) > 3 else
                            "var/alpha/alpha.world")
    dmadmin = os.path.abspath(sys.argv[4] if len(sys.argv) > 4 else "./dmadmin")

    with tempfile.TemporaryDirectory() as tmp:
        game = os.path.join(tmp, "game")
        os.mkdir(game)  # mkgame never creates it
        subprocess.run([mkgame, world, game], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([dmadmin, game, "create", "Boss", "master"], check=True,
                       input=b"boss password\n",
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        errlog = open(os.path.join(tmp, "stderr"), "w+")
        server = start(scepterd, game, errlog)
        try:
            try:
                test_telnet()
                test_gmcp()
                test_room_info()
                a, b = test_two_players()
                if a:
                    test_hangup(a, b)
                    test_full(a)
                    test_warning(server, a)
                    server.send_signal(signal.SIGTERM)
                    got = a.eof()
                    check(got is not None and got.endswith(b"End - aborted\r\n"),
                          "SIGTERM tells players 'End - aborted', then closes")
            except OSError as e:
                check(False, "the server is still there (%s)" % e)
            check(stop(server) == 0, "scepterd exits cleanly")
            test_opaque(scepterd, game, errlog)
        finally:
            if server.poll() is None:
                server.kill()
            errlog.seek(0)
            report = errlog.read()

    bad = [l for l in report.splitlines()
           if re.search(r"runtime error|AddressSanitizer|LeakSanitizer", l)
           and not re.search(r"room\.c:\d+:\d+: runtime error: index 6 out of bounds", l)]
    if not check(not bad, "no new sanitizer reports"):
        print(report)
    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
