#!/usr/bin/env python3
"""Golden dumps of whole games (tools/dumpgame.c, #146).

    python3 -I tools/dumptest.py [--update] [BINDIR]

BINDIR holds mkgame, dmtest and dumpgame (default: .).  Each case builds a
game in a temporary directory and dumps it:

    alpha       mkgame var/alpha/alpha.world
    havenford   mkgame var/havenford/havenford.world
    inuse       alpha, then `dmtest GAME 146`: a game in use

and compares the dump with tools/dumps/CASE.dump byte for byte.  The move
to SQLite (#71) must keep these dumps as they are; --update rewrites them,
for a change to a world file or a deliberate change to what is saved.

It also checks that the dumps name every field of the saved structs in
scepter.h and special.c, so a field added without being dumped fails.
`make check-dump` runs this.
"""

import difflib
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DUMPS = os.path.join(HERE, "dumps")
CASES = [
    ("alpha", "var/alpha/alpha.world", None),
    ("havenford", "var/havenford/havenford.world", None),
    ("inuse", "var/alpha/alpha.world", "146"),
]

# Saved struct fields the dump leaves out or shows in another form, and why.
NOT_DUMPED = {
    "tObjectType": {"Next": "pointer",
                    "Object": "the fields of its kind (tPortal, tWeap, ...)"},
    "tChest": {"ObjectTail": "the contents are dumped as obj N.M lines"},
    "tMonsterType": {"Next": "pointer", "DefMon": "pointer, play only",
                     "DefPlayer": "pointer, play only",
                     "CharmPlayer": "pointer, play only",
                     "ObjectTail": "the inventory is dumped as obj lines"},
    "RoomType": {"RmMonsterTail": "the monsters are dumped as mon lines",
                 "RmPlayerTail": "pointer, play only",
                 "RmObjectTail": "the objects are dumped as obj lines"},
    "tUserFRec": {"User": "the player line",
                  "NObj": "the player's obj lines", "Objs": "the player's obj lines",
                  "NWeap": "Worn=", "NArm": "Worn=", "NShield": "Worn=",
                  "NRingL": "Worn=", "NRingR": "Worn="},
    "SpecialType": {"Room": "the special line's room number",
                    "NStock": "Stock=", "NStagger": "Stagger="},
}

# ObClass values and the union member that holds their own fields
KINDS = {"Portal": "tPortal", "Weap": "tWeap", "Shield": "tShield",
         "Armor": "tArmor", "Coins": "tCoins", "Scroll": "tScroll",
         "Chest": "tChest", "Door": "tDoor", "Keys": "tKeys",
         "MagDevice": "tMagDevice", "Teleport": "tTeleport", "Ring": "tRing"}

failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1
    return cond


def body(src, start):
    """The text between the brace at or after start and its match."""
    i = src.index("{", start)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if not depth:
                return src[i + 1:j]
    raise ValueError("unbalanced braces")


def fields(text):
    """Field names declared directly in a struct body (nested ones skipped)."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    while "{" in text:
        i = text.index("{")
        text = text[:i] + text[i + len(body(text, i)) + 2:]
    names = []
    for decl in text.split(";"):
        decl = re.sub(r"\[[^]]*\]|:\s*\d+", "", decl).strip()
        if not decl:
            continue
        parts = decl.split(",")
        names.append(re.findall(r"\w+", parts[0])[-1])
        names += [p.strip(" *\t\n") for p in parts[1:]]
    return names


def structs():
    """{struct name: [fields]} for the saved structs."""
    with open(os.path.join(ROOT, "scepter.h")) as f:
        h = f.read()
    with open(os.path.join(ROOT, "special.c")) as f:
        sp = f.read()
    out = {}
    for tag in list(KINDS.values()) + ["tObjectType", "tMonsterType", "tUserSave",
                                       "tUserType", "tUserFRec"]:
        m = re.search(r"struct\s+%s\s*\{" % tag, h)
        out[tag] = fields(body(h, m.start()))
    m = re.search(r"typedef struct\s*\{(?:(?!typedef).)*?\}\s*RoomType;", h, re.S)
    out["RoomType"] = fields(body(h, m.start()))
    m = re.search(r"typedef struct\s*\{(?:(?!typedef).)*?\}\s*SpecialType;", sp, re.S)
    out["SpecialType"] = fields(body(sp, m.start()))
    return out


def keys(line):
    return set(re.findall(r" (\w+)=", line))


def test_coverage(dumps):
    s = structs()

    # tUserSave is what a player record saves of tUserType: they must agree
    user = s["tUserType"]
    check(user[:len(s["tUserSave"])] == s["tUserSave"],
          "tUserSave matches the saved part of tUserType")

    seen = {name: set() for name in s}
    found = {name: False for name in s}
    for text in dumps.values():
        for line in text.splitlines():
            k = keys(line)
            kind = None
            if re.match(r"(room \d+ (mon \d+ )?|player \S+ |)obj ", line) \
                    or line.startswith("olist "):
                kind = "tObjectType"
                cls = re.search(r" ObClass=(\w+)", line).group(1)
                if cls in KINDS:
                    seen[KINDS[cls]] |= k
                    found[KINDS[cls]] = True
                k = k - set(s.get(KINDS.get(cls), []))
                if line.startswith("player "):
                    seen["tUserFRec"] |= {"Worn"} & k
                    found["tUserFRec"] = True
                    k.discard("Worn")
            elif line.startswith("mlist ") or re.match(r"room \d+ mon \d+ Name=", line):
                kind = "tMonsterType"
            elif re.match(r"room \d+ Adjoin=", line):
                kind = "RoomType"
            elif line.startswith("player Name="):
                kind = "tUserSave"
            elif line.startswith("special "):
                kind = "SpecialType"
            if kind:
                seen[kind] |= k
                found[kind] = True
                extra = k - set(s[kind])
                if extra:
                    check(False, "%s line has unknown fields %s: %s"
                          % (kind, sorted(extra), line[:60]))

    for name, want in s.items():
        if name in ("tUserType", "tUserFRec"):
            continue
        skip = NOT_DUMPED.get(name, {})
        missing = [f for f in want if f not in skip and f not in seen[name]]
        check(found[name] and not missing,
              "every saved field of %s is dumped%s" % (name, ": missing "
              + ", ".join(missing) if missing else ""))
    for name, skip in NOT_DUMPED.items():
        stale = [f for f in skip if f not in s[name]]
        check(not stale, "the fields left out of %s exist%s" % (name, ": not "
              + ", ".join(stale) if stale else ""))
    check(found["tUserFRec"] and "Worn" in seen["tUserFRec"],
          "a player's worn objects are dumped")


def run(bindir, prog, *args):
    p = subprocess.run([os.path.join(bindir, prog)] + list(args),
                       capture_output=True, text=True, timeout=120)
    if p.returncode:
        raise RuntimeError("%s %s: exit %d\n%s" % (prog, " ".join(args),
                                                   p.returncode, p.stderr))
    return p.stdout


def main():
    update = "--update" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--update"]
    bindir = os.path.abspath(args[0] if args else ".")
    tmp = tempfile.mkdtemp()
    dumps = {}
    try:
        for name, world, test in CASES:
            game = os.path.join(tmp, name)
            run(bindir, "mkgame", os.path.join(ROOT, world), game)
            if test:
                run(bindir, "dmtest", game, test)
            dumps[name] = run(bindir, "dumpgame", game)
            again = run(bindir, "dumpgame", game)
            check(again == dumps[name], "%s: a second dump is the same" % name)
    finally:
        shutil.rmtree(tmp)

    for name, text in dumps.items():
        path = os.path.join(DUMPS, name + ".dump")
        if update:
            with open(path, "w") as f:
                f.write(text)
            print("wrote " + os.path.relpath(path, ROOT))
            continue
        with open(path) as f:
            want = f.read()
        if not check(text == want, "%s matches %s" % (name, os.path.relpath(path, ROOT))):
            diff = difflib.unified_diff(want.splitlines(), text.splitlines(),
                                        "golden", "now", lineterm="")
            for line in list(diff)[:40]:
                print("    " + line)

    test_coverage(dumps)
    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
