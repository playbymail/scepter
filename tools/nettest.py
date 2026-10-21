#!/usr/bin/env python3
"""Byte-level tests of scepterd's telnet front end (telnet.c).

    python3 -I tools/nettest.py [SCEPTERD [MKGAME [WORLD]]]

Builds a game from WORLD (default var/alpha/alpha.world) in a temporary
directory, runs SCEPTERD on a free port of 127.0.0.1, and drives it over raw
sockets.  Sanitizer reports in the server's stderr fail the run, apart from
the known room.c Adjoin[6] read (#25).  `make check-net` runs this.
"""

import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

IAC, DONT, DO, WONT, WILL, SB, GA, SE = 255, 254, 253, 252, 251, 250, 249, 240
ECHO, SGA, TTYPE, NAWS = 1, 3, 24, 31
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
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        self.buf = b""

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


def login(name, all_at_once=False):
    """Create a new first-level character; return the client in the game."""
    c = Client()
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
    if not c.expect(rb"Please welcome " + name.encode()):
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


def main():
    global PORT
    scepterd = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./scepterd")
    mkgame = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "./mkgame")
    world = os.path.abspath(sys.argv[3] if len(sys.argv) > 3 else
                            "var/alpha/alpha.world")

    with tempfile.TemporaryDirectory() as tmp:
        game = os.path.join(tmp, "game")
        os.mkdir(game)  # mkgame never creates it
        subprocess.run([mkgame, world, game], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        PORT = free_port()
        errlog = open(os.path.join(tmp, "stderr"), "w+")
        server = subprocess.Popen([scepterd, "-l", "127.0.0.1:%d" % PORT, game],
                                  stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL, stderr=errlog)
        try:
            for _ in range(50):
                try:
                    socket.create_connection(("127.0.0.1", PORT)).close()
                    break
                except OSError:
                    time.sleep(0.1)
            try:
                test_telnet()
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
            check(server.wait(10) == 0, "scepterd exits cleanly")
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
