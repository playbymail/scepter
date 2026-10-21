#!/usr/bin/env python3
"""Tests of tools/dmadmin, which manages a game's DMs (#139), and of a DM
it makes logging in through the console harness (#70).

    python3 -I tools/dmadmintest.py [BINDIR [WORLD]]

BINDIR holds scepterd, mkgame, dmadmin, dmtest and dumpgame (default: .).
Each case builds a game from WORLD (default var/alpha/alpha.world) in a
temporary directory.  A refusal must change nothing, as dumpgame sees it.
`make check-dmadmin` runs this.
"""

import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile


BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
WORLD = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "var/alpha/alpha.world")
TMP = tempfile.mkdtemp()
failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1
    return cond


def run(prog, *args, stdin="", env=None):
    e = dict(os.environ)
    e.pop("SUDO_USER", None)
    e.update(env or {})
    p = subprocess.run([os.path.join(BIN, prog)] + list(args), input=stdin,
                       capture_output=True, text=True, timeout=60, env=e)
    return p.returncode, p.stdout, p.stderr


def game(name, test=None):
    path = os.path.join(TMP, name)
    os.mkdir(path)  # mkgame never creates it
    rc, _, err = run("mkgame", WORLD, path)
    assert rc == 0, err
    if test:
        rc, _, err = run("dmtest", path, test)
        assert rc == 0, err
    return path


def dmadmin(g, *args, stdin="", env=None):
    return run("dmadmin", g, *args, stdin=stdin, env=env)


def dump(g):
    return run("dumpgame", g)[1]


def rows(g, sql):
    db = sqlite3.connect(os.path.join(g, "game.db"))
    try:
        return db.execute(sql).fetchall()
    finally:
        db.close()


def history(g):
    return rows(g, "SELECT player, old_rank, new_rank, by_whom, via FROM rank_changes"
                   " ORDER BY id")


def play(g, lines):
    """Play lines through the console harness; its output."""
    _, out, _ = run("scepterd", "-c", g, stdin="".join(l + "\n" for l in lines))
    return out


def refuses(g, args, stdin, words, what):
    """dmadmin args fails, saying words, and changes nothing."""
    before = dump(g)
    rc, out, err = dmadmin(g, *args, stdin=stdin)
    check(rc != 0 and all(w in err for w in words) and dump(g) == before,
          what + " (%s)" % err.strip().replace("dmadmin: ", ""))


def test_usage():
    g = game("usage")
    for args in ([], ["list", "x"], ["create", "Boss"], ["rank", "Boss"],
                 ["password"], ["remove"], ["promote", "Boss"]):
        rc, _, err = dmadmin(g, *args)
        check(rc == 2 and "usage" in err, "usage: dmadmin GAMEDIR %s" % " ".join(args))
    rc, _, err = run("dmadmin", os.path.join(TMP, "none"), "list")
    check(rc == 1, "a missing game directory is refused")


def test_create():
    g = game("create")
    rc, out, _ = dmadmin(g, "list")
    check(rc == 0 and out == "no DMs\n", "a new game has no DMs")

    rc, out, err = dmadmin(g, "create", "Boss", "master", stdin="boss password\n")
    check(rc == 0 and "BOSS: made, rank master" in out, "a master DM is made: " + out.strip())
    p = rows(g, "SELECT lvl, class, ssj, master, assoc, room FROM players WHERE name = 'BOSS'")
    start = rows(g, "SELECT start_room FROM settings")[0][0]
    check(p == [(2, "dm", 1, 1, 0, start)],
          "a 2nd-level DM character in the start room, with SSJ and Master")
    rc, out, _ = dmadmin(g, "create", "helper", "associate", stdin="  helper pass  \n")
    check(rc == 0, "an associate is made")
    rc, out, _ = dmadmin(g, "create", "Deputy", "dm", stdin="deputy password\n")
    rc, out, _ = dmadmin(g, "list")
    check(out == "BOSS       master\nHELPER     associate 1\nDEPUTY     dm\n",
          "list shows the DMs, most senior first")
    h = history(g)
    check([r[:3] for r in h] == [("BOSS", None, "master"), ("HELPER", None, "associate"),
                                 ("DEPUTY", None, "dm")]
          and all(r[4] == "dmadmin" and r[3] for r in h),
          "each new DM is in the history, by the account that ran dmadmin")

    refuses(g, ["create", "boss", "dm"], "another password\n", ["already"],
            "a name that is taken is refused")
    refuses(g, ["create", "Bob2", "dm"], "another password\n", ["letters"],
            "a name with a digit is refused")
    refuses(g, ["create", "Elevenchars", "dm"], "another password\n", ["letters"],
            "an 11-letter name is refused")
    refuses(g, ["create", "the", "dm"], "another password\n", ["name"],
            "a word the parser skips is refused")
    refuses(g, ["create", "Newbie", "none"], "another password\n", ["no such rank"],
            "create takes no rank none")
    refuses(g, ["create", "Newbie", "boss"], "another password\n", ["no such rank"],
            "an unknown rank is refused")
    refuses(g, ["create", "Newbie", "dm"], "short\n", ["8 to 72"],
            "a short password is refused")
    refuses(g, ["create", "Newbie", "dm"], "x" * 73 + "\n", ["72"],
            "a long password is refused")
    refuses(g, ["create", "Newbie", "dm"], "", ["no password"],
            "no password on standard input is refused")

    # the password is the line without its outer spaces, as at the login prompt
    out = play(g, ["helper", "helper pass", "quit", "y"])
    check("Wrong password" not in out and "Please welcome helper" in out,
          "the associate logs in with the password, outer spaces removed")


def test_rank():
    g = game("rank")
    dmadmin(g, "create", "Boss", "master", stdin="boss password\n")
    refuses(g, ["rank", "boss", "dm"], "", ["last master"],
            "the last master can't be demoted")
    refuses(g, ["remove", "boss"], "", ["last master"],
            "the last master can't be removed")
    refuses(g, ["rank", "nobody", "dm"], "", ["no such character"],
            "a missing character is refused")

    rc, _, _ = run("dmtest", g, "player", "Pat", "pat password")
    rc, out, _ = dmadmin(g, "rank", "pat", "master")
    check(rc == 0 and "PAT: none -> master" in out, "a player is made a master")
    rc, out, _ = dmadmin(g, "rank", "boss", "associate",
                         env={"SUDO_USER": "operator"})
    check(rc == 0 and "BOSS: master -> associate" in out,
          "with another master, the first can be demoted")
    rc, out, _ = dmadmin(g, "rank", "boss", "associate")
    check(rc == 0 and "already associate" in out, "the same rank changes nothing")
    rc, out, _ = dmadmin(g, "rank", "boss", "none")
    p = rows(g, "SELECT ssj, master, assoc FROM players WHERE name = 'BOSS'")
    check(rc == 0 and p == [(0, 0, 0)], "rank none takes every DM flag away")
    check(history(g)[1:] == [("PAT", "none", "master", history(g)[1][3], "dmadmin"),
                             ("BOSS", "master", "associate", "operator", "dmadmin"),
                             ("BOSS", "associate", "none", history(g)[3][3], "dmadmin")],
          "the changes are recorded, with $SUDO_USER under sudo")
    refuses(g, ["remove", "boss"], "", ["not a DM"], "remove takes only DMs")
    refuses(g, ["remove", "pat"], "", ["last master"], "nor the new last master")

    dmadmin(g, "rank", "boss", "dm")
    rc, out, _ = dmadmin(g, "remove", "boss")
    check(rc == 0 and rows(g, "SELECT name FROM players WHERE name = 'BOSS'") == []
          and history(g)[-1][:3] == ("BOSS", "dm", None), "a DM is removed, and recorded")
    rc, out, _ = run("dmadmin", g, "history")
    check(rc == 0 and len(out.splitlines()) == len(history(g))
          and "BOSS       dm -> removed  by" in out and "new -> master" in out
          and "(operator)" not in out and "by operator (dmadmin)" in out,
          "history prints each change")


def test_keeps_character():
    """rank and password change only what they say: the dump shows the rest."""
    g = game("inuse", "146")  # Alice is a master with objects, Bob an associate
    before = dump(g).splitlines()
    rc, _, _ = dmadmin(g, "password", "alice", stdin="alice new password\n")
    rc2, _, _ = dmadmin(g, "rank", "bob", "dm")
    after = dump(g).splitlines()
    changed = [(a, b) for a, b in zip(before, after) if a != b]
    check(rc == 0 and rc2 == 0 and len(changed) == 1
          and "Name=\"BOB\"" in changed[0][0] and "Assoc=0" in changed[0][1]
          and len(after) == len(before) + 1 and after[-1].startswith("rank ")
          and "Player=\"BOB\" Old=RAssoc New=RDM" in after[-1],
          "objects, worn things and other fields are kept")
    out = play(g, ["alice", "alice new password", "quit", "y"])
    check("Wrong password" not in out and "Please welcome alice" in out,
          "the new password opens the character")
    refuses(g, ["password", "alice"], "short\n", ["8 to 72"],
            "a short new password is refused")


def test_console_dm():
    """#70: a DM made by dmadmin plays the DM commands in the console harness."""
    g = game("console")
    dmadmin(g, "create", "Boss", "master", stdin="boss password\n")
    out = play(g, ["boss", "boss password", "*edit", "display player boss",
                   "modify player boss zz=d", "modify file boss zz=n",
                   "display player boss", "quit", "quit", "y"])
    check("Please welcome boss" in out and "Wrong password" not in out,
          "the DM logs in through scepterd -c")
    check("boss, the second level Caretaker (Master DM)" in out,
          "the editor shows the master DM")
    check(out.count("boss is the last master DM, so stays one.") == 2,
          "the editor won't demote the last master, playing or saved")
    check(rows(g, "SELECT ssj, master FROM players WHERE name = 'BOSS'") == [(1, 1)]
          and len(history(g)) == 1, "the master is still saved as one, and nothing recorded")

    dmadmin(g, "create", "Deputy", "dm", stdin="deputy password\n")
    out = play(g, ["boss", "boss password", "*edit", "delete file boss",
                   "modify file deputy zz=m", "modify player boss zz=d",
                   "quit", "quit", "y"])
    check("That is the last master DM, who can't be erased." in out,
          "the editor won't erase the last master")
    h = history(g)
    check(h[1:] == [("DEPUTY", None, "dm", h[1][3], "dmadmin"),
                    ("DEPUTY", "dm", "master", "BOSS", "game"),
                    ("BOSS", "master", "dm", "BOSS", "game")],
          "with a second master, the editor changes ranks and records who did")
    rc, out, _ = dmadmin(g, "list")
    check(out == "BOSS       dm\nDEPUTY     master\n", "and list shows the result")

    out = play(g, ["deputy", "deputy password", "*edit",
                   "modify player deputy as=3", "modify file boss zz=a",
                   "display file boss", "quit", "quit", "y"])
    check("deputy is the last master DM, so stays one." in out,
          "AS= on the last master is a demotion too")
    check("(Assoc DM 1)" in out, "ZZ=A makes an associate")
    out = play(g, ["deputy", "deputy password", "*edit", "delete file boss",
                   "quit", "quit", "y"])
    check(history(g)[-1][:3] == ("BOSS", "associate", None) and "Player erased." in out,
          "erasing a DM is recorded")


def main():
    try:
        test_usage()
        test_create()
        test_rank()
        test_keeps_character()
        test_console_dm()
    finally:
        shutil.rmtree(TMP)
    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
