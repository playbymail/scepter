#!/usr/bin/env python3
"""Tests of mkgame's game directory, the game database (gamedb.c) and
tools/upgradegame, its schema migrator.

    python3 -I tools/datatest.py [BINDIR [WORLD]]

BINDIR holds scepterd, mkgame, upgradegame, fightsim, dmtest, dumpgame,
dbtest and dbtest2 (default: .).  Each case builds a game from WORLD (default
var/alpha/alpha.world) in a temporary directory, damages or downgrades it,
and checks what scepterd, the database functions and upgradegame make of it.  Refusals must leave
everything as it was: nothing created, nothing changed.
`make check-data` runs this.
"""

import hashlib
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time


BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "gamedb.h")) as f:
    VERSION = int(re.search(r"#define\tDBVersion\t(\d+)", f.read()).group(1))
WORLD = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "var/alpha/alpha.world")
TMP = tempfile.mkdtemp()
failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1
    return cond


def run(prog, *args, stdin=""):
    p = subprocess.run([os.path.join(BIN, prog)] + list(args), input=stdin,
                       capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout, p.stderr


def game(name):
    path = os.path.join(TMP, name)
    os.mkdir(path)  # mkgame never creates it
    rc, _, err = run("mkgame", WORLD, path)
    assert rc == 0, err
    return path


def server_ok(path):
    """scepterd -c starts on path and plays a login prompt."""
    rc, out, err = run("scepterd", "-c", path, stdin="")
    return rc == 0 and "Enter your name?" in out, err


def server_refuses(path, *words):
    rc, out, err = run("scepterd", "-c", path, stdin="")
    return rc == 1 and "Enter your name?" not in out and all(w in err for w in words), err


def test_missing_settings():
    # #53: an error before the main loop is logged with the real time
    g = game("nosettings")
    db = sqlite3.connect(os.path.join(g, "game.db"))
    db.execute("DELETE FROM settings")
    db.commit()
    db.close()
    ok, err = server_refuses(g, "Cannot read the game's settings")
    check(ok, "scepterd reports missing settings on stderr")
    with open(os.path.join(g, "errlog")) as f:
        log = f.read()
    check(time.strftime("%Y") in log.split("\n")[0]
          and "Cannot read the game's settings" in log,
          "errlog records it with today's date")


def tree(path):
    """Every file under path with its bytes and mode, or None if path is absent."""
    if not os.path.lexists(path):
        return None
    if not os.path.isdir(path):
        with open(path, "rb") as f:
            return f.read()
    out = {}
    for root, dirs, files in os.walk(path):
        for d in dirs:
            out[os.path.relpath(os.path.join(root, d), path)] = "dir"
        for f in files:
            p = os.path.join(root, f)
            with open(p, "rb") as fp:
                out[os.path.relpath(p, path)] = (fp.read(), os.stat(p).st_mode)
    return out


ROOT = os.geteuid() == 0


def test_gamedir():
    """mkgame needs an existing, empty directory, and never creates one."""
    missing = os.path.join(TMP, "nodir", "game")
    rc, _, err = run("mkgame", WORLD, missing)
    check(rc == 1 and "No such file or directory" in err
          and not os.path.lexists(os.path.dirname(missing)),
          "mkgame refuses a missing GAMEDIR and creates nothing")

    afile = os.path.join(TMP, "afile")
    with open(afile, "w") as f:
        f.write("not a directory\n")
    rc, _, err = run("mkgame", WORLD, afile)
    check(rc == 1 and "not a directory" in err and tree(afile) == b"not a directory\n",
          "mkgame refuses a GAMEDIR that is a file and leaves it alone")

    full = os.path.join(TMP, "full")
    os.mkdir(full)
    with open(os.path.join(full, ".keep"), "w") as f:
        f.write("x")
    before = tree(full)
    rc, _, err = run("mkgame", WORLD, full)
    check(rc == 1 and "not empty" in err and ".keep" in err and tree(full) == before,
          "mkgame refuses a GAMEDIR holding a hidden file, and names it")

    g = game("again")
    before = tree(g)
    rc, _, err = run("mkgame", WORLD, g)
    check(rc == 1 and "not empty" in err and tree(g) == before,
          "mkgame refuses a GAMEDIR that holds a game and changes nothing")

    rc, _, err = run("mkgame", "-f", WORLD, os.path.join(TMP, "gone"))
    check(rc == 2 and "usage" in err, "mkgame no longer takes -f")

    if ROOT:
        print("skip an unwritable GAMEDIR: root can write to it")
        return
    ro = os.path.join(TMP, "ro")
    os.mkdir(ro, 0o555)
    rc, _, err = run("mkgame", WORLD, ro)
    os.chmod(ro, 0o755)
    check(rc == 1 and "not writable" in err and tree(ro) == {},
          "mkgame refuses an unwritable GAMEDIR and writes nothing")


def set_version(path, version):
    db = sqlite3.connect(os.path.join(path, "game.db"))
    db.execute("PRAGMA user_version = %d" % version)
    db.close()


def test_database():
    """scepterd refuses a game whose database it can't use, changing nothing."""
    def refuses(what, damage, *words):
        g = game("db-" + what.replace(" ", "-"))
        damage(g)
        before = tree(g)
        ok, err = server_refuses(g, "game.db", *words)
        check(ok and tree(g) == before,
              "scepterd refuses %s and creates or changes nothing" % what)
        if not ok:
            print(err)

    refuses("a missing database", lambda g: os.remove(os.path.join(g, "game.db")),
            "No such file or directory")
    refuses("an older schema version", lambda g: set_version(g, 0),
            "schema version 0", "upgradegame")
    refuses("a newer schema version", lambda g: set_version(g, VERSION + 1),
            "schema version %d" % (VERSION + 1), "newer scepterd")
    if ROOT:
        print("skip a read-only database: root can write to it")
    else:
        refuses("a read-only database",
                lambda g: os.chmod(os.path.join(g, "game.db"), 0o444), "read-only")

    def garble(g):
        with open(os.path.join(g, "game.db"), "r+b") as f:
            f.write(b"this is not a database at all, not even close" * 4)
    refuses("a corrupt database", garble, "not a database")

    def gap(g):
        db = sqlite3.connect(os.path.join(g, "game.db"))
        db.execute("DELETE FROM rooms WHERE room = 1500")
        db.commit()
        db.close()
    refuses("a game with a room missing", gap, "whole segments")

    def not_wal(g):
        db = sqlite3.connect(os.path.join(g, "game.db"))
        db.execute("PRAGMA journal_mode = DELETE")
        db.close()
    refuses("a database not in WAL mode", not_wal, "not in WAL mode")

    rc, _, err = run("scepterd", "-c", os.path.join(TMP, "nogame"), stdin="")
    check(rc == 1 and not os.path.lexists(os.path.join(TMP, "nogame")),
          "scepterd refuses a missing game directory and creates nothing")

    # a game from before the database: dat/ and ram/ full of struct files
    old = os.path.join(TMP, "oldgame")
    for d in ("", "dat", "ram"):
        os.mkdir(os.path.join(old, d))
    with open(os.path.join(old, "ram", "eperson.new"), "wb") as f:
        f.write(b"SCEPPERS" + bytes(24))
    before = tree(old)
    rc, out, err = run("scepterd", "-c", old, stdin="")
    check(rc == 1 and "make a new game with mkgame" in err and tree(old) == before,
          "scepterd tells an old struct-file game to make a new one, and leaves it alone")

    g = game("db-stock")
    db = sqlite3.connect(os.path.join(g, "game.db"))
    db.execute("UPDATE room_stock SET object = 999 WHERE (room, slot) ="
               " (SELECT room, slot FROM room_stock LIMIT 1)")
    db.commit()
    db.close()
    ok, err = server_refuses(g, "Bad stock in the special rooms")
    check(ok, "scepterd refuses a shop selling a template that can't exist")

    g = game("db-fight")
    set_version(g, 0)
    rc, _, err = run("fightsim", "-r", "1", g, "4", "9")
    check(rc == 1 and "schema version 0" in err, "fightsim refuses it too")


def column_list(name):
    """ObjCols or MonCols from gamedb.h, as a list of column names."""
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "gamedb.h")) as f:
        h = f.read()
    m = re.search(r"#define\t%s\t(.*?)\n(?=\n|#define)" % name, h, re.S)
    text = "".join(re.findall(r'"([^"]*)"', m.group(1)))
    return [c.strip() for c in text.split(",")]


def test_columns():
    """dbstore.c's column lists match every table that holds objects or monsters."""
    g = game("columns")
    db = sqlite3.connect(os.path.join(g, "game.db"))
    obj, mon = column_list("ObjCols"), column_list("MonCols")
    times = ["created_at", "updated_at"]  # world data (migration 4, #138)
    for table, keys, cols in [
            ("object_templates", ["number"], obj + times),
            ("room_objects", ["room", "slot"], obj),
            ("chest_objects", ["room", "chest", "slot"], obj),
            ("monster_objects", ["room", "monster", "slot"], obj),
            ("player_objects", ["player", "slot", "worn"], obj),
            ("monster_templates", ["number"], mon + times),
            ("room_monsters", ["room", "slot"], mon)]:
        got = [r[1] for r in db.execute("PRAGMA table_info(%s)" % table)]
        check(got == keys + cols, "%s has the columns dbstore.c reads and writes" % table)
    db.close()


def dbtest(*args):
    return run("dbtest", *args)


def test_dbfuncs():
    """Every database function refuses a bad path and leaves nothing behind."""
    afile = os.path.join(TMP, "dbfile")
    with open(afile, "w") as f:
        f.write("x")
    missing = os.path.join(TMP, "dbmissing")
    ro = os.path.join(TMP, "dbro")
    os.mkdir(ro, 0o555)
    g = game("dbfuncs")
    backup = os.path.join(TMP, "dbfuncs.bak")

    def bad(what, args, words, paths):
        before = [tree(p) for p in paths]
        rc, _, err = dbtest(*args)
        check(rc == 1 and words in err and [tree(p) for p in paths] == before,
              "%s: %s" % (args[0], what))

    for name, d, words in [("a missing directory", missing, "No such file"),
                           ("a path that is a file", afile, "not a directory"),
                           ("an unwritable directory", ro, "not writable")]:
        if ROOT and d == ro:
            print("skip %s: root can write to it" % name)
            continue
        bad("refuses " + name, ["create", d], words, [d])
        bad("refuses " + name, ["open", d], words, [d])
        bad("refuses " + name, ["migrate", d], words, [d])
        bad("refuses " + name, ["restore", os.path.join(g, "game.db"), d], words, [d, g])
        bad("refuses a backup into " + name,
            ["backup", g, os.path.join(d, "game.bak")], words, [d, g])

    rc, _, err = dbtest("create", g)
    check(rc == 1 and "already exists" in err, "create: refuses an existing game.db")

    rc, _, _ = dbtest("backup", g, backup)
    check(rc == 0 and os.path.isfile(backup) and not os.path.lexists(backup + "-wal"),
          "backup: copies game.db to a new file, leaving no -wal behind")
    rc, _, err = dbtest("backup", g, backup)
    check(rc == 1 and "already exists" in err, "backup: never overwrites a file")

    db = sqlite3.connect(os.path.join(g, "game.db"))
    db.execute("UPDATE news SET text = 'after the backup' WHERE line = 1")
    db.commit()
    db.close()
    rc, _, err = dbtest("restore", backup, g)
    db = sqlite3.connect(os.path.join(g, "game.db"))
    n = db.execute("SELECT count(*) FROM news WHERE text = 'after the backup'").fetchone()[0]
    db.close()
    check(rc == 0 and n == 0 and not os.path.lexists(os.path.join(g, "game.db.restore")),
          "restore: puts the backup back, with nothing left over")
    rc, _, _ = dbtest("open", g)
    check(rc == 0, "open: the restored game opens")
    with open(afile, "w") as f:
        f.write("this is not a database at all, not even close\n" * 10)
    rc, _, err = dbtest("restore", afile, g)
    check(rc == 1 and "not a database" in err, "restore: refuses a file that isn't a database")

    # an empty version 0 database: migration 1 makes the whole schema
    old = os.path.join(TMP, "dbold")
    os.mkdir(old)
    db = sqlite3.connect(os.path.join(old, "game.db"))
    db.execute("PRAGMA journal_mode = WAL")
    db.close()
    before = tree(old)
    rc, out, _ = dbtest("migrate", "-n", old)
    check(rc == 0 and "would migrate to %d" % VERSION in out and tree(old) == before,
          "migrate -n: says what it would do and changes nothing")
    rc, out, _ = dbtest("migrate", old)
    backups = [f for f in os.listdir(old) if f.startswith("game.db.backup-")]
    rc2, ver, _ = dbtest("version", old)
    check(rc == 0 and len(backups) == 1 and ver.strip() == str(VERSION),
          "migrate: backs up to a file in the game directory, then reaches version %d"
          % VERSION)
    rc, _, _ = dbtest("open", old)
    check(rc == 0, "open: the migrated game opens")
    rc, out, _ = dbtest("migrate", old)
    check(rc == 0 and "already at schema version %d" % VERSION in out,
          "migrate: a current game is left alone")
    os.chmod(ro, 0o755)


def test_upgradegame():
    """upgradegame migrates the database, and changes nothing it can't."""
    g = game("current")
    before = tree(g)
    rc, out, _ = run("upgradegame", g)
    check(rc == 0 and "already at schema version %d" % VERSION in out and tree(g) == before,
          "upgradegame leaves a current game alone")

    g = game("newer")
    set_version(g, VERSION + 1)
    before = tree(g)
    rc, _, err = run("upgradegame", g)
    check(rc == 1 and "newer than this build" in err and tree(g) == before,
          "upgradegame refuses a newer database and changes nothing")

    # a game whose version says 0, but that already has the tables:
    # migration 1 fails, and the database stays as it was
    g = game("failing")
    set_version(g, 0)
    before = tree(g)
    rc, out, err = run("upgradegame", "-n", g)
    check(rc == 0 and "would migrate to %d" % VERSION in out and tree(g) == before,
          "upgradegame -n says what it would do and changes nothing")
    rc, out, err = run("upgradegame", g)
    after = tree(g)
    backups = [f for f in after if f.startswith("game.db.backup-")]
    for f in backups:
        del after[f]
    check(rc == 1 and "migration 1 failed" in err and "not upgraded" in err
          and len(backups) == 1 and after == before,
          "a failed migration leaves the database as it was, beside its backup")
    ok, _ = server_refuses(g, "schema version 0")
    check(ok, "and scepterd still refuses it")

    # an empty version 0 database: migration 1 makes the whole schema
    g = os.path.join(TMP, "empty")
    os.mkdir(g)
    db = sqlite3.connect(os.path.join(g, "game.db"))
    db.execute("PRAGMA journal_mode = WAL")
    db.close()
    rc, out, _ = run("upgradegame", g)
    rc2, ver, _ = dbtest("version", g)
    check(rc == 0 and "migrated from schema version 0 to %d" % VERSION in out
          and ver.strip() == str(VERSION), "upgradegame migrates a version 0 database")


# The tables each migration after the first adds, and the columns it adds
# to older tables, to make an older game
OLD_TABLES = {2: ["login_hosts", "login_names"], 3: ["rank_changes"],
              4: ["world", "world_history", "encounter_times", "treasure_times"], 5: []}
OLD_COLUMNS = {4: [(t, c) for t in ("rooms", "descriptions", "monster_templates",
                                    "object_templates")
                   for c in ("created_at", "updated_at")],
               5: [("rooms", "name")]}


def test_upgrade_old(version):
    """A game at an older version, as deployed before a migration, upgrades."""
    g = game("v%d" % version)
    run("dmtest", g, "player", "Oldtimer", "ac secret")
    added = [t for v in range(version + 1, VERSION + 1) for t in OLD_TABLES[v]]
    columns = [tc for v in range(version + 1, VERSION + 1) for tc in OLD_COLUMNS.get(v, [])]
    db = sqlite3.connect(os.path.join(g, "game.db"))
    db.executescript("".join("DROP TABLE %s; " % t for t in added)
                     + "".join("ALTER TABLE %s DROP COLUMN %s; " % tc for tc in columns)
                     + "PRAGMA user_version = %d;" % version)
    db.close()
    ok, _ = server_refuses(g, "schema version %d" % version, "upgradegame")
    check(ok, "scepterd refuses a version %d game" % version)
    rc, out, err = run("upgradegame", g)
    db = sqlite3.connect(os.path.join(g, "game.db"))
    tables = {r[0] for r in db.execute("SELECT name FROM sqlite_schema WHERE type = 'table'")}
    who = db.execute("SELECT name FROM players").fetchall()
    db.close()
    check(rc == 0 and "migrated from schema version %d to %d" % (version, VERSION) in out
          and set(added) <= tables and who == [("OLDTIMER",)],
          "upgradegame adds %s, keeping the character" % ", ".join(added))
    rc, out, _ = run("scepterd", "-c", g, stdin="Oldtimer\nac secret\nquit\ny\n")
    check("Oldtimer" in out and "Wrong password" not in out,
          "and Oldtimer logs in to the upgraded game")


def test_migration():
    """A real migration, on a whole game: dbtest2 has a test-only migration 2."""
    g = game("migrate")
    rc, _, _ = run("dmtest", g, "player", "Oldtimer", "ac secret")
    files = sorted(os.listdir(g))
    _, state, _ = run("dumpgame", g)
    rc, out, err = run("dbtest2", "migrate", g)
    backups = [f for f in os.listdir(g) if f.startswith("game.db.backup-")]
    db = sqlite3.connect(os.path.join(g, "game.db"))
    got = db.execute("SELECT x FROM test_migration").fetchall()
    who = db.execute("SELECT name FROM players").fetchall()
    db.close()
    check(rc == 0 and "migrated from schema version %d to %d" % (VERSION, VERSION + 1)
          in out
          and len(backups) == 1 and got == [(VERSION + 1,)] and who == [("OLDTIMER",)],
          "a game is migrated, keeping its character, beside a backup")
    ok, _ = server_refuses(g, "schema version %d" % (VERSION + 1), "newer scepterd")
    check(ok, "this build's scepterd refuses the migrated game")
    rc, _, _ = dbtest("restore", os.path.join(g, backups[0]), g)
    os.remove(os.path.join(g, backups[0]))
    _, again, _ = run("dumpgame", g)
    check(rc == 0 and sorted(os.listdir(g)) == files and again == state,
          "restoring the backup gives the game back, as dumpgame sees it")
    ok, _ = server_ok(g)
    check(ok, "and scepterd plays it again")


def world_text(change):
    """A world file made from WORLD with change applied to its text."""
    with open(WORLD) as f:
        text = change(f.read())
    path = os.path.join(TMP, "w%d.world" % len(os.listdir(TMP)))
    with open(path, "w") as f:
        f.write(text)
    return path


def world_id():
    with open(WORLD) as f:
        return re.search(r"^id = (\S+)$", f.read(), re.M).group(1)


def test_world_made():
    """#138: mkgame records the world, its revision and when, in one go."""
    g = game("world")
    with open(WORLD, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    db = sqlite3.connect(os.path.join(g, "game.db"))
    world = db.execute("SELECT world_id, name, revision FROM world").fetchall()
    hist = db.execute("SELECT time, event, revision, schema_version FROM world_history").fetchall()
    stamps = set()
    for t in ("rooms", "descriptions", "monster_templates", "object_templates",
              "encounter_times", "treasure_times"):
        stamps |= set(db.execute("SELECT created_at, updated_at FROM %s" % t).fetchall())
    tables = [db.execute("SELECT count(*) FROM %s" % t).fetchone()[0]
              for t in ("encounter_times", "treasure_times")]
    db.close()
    check(world == [(world_id(), "alpha", sha)],
          "the world's id, name and revision (shasum -a 256 of the file) are recorded")
    check(len(hist) == 1 and hist[0][1:] == ("made", sha, VERSION)
          and abs(hist[0][0] - time.time()) < 60, "and a 'made' history row, now")
    check(stamps == {(hist[0][0], hist[0][0])} and tables == [200, 200],
          "every row of world data was created and updated then")

    upper = world_text(lambda t: t.replace("id = " + world_id(), "id = " + world_id().upper()))
    path = os.path.join(TMP, "world-upper")
    os.mkdir(path)
    rc, _, err = run("mkgame", upper, path)
    db = sqlite3.connect(os.path.join(path, "game.db"))
    got = db.execute("SELECT world_id FROM world").fetchall()
    db.close()
    check(rc == 0 and got == [(world_id(),)], "an id in capitals is kept in small letters")

    for what, change, words in [
            ("a world without an id",
             lambda t: t.replace("id = " + world_id() + "\n", ""), ["has no id", ":5:"]),
            ("an id that is too short",
             lambda t: t.replace("id = " + world_id(), "id = " + world_id()[:-1]), ["UUID"]),
            ("an id that isn't hex",
             lambda t: t.replace("id = " + world_id(), "id = g" + world_id()[1:]), ["UUID"]),
            ("an id without its dashes",
             lambda t: t.replace("id = " + world_id(), "id = " + world_id().replace("-", "x")),
             ["UUID"])]:
        path = os.path.join(TMP, "world-" + str(len(os.listdir(TMP))))
        os.mkdir(path)
        rc, _, err = run("mkgame", world_text(change), path)
        check(rc == 1 and all(w in err for w in words) and os.listdir(path) == [],
              "mkgame refuses %s, and writes nothing" % what)


def test_world_unknown():
    """#138: a game from before migration 4 has no world, and says so."""
    g = game("world-v3")
    db = sqlite3.connect(os.path.join(g, "game.db"))
    db.executescript("".join("DROP TABLE %s; " % t for v in range(4, VERSION + 1)
                             for t in OLD_TABLES[v])
                     + "".join("ALTER TABLE %s DROP COLUMN %s; " % tc
                               for v in range(4, VERSION + 1) for tc in OLD_COLUMNS.get(v, []))
                     + "PRAGMA user_version = 3;")
    db.close()
    rc, out, _ = run("upgradegame", g)
    db = sqlite3.connect(os.path.join(g, "game.db"))
    world = db.execute("SELECT count(*) FROM world").fetchone()[0]
    times = db.execute("SELECT DISTINCT created_at, updated_at FROM rooms").fetchall()
    db.close()
    check(rc == 0 and world == 0 and times == [(0, 0)],
          "a version 3 game upgrades with no world and its times unknown (0)")
    rc, out, err = run("scepterd", "-c", g, stdin="")
    check("world unknown" in err, "scepterd says its world is unknown")
    run("dmadmin", g, "create", "Boss", "master", stdin="boss password\n")
    _, out, _ = run("scepterd", "-c", g, stdin="boss\nboss password\n*world\nquit\ny\n")
    check("doesn't record its world" in out and "Changed by DMs since then: nothing." in out,
          "and so does *WORLD")


def test_world_edits():
    """#138: the DM editor marks what it changes; play doesn't."""
    g = game("world-edits")
    _, err0 = run("scepterd", "-c", g, stdin="")[1:]
    check("world alpha, id " + world_id() in err0, "scepterd logs the world at start-up")
    run("dmadmin", g, "create", "Boss", "master", stdin="boss password\n")

    def stamps():
        db = sqlite3.connect(os.path.join(g, "game.db"))
        try:
            since = db.execute("SELECT max(time) FROM world_history").fetchone()[0]
            got = set()
            for kind, sql in [
                    ("room", "SELECT room FROM rooms WHERE updated_at > ?"),
                    ("description", "SELECT DISTINCT record FROM descriptions WHERE updated_at > ?"),
                    ("monster", "SELECT number FROM monster_templates WHERE updated_at > ?"),
                    ("object", "SELECT number FROM object_templates WHERE updated_at > ?"),
                    ("encounter", "SELECT encounter FROM encounter_times WHERE updated_at > ?"),
                    ("treasure", "SELECT treasure FROM treasure_times WHERE updated_at > ?")]:
                got |= {(kind, r[0]) for r in db.execute(sql, (since,))}
            return got
        finally:
            db.close()

    # play: walking (segment swaps), a character saved, the hourly update, a stop
    play = "boss\nboss password\n" + "north\nsouth\n" * 5 + "*update\nsave\nquit\ny\n"
    run("scepterd", "-c", g, stdin=play)
    # room contents and settings aren't world data an update keeps
    run("scepterd", "-c", g, stdin="boss\nboss password\n*edit\n"
        "create object x 3 na=\"rock,grey *\"\nquit\n*notice Hello.\nquit\ny\n")
    check(stamps() == set(), "play, room contents and the notice mark nothing")

    _, out, _ = run("scepterd", "-c", g, stdin="boss\nboss password\n*edit\n"
                    "modify room 2 sa=t\ncopy room 2 5\nmodify olist 3 we=9\n"
                    "modify mlist 1 hi=20\nmodify elist 2 1 2 0 0 0 0 0 0\n"
                    "modify tlist 4 1 0 0 0 0 0 0 0\nmodify description 3 2\n"
                    "1/A new line./\nquit\n*world\nquit\ny\n")
    want = {("room", 2), ("room", 5), ("object", 3), ("monster", 1), ("encounter", 2),
            ("treasure", 4), ("description", 3)}
    check(stamps() == want, "the editor marks each room, template, table row and"
          " description it changes, within the second it was made in too")
    check("Changed by DMs since then: room 2, room 5, description 3, monster 1," in out
          and "encounter 2, treasure 4." in out, "and *WORLD lists them")
    db = sqlite3.connect(os.path.join(g, "game.db"))
    made = db.execute("SELECT DISTINCT created_at FROM rooms").fetchall()
    db.close()
    check(len(made) == 1, "an edit leaves created_at alone")


def test_fightsim():
    g = game("fight")
    rc, _, err = run("fightsim", "-r", "1", g, "4", "9")
    check(rc == 0 and "won 199 (99.5%)" in err and "average 2.74" in err,
          "fightsim gives its usual results")


def main():
    try:
        test_missing_settings()
        test_gamedir()
        test_database()
        test_dbfuncs()
        test_columns()
        test_upgradegame()
        for version in range(1, VERSION):
            test_upgrade_old(version)
        test_migration()
        test_world_made()
        test_world_unknown()
        test_world_edits()
        test_fightsim()
    finally:
        shutil.rmtree(TMP)
    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
