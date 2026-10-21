#!/usr/bin/env python3
"""Regression tests for the player-reachable security bugs.

    python3 -I tools/securitytest.py [SCEPTERD [MKGAME [WORLD [TEST...]]]]

Each test builds a fresh game from WORLD (default var/alpha/alpha.world).
Player tests run SCEPTERD on a free port of 127.0.0.1 and play the scenario
from an issue over raw sockets.  DM and bad-data tests run tools/dmtest.c,
the dmtest binary beside SCEPTERD, which drives the engine directly.  A test
fails if the server crashes, stops answering, or prints a sanitizer report
(apart from the known room.c Adjoin[6] read, #25).  Name tests (e.g. 55 8)
to run only those.  `make check-security` runs this; use a sanitizer build.

Most scenarios use the wizards' workshop, room 27 of the alpha world.
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import time

IAC, GA = 255, 249
PROMPT_END = bytes([IAC, GA])

failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1
    return cond


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
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

    def line(self, text):
        self.sock.sendall((text + "\r\n").encode())

    def close(self):
        self.sock.close()


def prompt(text):
    return re.escape(text.encode()) + re.escape(PROMPT_END)


def login(port, name):
    """Create a new first-level fighter; return the client in the game."""
    c = Client(port)
    for p, a in [(prompt("Enter your name? "), name),
                 (prompt("Class? "), "fighter"),
                 (prompt("Stats? "), "15, 7, 9, 10, 9"),
                 (prompt("Skill? "), "sharp"),
                 (prompt("Enter access password? "), "secretpw"),
                 (prompt("Finally, is your character a male or female? "), "male")]:
        if not c.expect(p):
            return None
        c.line(a)
    if not c.expect(rb"Please welcome " + name.encode() + rb".*?You are "):
        return None
    return c


def enter(port, name, password):
    """Log in as saved character name; return the client and what it got."""
    c = Client(port)
    if not c.expect(prompt("Enter your name? ")):
        return c, None
    c.line(name)
    if not c.expect(prompt("Enter your password? ")):
        return c, None
    c.line(password)
    return c, c.expect(rb"You are |Wrong password, sorry\.|Too many wrong passwords.*?\r\n")


def quit_game(c):
    c.line("quit")
    c.expect(prompt("Are you sure? "), 5)
    c.line("yes")
    return c.expect(rb"End Scepter", 5) is not None


def walk(c, *dirs):
    for d in dirs:
        c.line(d)
        if not c.expect(rb"You are ", 5):
            return False
    return True


TO_WORKSHOP = ("s", "s", "e", "s", "e")		# rooms 1, 4, 13, 14, 18, 27


def alive(c, name, what):
    """The server still answers c, who is playing name."""
    c.line("who")
    return check(c.expect(rb"Name\r\n.*" + name.encode(), 5) is not None,
                 "the server still answers after " + what)


def test_55(port):
    """POLYMORPH with no template in MList 30-102 must not hang (#55)."""
    a = login(port, "Alpha")
    if not check(a and walk(a, *TO_WORKSHOP), "Alpha reaches the workshop"):
        return
    a.line("use orb wisp")
    check(a.expect(rb"Nothing happens\.", 5) is not None,
          "polymorph with no templates: nothing happens")
    alive(a, "Alpha", "polymorph with no templates")


def test_39(port):
    """POLYMORPH at a monster with MaxHits 0 must not divide by zero (#39)."""
    a = login(port, "Alpha")
    if not check(a and walk(a, *TO_WORKSHOP), "Alpha reaches the workshop"):
        return
    a.line("use orb wisp")
    check(a.expect(rb"marsh wisp transforms into a newt!", 5) is not None,
          "the MaxHits 0 wisp turns into a newt")
    alive(a, "Alpha", "polymorphing a MaxHits 0 monster")


def test_8(port):
    """PSI BLAST at a player must not touch a NULL monster (#8)."""
    a = login(port, "Alpha")
    b = login(port, "Bravo")
    if not check(a and b and walk(a, *TO_WORKSHOP) and walk(b, *TO_WORKSHOP),
                 "Alpha and Bravo reach the workshop"):
        return
    a.line("use prism bravo")
    check(b.expect(rb"Alpha casts a psi blast spell on you!", 5) is not None,
          "Bravo is psi blasted")
    alive(a, "Alpha", "psi blasting a player")


def test_11(port):
    """Dying with no money must not link a garbage pointer (#11)."""
    a = login(port, "Alpha")
    b = login(port, "Bravo")
    if not check(a and b and walk(b, *TO_WORKSHOP), "Bravo waits in the workshop"):
        return
    walk(a, "s")
    a.line("buy 6")					# the ash wand costs all 200 shillings
    check(a.expect(rb"Here you are, sir", 5) is not None, "Alpha spends everything")
    a.line("buy 1")
    check(a.expect(rb"You don't have enough cash", 5) is not None,
          "Alpha has no money")
    if not check(walk(a, *TO_WORKSHOP[1:]), "Alpha reaches the workshop"):
        return
    a.line("open chest")
    check(b.expect(rb"Alpha was just killed by a deadly trap", 5) is not None,
          "the trapped chest kills Alpha")
    # a dead player who makes the high score list writes an obituary first
    got = a.expect(rb"In one line|you find\.\.\.\.|End Scepter", 5)
    if got and got.endswith(b"In one line"):
        a.line("He opened the wrong chest.")
        got = a.expect(rb"you find\.\.\.\.|End Scepter", 5)
    check(got is not None, "Alpha's death is over")
    b.line("look")
    got = b.expect(rb"You are in the wizards.*?\r\n\r\n", 5)
    check(got and re.search(rb"Alpha's\s+body", got), "Bravo sees Alpha's body")
    alive(b, "Bravo", "a player dies with no money")


def test_13(port):
    """GET coins from a chest must not read the freed coins (#13)."""
    a = login(port, "Alpha")
    if not check(a and walk(a, "w"), "Alpha reaches the temple"):
        return
    a.line("get key")
    check(a.expect(rb"Ok\.", 5) is not None, "Alpha takes the brass key")
    if not check(walk(a, "e", "e", "u"), "Alpha reaches the inn's loft"):
        return
    a.line("unlock chest key")
    check(a.expect(rb"Ok\.", 5) is not None, "Alpha unlocks the chest")
    a.line("get gold chest")
    check(a.expect(rb"You now have 250 shillings", 5) is not None,
          "Alpha takes the coins from the chest")
    alive(a, "Alpha", "taking coins from a chest")


def test_14(port):
    """One ring worn on both hands must not dangle once pawned (#14)."""
    a = login(port, "Alpha")
    if not check(a and walk(a, *TO_WORKSHOP), "Alpha reaches the workshop"):
        return
    a.line("get ring")
    a.line("wear ring left")
    a.line("wear ring right")
    check(a.expect(rb"You put on the ring\..*You put on the ring\.", 5)
          is not None, "Alpha wears the ring left, then right")
    if not check(walk(a, "w", "n", "w", "n", "w"), "Alpha reaches the pawnshop"):
        return
    a.line("pawn ring")
    a.expect(rb"Is this to your satisfaction", 5)
    a.line("yes")
    check(a.expect(rb"Thank you!  Come again!", 5) is not None,
          "Alpha pawns the ring")
    a.line("look alpha")
    alive(a, "Alpha", "pawning a ring worn on both hands")
    a.line("inv")
    check(a.expect(rb"You are carrying nothing at all", 5) is not None,
          "the pawned ring is gone")


def test_15(port):
    """A spell must not change what a pending PARLEY sells (#15)."""
    a = login(port, "Alpha")
    b = login(port, "Bravo")
    if not check(b and walk(b, *TO_WORKSHOP), "Bravo reaches the workshop"):
        return
    b.line("get wand")
    check(b.expect(rb"Ok\.", 5) is not None, "Bravo takes the bone wand")
    if not check(a and walk(a, "n") and walk(b, "w", "n", "w", "n", "n", "n"),
                 "Alpha and Bravo meet on the north road"):
        return
    a.line("parley peddler")
    got = a.expect(rb"I will sell you (an? |some )?(.*?) for a mere", 5)
    if not check(got is not None, "the peddler offers Alpha something"):
        return
    item = re.search(rb"I will sell you (?:an? |some )?(.*?) for a mere", got).group(1)
    b.line("use wand alpha")
    check(b.expect(rb"Ok\.", 5) is not None, "Bravo uses the wand on Alpha")
    a.line("y")		# Alpha hears of the spell once the prompt is answered
    check(a.expect(rb"Bravo casts a befuddle spell on you.*Here you are, my friend",
                   5) is not None, "Alpha, befuddled, buys it")
    a.line("look")				# room 2 has two descriptions
    got = a.expect(rb"Obvious exits are north and south\.\r\nYou see.*?\r\n\r\n", 5)
    check(got is not None and item in re.sub(rb"\s+", b" ", got),
          "the room holds the %s Alpha was offered" % item.decode())
    alive(a, "Alpha", "a spell during PARLEY")


def test_12(port):
    """A spring door into an unloaded segment, with every slot in use (#12)."""
    a = login(port, "Alpha")
    # Rooms 30, 40 ... 140 load segments 3-14, so with segment 0 every slot
    # is in use and going to room 150 must unload one.  GoDirection loads
    # the new segment while Alpha still holds the old one, so the old room
    # and its door stay valid; this guards that order.
    if not check(a and walk(a, *["d"] * 13), "Alpha tours segments 3-14"):
        return
    a.line("go door")
    check(a.expect(rb"oak door slams shut!", 5) is not None,
          "Alpha goes through the spring door")
    walk(a, "u")
    a.line("look")
    got = a.expect(rb"You are in the village square.*?\r\n\r\n", 5)
    check(got is not None and b"oak door" in got, "the oak door is still there")
    alive(a, "Alpha", "a spring door into an unloaded segment")


def test_82_new(port):
    """A new character's password is 8 to 72 characters (#82)."""
    c = Client(port)
    for p, a in [(prompt("Enter your name? "), "Alpha"),
                 (prompt("Class? "), "fighter"),
                 (prompt("Stats? "), "15, 7, 9, 10, 9"),
                 (prompt("Skill? "), "sharp")]:
        if not check(c.expect(p) is not None, "Alpha is asked " + p.decode(errors="replace")[:20]):
            return
        c.line(a)
    check(c.expect(rb"Choose a password of 8 to 72 characters.*" + prompt("Enter access password? "))
          is not None, "Alpha is told the password rules")
    c.line("short")
    check(c.expect(rb"Passwords must be 8 to 72 characters long\.\r\n.*"
                   + prompt("Enter access password? ")) is not None,
          "a 5-character password is refused")
    c.line("x" * 73)
    check(c.expect(rb"Passwords must be 8 to 72 characters long\.\r\n.*"
                   + prompt("Enter access password? ")) is not None,
          "a 73-character password is refused")
    c.line("  open sesame  ")
    check(c.expect(prompt("Finally, is your character a male or female? ")) is not None,
          "a password with spaces is taken")


def test_82_legacy(port):
    """An old password works once, then only exactly as typed (#82)."""
    # Oldtimer's old password is "ac"; the old hash took "CB" too
    c, got = enter(port, "Oldtimer", "AC")
    check(got and got.endswith(b"You are "), "Oldtimer logs in with the old password")
    check(c.expect(rb"Passwords now tell capital letters from small ones", 5) is not None,
          "Oldtimer is told the password now has case")
    check(c.expect(rb"use CHANGEPW to choose one of at least 8", 5) is not None,
          "and is asked for a longer one")
    check(quit_game(c), "Oldtimer quits, saving the new hash")
    c, got = enter(port, "Oldtimer", "CB")
    check(got and b"Wrong password" in got, "a colliding password no longer works")
    c, got = enter(port, "Oldtimer", "ac")
    check(got and b"Wrong password" in got, "nor the old one in another case")
    c, got = enter(port, "Oldtimer", "   AC   ")
    check(got and got.endswith(b"You are "), "the password as typed works, spaces trimmed")
    alive(c, "Oldtimer", "logging in")


def test_82_change(port):
    """CHANGEPW asks for the old password and the new one twice (#82)."""
    c, got = enter(port, "Oldtimer", "ac")
    if not check(got and got.endswith(b"You are "), "Oldtimer logs in"):
        return
    steps = [
        (["wrong guess"], rb"Wrong password, sorry\.", "a wrong old password is refused"),
        (["ac", "short"], rb"Passwords must be 8 to 72 characters long\.  Password not changed\.",
         "a short new password is refused"),
        (["ac", "open sesame", "open sesamE"], rb"The passwords don't match\.",
         "a mistyped repeat is refused"),
        (["ac", "open sesame", "open sesame"], rb"PW change complete\.",
         "the password is changed"),
    ]
    prompts = [prompt("Enter your password? "), prompt("Enter new password? "),
               prompt("Enter the new password again? ")]
    for answers, want, what in steps:
        c.line("changepw")
        ok = True
        for p, a in zip(prompts, answers):
            ok = ok and c.expect(p, 5) is not None
            c.line(a)
        check(ok and c.expect(want, 5) is not None, what)
    check(quit_game(c), "Oldtimer quits")
    c, got = enter(port, "Oldtimer", "open sesame")
    check(got and got.endswith(b"You are "), "the new password works")
    check(quit_game(c), "Oldtimer quits again")
    c, got = enter(port, "Oldtimer", "ac")
    check(got and b"Wrong password" in got, "the old one does not")


def test_82_limit(port):
    """Five wrong passwords from an address refuse it for 10 minutes (#82)."""
    for i in range(5):
        c, got = enter(port, "Oldtimer", "guess %d" % i)
        check(got and b"Wrong password" in got, "wrong password %d is refused" % (i + 1))
    c, got = enter(port, "Oldtimer", "ac")
    check(got and b"Too many wrong passwords.  Try again in 10 minutes." in got,
          "then even the right one is refused for 10 minutes")
    with open(os.path.join(GAME, "dat", "errlog")) as f:
        log = f.read()
    check(log.count("Wrong password for Oldtimer from 127.0.0.1") == 5
          and "address refused for 10 minutes" in log, "errlog records the failures")


def test_92(port):
    """Wrong old passwords at CHANGEPW count against the login limits (#92)."""
    c, got = enter(port, "Oldtimer", "ac")
    if not check(got and got.endswith(b"You are "), "Oldtimer logs in"):
        return
    for i in range(5):
        c.line("changepw")
        ok = c.expect(prompt("Enter your password? "), 5) is not None
        c.line("guess %d" % i)
        check(ok and c.expect(rb"Wrong password, sorry\.", 5) is not None,
              "wrong old password %d is refused" % (i + 1))
    c.line("changepw")
    check(c.expect(rb"Too many wrong passwords\.  Try again in 10 minutes\.", 5)
          is not None, "then CHANGEPW is refused for 10 minutes")
    c.line("ac")
    check(c.expect(rb"Enter new password\? ", 2) is None,
          "without asking for the old password")
    alive(c, "Oldtimer", "the refusal")
    check(quit_game(c), "Oldtimer quits")
    c, got = enter(port, "Oldtimer", "ac")
    check(got and b"Too many wrong passwords.  Try again in 10 minutes." in got,
          "and logging in again from that address is refused too")
    with open(os.path.join(GAME, "dat", "errlog")) as f:
        log = f.read()
    check(log.count("Wrong password for Oldtimer from 127.0.0.1") == 5
          and "address refused for 10 minutes" in log, "errlog records the failures")


def legacy(dmtest, game):
    """Save Oldtimer, a 2nd-level character with the old password hash of "ac"."""
    subprocess.run([dmtest, game, "legacy", "Oldtimer", "ac"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def many_segments(world):
    """World with rooms 30-150, one per segment, and a spring door to 150."""
    world = world.replace("[room 1]\n", "[room 1]\ndown = 30\n", 1)
    rooms = ["[room %d]\ndown = %d\ndesc = 1:3\n" % (r, r + 10 if r < 140 else 1)
             for r in range(30, 150, 10)]
    rooms.append("[room 150]\nup = 1\ndesc = 1:3\n")
    rooms.append("[room 1 / object 3]\nname = door,oak *\narticle = an\n"
                 "kind = door\npermanent = yes\nto = 150\nspring = yes\n")
    return world + "\n" + "\n".join(rooms)


def drop_section(world, header):
    """World text without the [header] section."""
    out, skip = [], False
    for line in world.splitlines(keepends=True):
        if line.startswith("["):
            skip = line.strip() == "[%s]" % header
        if not skip:
            out.append(line)
    return "".join(out)


# test name -> (function, world transform, game setup)
TESTS = {
    "55": (test_55, lambda w: drop_section(w, "monster 30"), None),
    "39": (test_39, None, None),
    "8": (test_8, None, None),
    "11": (test_11, None, None),
    "13": (test_13, None, None),
    "14": (test_14, None, None),
    "15": (test_15, None, None),
    "12": (test_12, many_segments, None),
    "82-new": (test_82_new, None, None),
    "82-legacy": (test_82_legacy, None, legacy),
    "82-change": (test_82_change, None, legacy),
    "82-limit": (test_82_limit, None, legacy),
    "92": (test_92, None, legacy),
}
GAME = None


# DM-only and bad-data tests, run by tools/dmtest.c
DMTESTS = ["20", "23", "22", "24", "26", "82", "88"]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def sanitizer_ok(report):
    bad = [l for l in report.splitlines()
           if re.search(r"runtime error|AddressSanitizer|LeakSanitizer", l)
           and not re.search(r"room\.c:\d+:\d+: runtime error: index 6 out of bounds", l)]
    if not check(not bad, "no new sanitizer reports"):
        print(report)


def run_dm(name, dmtest, mkgame, world):
    print("# #%s: dmtest %s" % (name, name))
    with tempfile.TemporaryDirectory() as tmp:
        game = os.path.join(tmp, "game")
        subprocess.run([mkgame, world, game], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            p = subprocess.run([dmtest, game, name], stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                               timeout=30)
            report, rc = p.stderr.decode(errors="replace"), p.returncode
        except subprocess.TimeoutExpired as e:
            report, rc = (e.stderr or b"").decode(errors="replace"), "timeout"
    for l in report.splitlines():
        if l.startswith(("ok   ", "FAIL ")):
            check(l.startswith("ok"), l[5:])
    check(rc == 0, "dmtest %s exits cleanly (%s)" % (name, rc))
    sanitizer_ok(report)


def run(name, scepterd, mkgame, world, dmtest):
    global GAME
    fn, transform, setup = TESTS[name]
    print("# #%s: %s" % (name, fn.__doc__))
    with tempfile.TemporaryDirectory() as tmp:
        if transform:
            with open(world) as f:
                text = transform(f.read())
            world = os.path.join(tmp, "test.world")
            with open(world, "w") as f:
                f.write(text)
        game = os.path.join(tmp, "game")
        subprocess.run([mkgame, world, game], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if setup:
            setup(dmtest, game)
        GAME = game
        port = free_port()
        errlog = open(os.path.join(tmp, "stderr"), "w+")
        server = subprocess.Popen([scepterd, "-l", "127.0.0.1:%d" % port, game],
                                  stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL, stderr=errlog)
        try:
            for _ in range(50):
                try:
                    socket.create_connection(("127.0.0.1", port)).close()
                    break
                except OSError:
                    time.sleep(0.1)
            try:
                fn(port)
            except OSError as e:
                check(False, "the server is still there (%s)" % e)
            check(server.poll() is None, "scepterd is still running")
        finally:
            if server.poll() is None:
                server.kill()
            server.wait()
            errlog.seek(0)
            report = errlog.read()
    sanitizer_ok(report)


def main():
    scepterd = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./scepterd")
    mkgame = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "./mkgame")
    world = os.path.abspath(sys.argv[3] if len(sys.argv) > 3 else
                            "var/alpha/alpha.world")
    dmtest = os.path.join(os.path.dirname(scepterd), "dmtest")
    for name in sys.argv[4:] or list(TESTS) + DMTESTS:
        if name in TESTS:
            run(name, scepterd, mkgame, world, dmtest)
        elif name in DMTESTS:
            run_dm(name, dmtest, mkgame, world)
        else:
            check(False, "there is a test %s" % name)
    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
