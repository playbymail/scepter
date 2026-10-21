#!/usr/bin/env python3
"""Tests of mkgame's checks on areas and room coordinates (#141).

    python3 -I tools/worldtest.py [MKGAME [WORLD]]

MKGAME defaults to ./mkgame and WORLD to var/alpha/alpha.world.  Each case
appends blocks to WORLD and checks what mkgame says about them.
`make check-worlds` runs this.
"""

import os
import shutil
import subprocess
import sys
import tempfile

MKGAME = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./mkgame")
WORLD = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "var/alpha/alpha.world")
TMP = tempfile.mkdtemp()
failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1
    return cond


def mkgame(extra):
    """mkgame on WORLD plus extra; returns (exit code, stderr)."""
    world = os.path.join(TMP, "test.world")
    with open(WORLD) as f, open(world, "w") as out:
        out.write(f.read() + "\n" + extra)
    p = subprocess.run([MKGAME, "-f", world, os.path.join(TMP, "game")],
                       capture_output=True, text=True, timeout=60)
    return p.returncode, p.stderr


def refused(extra, message, what):
    rc, err = mkgame(extra)
    check(rc != 0 and message in err, what)


try:
    rc, err = mkgame("")
    check(rc == 0, "the world compiles with its areas and coordinates")

    rc, err = mkgame("[area 9]\nname = Outskirts\n\n"
                     "[room 30]\narea = 9\nx = -999\ny = 999\nz = -3\n")
    check(rc == 0 and "error" not in err, "a new area and a placed room are accepted")

    refused("[area 9]\n", "area 9 has no name", "an area without a name")
    rc, err = mkgame("[area 9]\nname = A\tB\n")
    check(rc != 0 and "printable" in err and "has no name" not in err,
          "an unprintable name, reported once")
    refused("[area 9]\nname = " + "x" * 61 + "\n", "longer than 60",
            "a name longer than 60 characters")
    refused("[area 9]\nname = A\ncolour = red\n", "unknown area key",
            "an unknown area key")
    refused("[area 1]\nname = Again\n", "area 1 defined twice", "an area defined twice")
    refused("[area 256]\nname = A\n", "out of range", "an area number over 255")
    refused("[room 30]\narea = 7\n", "room 30 is in undefined area 7",
            "a room in an undefined area")
    refused("[room 30]\ny = 4\n", "room 30 has coordinates but no area",
            "coordinates on a room without an area")
    refused("[room 30]\narea = 1\nx = 1000\n", "out of range",
            "a coordinate out of range")

    rc, err = mkgame("[room 30]\narea = 1\n")
    check(rc == 0 and "room 30 is at the same place as room 1" in err,
          "two rooms at the same place in an area are warned about")
finally:
    shutil.rmtree(TMP)

if failures:
    sys.exit(f"{failures} failure(s)")
