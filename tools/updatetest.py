#!/usr/bin/env python3
"""Tests of mkgame -u, which updates a game to a new revision of its world
(#137), using the world and the DM edits the game records (#138).

    python3 -I tools/updatetest.py [BINDIR [WORLD]]

BINDIR holds scepterd, mkgame, upgradegame, dmadmin, dmtest and dumpgame
(default: .).  Each case makes a game from WORLD (default
var/alpha/alpha.world) in a temporary directory, then updates it from WORLD
or from an edited copy.  A refusal must change nothing, as dumpgame sees
it.  `make check-update` runs this.
"""

import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile


BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
WORLD = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "var/alpha/alpha.world")
TMP = tempfile.mkdtemp()
failures = 0
with open(WORLD) as f:
    TEXT = f.read()
WORLD_ID = re.search(r"^id = (\S+)$", TEXT, re.M).group(1)

# A new revision: room 2 leads up to a new room 60, which holds a lantern;
# room 4 is renamed (#81); object template 3 is heavier; encounter table 2
# changes.
REVISION_2 = (TEXT.replace("[room 2]\nname = North road\nnorth = 6\nsouth = 1\n",
                           "[room 2]\nname = North road\nnorth = 6\nsouth = 1\nup = 60\n", 1)
              .replace("[room 4]\nname = Market\n", "[room 4]\nname = Market square\n", 1)
              + "\n[room 60]\ndown = 2\ndesc = 1:3\n\n[room 60 / object 1]\n"
                "name = lantern,brass *\nkind = misc\npermanent = yes\nweight = 2\n")
assert REVISION_2.count("up = 60") == 1 and "Market square" in REVISION_2


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1
    return cond


def run(prog, *args, stdin=""):
    p = subprocess.run([os.path.join(BIN, prog)] + list(args), input=stdin,
                       capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout, p.stderr


def world(name, text):
    path = os.path.join(TMP, name + ".world")
    with open(path, "w") as f:
        f.write(text)
    return path


def game(name, world_file=WORLD):
    path = os.path.join(TMP, name)
    os.mkdir(path)  # mkgame never creates it
    rc, _, err = run("mkgame", world_file, path)
    assert rc == 0, err
    return path


def dump(g):
    return run("dumpgame", g)[1]


def files(g):
    return sorted(os.listdir(g))


def sql(g, query, *args):
    db = sqlite3.connect(os.path.join(g, "game.db"))
    try:
        rows = db.execute(query, args).fetchall()
        db.commit()
        return rows
    finally:
        db.close()


def update(g, world_file, *flags):
    return run("mkgame", "-u", *flags, world_file, g)


def refused(g, world_file, flags, words, what):
    """mkgame -u fails, saying words, and changes nothing."""
    before, names = dump(g), files(g)
    rc, out, err = update(g, world_file, *flags)
    check(rc == 1 and all(w in out + err for w in words)
          and dump(g) == before and files(g) == names, what)
    return out + err


def play(g, lines):
    _, out, _ = run("scepterd", "-c", g, stdin="".join(l + "\n" for l in lines))
    return out


def test_usage():
    g = game("usage")
    for args in (["-n", WORLD, g], ["--adopt", WORLD, g], ["--discard-edits", WORLD, g],
                 ["-u", WORLD], ["-u", "-x", WORLD, g]):
        rc, _, err = run("mkgame", *args)
        check(rc == 2 and "usage" in err, "usage: mkgame %s" % " ".join(
            a if a not in (WORLD, g) else os.path.basename(a) for a in args))


def test_same_revision():
    g = game("same")
    names = files(g)
    rc, out, _ = update(g, WORLD)
    check(rc == 0 and "already at revision" in out and files(g) == names,
          "the same revision is left alone, without a backup")


def test_update():
    g = game("update")
    run("dmtest", g, "player", "Wanda", "wanda password")
    sql(g, "UPDATE settings SET notice = 'A DM notice.', monster_speed = 7, runs = 5")
    sql(g, "UPDATE news SET text = 'DM news.' WHERE line = 1")
    run("dmadmin", g, "create", "Boss", "master", stdin="boss password\n")
    new = world("rev2", REVISION_2)

    before, names = dump(g), files(g)
    rc, out, _ = update(g, new, "-n")
    check(rc == 0 and dump(g) == before and files(g) == names,
          "-n writes nothing")
    check("would be updated" in out and "rooms added: 60 (1)" in out
          and "rooms changed: 2 4 (2)" in out and "room contents: 60 (1)" in out,
          "-n reports the room added, the rooms changed (one renamed) and the contents")

    rc, out, _ = update(g, new)
    backups = [f for f in files(g) if f.startswith("game.db.backup-")]
    check(rc == 0 and len(backups) == 1 and "backed up to" in out
          and "updated from" in out, "the game is updated, beside a backup")
    check(sql(g, "SELECT up FROM rooms WHERE room = 2") == [(60,)]
          and sql(g, "SELECT name FROM room_objects WHERE room = 60")
          == [("lantern,brass *",)], "the new rooms and room contents are in")
    check(sql(g, "SELECT name FROM players ORDER BY id") == [("WANDA",), ("BOSS",)]
          and sql(g, "SELECT player FROM rank_changes") == [("BOSS",)],
          "characters, DMs and the rank history are kept")
    check(sql(g, "SELECT notice, monster_speed, runs FROM settings")
          == [("A DM notice.", 7, 5)]
          and sql(g, "SELECT text FROM news WHERE line = 1") == [("DM news.",)],
          "the notice, news, monster speed and run counter are kept")
    hist = sql(g, "SELECT event FROM world_history ORDER BY id")
    check(hist == [("made",), ("updated",)]
          and sql(g, "SELECT world_id FROM world") == [(WORLD_ID,)],
          "the update is recorded in the world's history")
    out = play(g, ["boss", "boss password", "*world", "quit", "y"])
    check("Please welcome boss" in out and "  updated  " in out
          and "Changed by DMs since then: nothing." in out,
          "scepterd plays the updated game, and *WORLD shows the update")

    # and back: room 60 goes, and a character in it is moved to the start room
    sql(g, "UPDATE players SET room = 60 WHERE name = 'WANDA'")
    rc, out, _ = update(g, WORLD)
    check(rc == 0 and "rooms removed: 60 (1)" in out
          and "characters moved to the start room 1: WANDA (from 60)" in out
          and sql(g, "SELECT room FROM players WHERE name = 'WANDA'") == [(1,)]
          and sql(g, "SELECT count(*) FROM room_objects WHERE room = 60") == [(0,)],
          "a removed room's character goes to the start room, its contents go")
    check(len([f for f in files(g) if f.startswith("game.db.backup-")]) == 2,
          "each update has its own backup, even within one second")


def test_other_world():
    g = game("other")
    other = world("other", TEXT.replace("id = " + WORLD_ID,
                                        "id = 00000000-0000-4000-8000-000000000000"))
    refused(g, other, [], ["only be updated from its own world"],
            "a world with another id is refused")
    refused(g, other, ["--adopt"], ["only be updated from its own world"],
            "even with --adopt")
    bad = world("bad", REVISION_2.replace("down = 2\n", "down = 5000\n"))
    refused(g, bad, [], ["error", "not updated"], "a world with errors is refused")


def test_edits():
    g = game("edits")
    run("dmadmin", g, "create", "Boss", "master", stdin="boss password\n")
    # a DM's room contents aren't kept: the update replaces them
    play(g, ["boss", "boss password", "*edit", 'create object x 3 na="rock,grey *"',
             "modify room 3 sa=t", "modify olist 3 we=9", "quit", "quit", "y"])
    new = world("edits2", REVISION_2)
    out = refused(g, new, [], ["room 3", "object template 3", "--discard-edits"],
                  "DM edits refuse the update, and are listed")
    rc, out, err = update(g, new, "-n")
    check(rc == 1 and "room 3" in out and "rooms added: 60" in out,
          "-n lists the edits and the changes, and says it would be refused")

    rc, out, _ = update(g, new, "--discard-edits")
    check(rc == 0 and sql(g, "SELECT safe FROM rooms WHERE room = 3") == [(0,)]
          and sql(g, "SELECT weight FROM object_templates WHERE number = 3")
          != [(9,)], "--discard-edits overwrites them with the world's")
    check(sql(g, "SELECT name FROM room_objects WHERE room = 3 AND name = 'rock,grey *'")
          == [], "room contents are the world's")
    out = play(g, ["boss", "boss password", "*world", "quit", "y"])
    check("Changed by DMs since then: nothing." in out, "and the edits are cleared")


def test_adopt():
    g = game("adopt")
    run("dmtest", g, "player", "Wanda", "wanda password")
    sql(g, "DELETE FROM world")
    sql(g, "DELETE FROM world_history")
    for t in ("rooms", "descriptions", "monster_templates", "object_templates"):
        sql(g, "UPDATE %s SET created_at = 0, updated_at = 0" % t)  # as migration 4 leaves them
    sql(g, "DELETE FROM encounter_times")
    sql(g, "DELETE FROM treasure_times")
    refused(g, WORLD, [], ["doesn't record its world", "--adopt"],
            "a game with no recorded world needs --adopt")
    rc, out, _ = update(g, WORLD, "--adopt")
    check(rc == 0 and sql(g, "SELECT world_id FROM world") == [(WORLD_ID,)]
          and sql(g, "SELECT event FROM world_history") == [("adopted",)]
          and sql(g, "SELECT name FROM players") == [("WANDA",)],
          "--adopt records the world and updates it, keeping characters")
    check(sql(g, "SELECT DISTINCT created_at FROM rooms") == [(0,)],
          "rows from before the world was known keep created_at unknown")
    rc, out, _ = update(g, WORLD)
    check(rc == 0 and "already at revision" in out, "and then it is up to date")


def test_adopt_migrated():
    """The live game's path: a schema 3 game, upgraded, then adopted."""
    g = game("migrated")
    run("dmtest", g, "player", "Wanda", "wanda password")
    db = sqlite3.connect(os.path.join(g, "game.db"))
    db.executescript(
        "ALTER TABLE rooms DROP COLUMN name;"
        " DROP TABLE world; DROP TABLE world_history; DROP TABLE encounter_times;"
        " DROP TABLE treasure_times;"
        + "".join("ALTER TABLE %s DROP COLUMN created_at; ALTER TABLE %s DROP COLUMN"
                  " updated_at; " % (t, t) for t in ("rooms", "descriptions",
                                                     "monster_templates", "object_templates"))
        + "PRAGMA user_version = 3;")
    db.close()
    rc, _, err = run("upgradegame", g)
    assert rc == 0, err
    rc, out, err = update(g, world("migrated2", REVISION_2), "--adopt")
    check(rc == 0 and "rooms added: 60 (1)" in out
          and sql(g, "SELECT event FROM world_history") == [("adopted",)]
          and sql(g, "SELECT name, room FROM players") == [("WANDA", 1)],
          "an upgraded schema 3 game is adopted and updated, keeping its character")


def test_failure():
    """A failure part way leaves the game as it was, beside its backup."""
    g = game("fail")
    sql(g, "CREATE TRIGGER fail BEFORE INSERT ON world_history BEGIN"
           " SELECT raise(ABORT, 'planted failure'); END")
    before = dump(g)
    rc, out, err = update(g, world("fail2", REVISION_2))
    check(rc == 1 and "planted failure" in err and "as it was, beside its backup" in err
          and dump(g) == before
          and any(f.startswith("game.db.backup-") for f in files(g)),
          "a failure after every write leaves the game as it was")


def main():
    try:
        test_usage()
        test_same_revision()
        test_update()
        test_other_world()
        test_edits()
        test_adopt()
        test_adopt_migrated()
        test_failure()
    finally:
        shutil.rmtree(TMP)
    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
