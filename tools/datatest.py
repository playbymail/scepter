#!/usr/bin/env python3
"""Tests of the game data file headers (datafile.c), tools/upgradegame,
mkgame's game directory and the game database (gamedb.c).

    python3 -I tools/datatest.py [BINDIR [WORLD]]

BINDIR holds scepterd, mkgame, upgradegame, fightsim, dmtest and dbtest
(default: .).  Each case builds a game from WORLD (default
var/alpha/alpha.world) in a temporary directory, damages or downgrades it,
and checks what scepterd and upgradegame make of it.  Refusals must leave
everything as it was: nothing created, nothing changed.
`make check-data` runs this.
"""

import filecmp
import os
import sqlite3
import time
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

HEAD = 32
VERSION = 2
FILES = {
    "edungeon": b"SCEPDUNG", "dbobj": b"SCEPDBOB",
    "dbmon": b"SCEPDBMN", "eperson": b"SCEPPERS",
    "mlist": b"SCEPMLST", "mlist.hd": b"SCEPMLST",
    "olist": b"SCEPOLST", "olist.hd": b"SCEPOLST",
    "dscrpt": b"SCEPDESC", "dscrpt.hd": b"SCEPDESC",
}

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


def strip(path, names=FILES):
    """Turn files back into version 0: drop their headers."""
    for f in names:
        p = os.path.join(path, f)
        with open(p, "rb") as fp:
            data = fp.read()
        with open(p, "wb") as fp:
            fp.write(data[HEAD:])


def layout():
    """Where the password fields are in a player record, from dmtest."""
    rc, out, err = run("dmtest", "-", "layout")
    assert rc == 0, err
    w = out.split()
    return {"pwhash": int(w[3]), "pwhashlen": int(w[4]), "usersave": int(w[6]),
            "record": int(w[8])}


def clear_padding(path):
    """Zero the padding after PWHash in each player record.  WritePlayer()
    saves whatever UserType holds there; upgradegame writes zeros."""
    lay = layout()
    p = os.path.join(path, "eperson")
    with open(p, "rb") as fp:
        data = bytearray(fp.read())
    pad = lay["pwhash"] + lay["pwhashlen"]
    for i in range(HEAD, len(data), lay["record"]):
        data[i + pad:i + lay["usersave"]] = bytes(lay["usersave"] - pad)
    with open(p, "wb") as fp:
        fp.write(data)


def lock(path):
    """Give each player record the locked hash that upgradegame writes (#50)."""
    lay = layout()
    p = os.path.join(path, "eperson")
    with open(p, "rb") as fp:
        data = bytearray(fp.read())
    at = lay["pwhash"]
    for i in range(HEAD, len(data), lay["record"]):
        data[i + at:i + at + lay["pwhashlen"]] = b"*".ljust(lay["pwhashlen"], b"\0")
    with open(p, "wb") as fp:
        fp.write(data)


def downgrade(path, names=FILES):
    """Turn version 2 files into version 1: player records lose PWHash."""
    lay = layout()
    for f in names:
        p = os.path.join(path, f)
        with open(p, "rb") as fp:
            head, data = bytearray(fp.read(HEAD)), fp.read()
        head[8:12] = struct.pack("<I", 1)
        if f == "eperson":
            size, at, cut = lay["record"], lay["pwhash"], lay["pwhashlen"]
            head[12:16] = struct.pack("<I", size - cut)
            data = b"".join(data[i:i + at] + data[i + at + cut:i + size]
                            for i in range(0, len(data), size))
        with open(p, "wb") as fp:
            fp.write(head + data)


def patch(path, f, offset, data):
    with open(os.path.join(path, f), "r+b") as fp:
        fp.seek(offset)
        fp.write(data)


def snapshot(path):
    copy = path + ".snap"
    shutil.copytree(path, copy)
    return copy


def same_tree(a, b):
    c = filecmp.dircmp(a, b)
    def walk(c):
        if c.left_only or c.right_only or c.diff_files or c.funny_files:
            return False
        return all(walk(s) for s in c.subdirs.values())
    # dircmp compares shallowly by stat; force content comparison
    for root, _, files in os.walk(a):
        for f in files:
            pa = os.path.join(root, f)
            pb = os.path.join(b, os.path.relpath(pa, a))
            if not os.path.exists(pb) or not filecmp.cmp(pa, pb, shallow=False):
                return False
    return walk(c)


def test_fresh():
    g = game("fresh")
    good = True
    for f, magic in FILES.items():
        with open(os.path.join(g, f), "rb") as fp:
            head = fp.read(HEAD)
        good &= (len(head) == HEAD and head[:8] == magic
                 and struct.unpack("<I", head[8:12])[0] == VERSION
                 and head[16:] == bytes(16))
    check(good, "mkgame writes a magic, version %d and zero padding to every file"
          % VERSION)
    ok, err = server_ok(g)
    check(ok, "scepterd starts on a fresh game")
    rc, out, _ = run("upgradegame", g)
    check(rc == 0 and "already at format version %d" % VERSION in out,
          "upgradegame leaves a fresh game alone")
    return game("pristine")		# never run, for comparisons


def test_damaged():
    cases = [
        ("wrong kind", "dbobj", lambda g: patch(g, "dbobj", 0, b"SCEPDBMN"),
         ["dbobj", "wrong kind of file"]),
        ("newer version", "dbmon",
         lambda g: patch(g, "dbmon", 8, struct.pack("<I", VERSION + 1)),
         ["dbmon", "format version %d" % (VERSION + 1), "newer scepterd"]),
        ("record size", "olist",
         lambda g: patch(g, "olist", 12, struct.pack("<I", 12345)),
         ["olist", "12345 bytes"]),
        ("truncated header", "eperson",
         lambda g: open(os.path.join(g, "eperson"), "wb").write(b"SCEPPERS\1\0"),
         ["eperson", "truncated"]),
        ("foreign file", "dscrpt.hd",
         lambda g: open(os.path.join(g, "dscrpt.hd"), "wb").write(b"hello, world\n"),
         ["dscrpt.hd", "not a Scepter data file"]),
        ("missing file", "mlist.hd",
         lambda g: os.remove(os.path.join(g, "mlist.hd")),
         ["mlist.hd", "No such file"]),
    ]
    for i, (what, f, damage, words) in enumerate(cases):
        g = game("damaged%d" % i)
        damage(g)
        ok, err = server_refuses(g, *words)
        check(ok, "scepterd refuses a game with a %s (%s)" % (what, f))
        if not ok:
            print(err)
        snap = snapshot(g)
        rc, _, err = run("upgradegame", g)
        check(rc == 1 and "nothing changed" in err and same_tree(g, snap)
              and not [d for d in os.listdir(TMP) if d.startswith("damaged%d.backup" % i)],
              "upgradegame refuses it too and changes nothing")


def test_missing_efile():
    # #53: an error before the main loop is logged with the real time
    g = game("noefile")
    os.remove(os.path.join(g, "efile"))
    ok, err = server_refuses(g, "File open error on EFILE")
    check(ok, "scepterd reports a missing efile on stderr")
    with open(os.path.join(g, "errlog")) as f:
        log = f.read()
    check(time.strftime("%Y") in log.split("\n")[0] and "File open error on EFILE" in log,
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
    refuses("a newer schema version", lambda g: set_version(g, 2),
            "schema version 2", "newer scepterd")
    if ROOT:
        print("skip a read-only database: root can write to it")
    else:
        refuses("a read-only database",
                lambda g: os.chmod(os.path.join(g, "game.db"), 0o444), "read-only")

    def garble(g):
        with open(os.path.join(g, "game.db"), "r+b") as f:
            f.write(b"this is not a database at all, not even close" * 4)
    refuses("a corrupt database", garble, "not a database")

    def not_wal(g):
        db = sqlite3.connect(os.path.join(g, "game.db"))
        db.execute("PRAGMA journal_mode = DELETE")
        db.close()
    refuses("a database not in WAL mode", not_wal, "not in WAL mode")

    rc, _, err = run("scepterd", "-c", os.path.join(TMP, "nogame"), stdin="")
    check(rc == 1 and not os.path.lexists(os.path.join(TMP, "nogame")),
          "scepterd refuses a missing game directory and creates nothing")

    g = game("db-fight")
    set_version(g, 0)
    rc, _, err = run("fightsim", "-r", "1", g, "4", "9")
    check(rc == 1 and "schema version 0" in err, "fightsim refuses it too")


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
    db.execute("INSERT INTO news VALUES (1, 'after the backup')")
    db.commit()
    db.close()
    rc, _, err = dbtest("restore", backup, g)
    db = sqlite3.connect(os.path.join(g, "game.db"))
    n = db.execute("SELECT count(*) FROM news").fetchone()[0]
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
    check(rc == 0 and "would migrate to 1" in out and tree(old) == before,
          "migrate -n: says what it would do and changes nothing")
    rc, out, _ = dbtest("migrate", old)
    backups = [f for f in os.listdir(old) if f.startswith("game.db.backup-")]
    rc2, ver, _ = dbtest("version", old)
    check(rc == 0 and len(backups) == 1 and ver.strip() == "1",
          "migrate: backs up to a file in the game directory, then reaches version 1")
    rc, _, _ = dbtest("open", old)
    check(rc == 0, "open: the migrated game opens")
    rc, out, _ = dbtest("migrate", old)
    check(rc == 0 and "already at schema version 1" in out, "migrate: a current game is left alone")
    os.chmod(ro, 0o755)


def test_upgrade(fresh):
    g = game("v0")
    strip(g)
    os.chmod(os.path.join(g, "eperson"), 0o640)
    v0 = snapshot(g)

    ok, err = server_refuses(g, "no header", "upgradegame")
    check(ok and all(f in err for f in FILES),
          "scepterd refuses a version 0 game, naming every file")

    rc, out, _ = run("upgradegame", "-n", g)
    check(rc == 0 and out.count("would upgrade") == len(FILES) and same_tree(g, v0),
          "upgradegame -n reports every file and changes nothing")

    rc, out, err = run("upgradegame", g)
    check(rc == 0 and "now at format version %d" % VERSION in out,
          "upgradegame upgrades a version 0 game")
    backups = [d for d in os.listdir(TMP) if d.startswith("v0.backup-")]
    check(len(backups) == 1 and same_tree(os.path.join(TMP, backups[0]), v0),
          "the backup is an exact copy of the version 0 game")
    check(same_tree(g, fresh), "the upgraded game is identical to a fresh one")
    mode = stat.S_IMODE(os.stat(os.path.join(g, "eperson")).st_mode)
    check(mode == 0o640, "upgraded files keep their mode")
    ok, _ = server_ok(g)
    check(ok, "scepterd starts on the upgraded game")
    rc, out, _ = run("upgradegame", g)
    check(rc == 0 and "already" in out, "a second upgradegame changes nothing")


def test_upgrade1():
    # a version 1 game with a saved character
    g = game("v1")
    rc, _, err = run("dmtest", g, "player", "Oldtimer", "ac secret")
    check(rc == 0, "dmtest saves a character")
    want = snapshot(g)
    clear_padding(want)
    lock(want)
    downgrade(g)
    ok, err = server_refuses(g, "format version 1", "upgradegame")
    check(ok and all(f in err for f in FILES),
          "scepterd refuses a version 1 game, naming every file")
    rc, out, _ = run("upgradegame", "-n", g)
    check(rc == 0 and out.count("version 1, would upgrade") == len(FILES),
          "upgradegame -n reports every version 1 file")
    rc, out, err = run("upgradegame", g)
    check(rc == 0 and "now at format version %d" % VERSION in out,
          "upgradegame upgrades a version 1 game")
    check(same_tree(g, want), "the character is kept, with a locked password hash")
    ok, _ = server_ok(g)
    check(ok, "scepterd starts on the upgraded game")
    rc, out, _ = run("scepterd", "-c", g, stdin="Oldtimer\nac secret\n")
    check("Wrong password, sorry." in out, "the locked character refuses its old password")

    # the same character in a version 0 game
    g = game("v0person")
    run("dmtest", g, "player", "Oldtimer", "ac secret")
    want = snapshot(g)
    clear_padding(want)
    lock(want)
    downgrade(g)
    strip(g)
    rc, out, err = run("upgradegame", g)
    check(rc == 0 and out.count("version 0, upgrading") == len(FILES)
          and same_tree(g, want), "upgradegame upgrades a version 0 player file")

    # a version 1 player file whose records are the wrong size
    g = game("v1bad")
    downgrade(g)
    patch(g, "eperson", 12, struct.pack("<I", 12345))
    snap = snapshot(g)
    rc, _, err = run("upgradegame", g)
    check(rc == 1 and "12345 bytes" in err and same_tree(g, snap),
          "upgradegame refuses a version 1 file with the wrong record size")


def test_partial(fresh):
    # an upgrade that stopped part way: some files done, some not
    g = game("partial")
    strip(g, ["dbobj", "dscrpt.hd", "eperson"])
    rc, out, _ = run("upgradegame", g)
    check(rc == 0 and out.count("upgrading") == 3 and same_tree(g, fresh),
          "upgradegame finishes a partly upgraded game")


def test_fightsim(fresh):
    if not os.path.exists(os.path.join(BIN, "fightsim")):
        print("skip fightsim: not built")
        return
    g = game("fight")
    strip(g)
    run("upgradegame", g)
    a = run("fightsim", "-r", "1", fresh, "4", "9")
    b = run("fightsim", "-r", "1", g, "4", "9")
    check(a[0] == 0 and a == b and "won 199 (99.5%)" in a[2] and "average 2.74" in a[2],
          "fightsim gives the same results on a fresh and an upgraded game")
    strip(g, ["mlist"])
    rc, _, err = run("fightsim", "-r", "1", g, "4", "9")
    check(rc == 1 and "mlist" in err, "fightsim refuses a mismatched game")


def main():
    try:
        fresh = test_fresh()
        test_damaged()
        test_missing_efile()
        test_gamedir()
        test_database()
        test_dbfuncs()
        test_upgrade(fresh)
        test_upgrade1()
        test_partial(fresh)
        test_fightsim(fresh)
    finally:
        shutil.rmtree(TMP)
    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
