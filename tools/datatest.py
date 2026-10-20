#!/usr/bin/env python3
"""Tests of the game data file headers (datafile.c) and tools/upgradegame.

    python3 -I tools/datatest.py [BINDIR [WORLD]]

BINDIR holds scepterd, mkgame, upgradegame, fightsim and dmtest (default: .).
Each case builds a game from WORLD (default var/alpha/alpha.world) in a
temporary directory, damages or downgrades it, and checks what scepterd and
upgradegame make of it.  `make check-data` runs this.
"""

import filecmp
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

HEAD = 32
VERSION = 2
FILES = {
    "ram/edungeon.new": b"SCEPDUNG", "ram/dbobj.new": b"SCEPDBOB",
    "ram/dbmon.new": b"SCEPDBMN", "ram/eperson.new": b"SCEPPERS",
    "ram/mlist.new": b"SCEPMLST", "dat/mlist.new": b"SCEPMLST",
    "ram/olist.new": b"SCEPOLST", "dat/olist.new": b"SCEPOLST",
    "ram/dscrpt": b"SCEPDESC", "dat/dscrpt": b"SCEPDESC",
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
    p = os.path.join(path, "ram/eperson.new")
    with open(p, "rb") as fp:
        data = bytearray(fp.read())
    pad = lay["pwhash"] + lay["pwhashlen"]
    for i in range(HEAD, len(data), lay["record"]):
        data[i + pad:i + lay["usersave"]] = bytes(lay["usersave"] - pad)
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
        if f == "ram/eperson.new":
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
        ("wrong kind", "ram/dbobj.new", lambda g: patch(g, "ram/dbobj.new", 0, b"SCEPDBMN"),
         ["ram/dbobj.new", "wrong kind of file"]),
        ("newer version", "ram/dbmon.new",
         lambda g: patch(g, "ram/dbmon.new", 8, struct.pack("<I", VERSION + 1)),
         ["ram/dbmon.new", "format version %d" % (VERSION + 1), "newer scepterd"]),
        ("record size", "ram/olist.new",
         lambda g: patch(g, "ram/olist.new", 12, struct.pack("<I", 12345)),
         ["ram/olist.new", "12345 bytes"]),
        ("truncated header", "ram/eperson.new",
         lambda g: open(os.path.join(g, "ram/eperson.new"), "wb").write(b"SCEPPERS\1\0"),
         ["ram/eperson.new", "truncated"]),
        ("foreign file", "dat/dscrpt",
         lambda g: open(os.path.join(g, "dat/dscrpt"), "wb").write(b"hello, world\n"),
         ["dat/dscrpt", "not a Scepter data file"]),
        ("missing file", "dat/mlist.new",
         lambda g: os.remove(os.path.join(g, "dat/mlist.new")),
         ["dat/mlist.new", "No such file"]),
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


def test_upgrade(fresh):
    g = game("v0")
    strip(g)
    os.chmod(os.path.join(g, "ram/eperson.new"), 0o640)
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
    mode = stat.S_IMODE(os.stat(os.path.join(g, "ram/eperson.new")).st_mode)
    check(mode == 0o640, "upgraded files keep their mode")
    ok, _ = server_ok(g)
    check(ok, "scepterd starts on the upgraded game")
    rc, out, _ = run("upgradegame", g)
    check(rc == 0 and "already" in out, "a second upgradegame changes nothing")


def test_upgrade1():
    # a version 1 game with a saved character
    g = game("v1")
    rc, _, err = run("dmtest", g, "legacy", "Oldtimer", "ac")
    check(rc == 0, "dmtest saves an old-style character")
    want = snapshot(g)
    clear_padding(want)
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
    check(same_tree(g, want), "the character is kept, with an empty password hash")
    ok, _ = server_ok(g)
    check(ok, "scepterd starts on the upgraded game")

    # the same character in a version 0 game
    g = game("v0person")
    run("dmtest", g, "legacy", "Oldtimer", "ac")
    want = snapshot(g)
    clear_padding(want)
    downgrade(g)
    strip(g)
    rc, out, err = run("upgradegame", g)
    check(rc == 0 and out.count("version 0, upgrading") == len(FILES)
          and same_tree(g, want), "upgradegame upgrades a version 0 player file")

    # a version 1 player file whose records are the wrong size
    g = game("v1bad")
    downgrade(g)
    patch(g, "ram/eperson.new", 12, struct.pack("<I", 12345))
    snap = snapshot(g)
    rc, _, err = run("upgradegame", g)
    check(rc == 1 and "12345 bytes" in err and same_tree(g, snap),
          "upgradegame refuses a version 1 file with the wrong record size")


def test_partial(fresh):
    # an upgrade that stopped part way: some files done, some not
    g = game("partial")
    strip(g, ["ram/dbobj.new", "dat/dscrpt", "ram/eperson.new"])
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
    strip(g, ["ram/mlist.new"])
    rc, _, err = run("fightsim", "-r", "1", g, "4", "9")
    check(rc == 1 and "ram/mlist.new" in err, "fightsim refuses a mismatched game")


def main():
    try:
        fresh = test_fresh()
        test_damaged()
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
