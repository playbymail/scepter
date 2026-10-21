# Scepter of Goth -- macOS/Linux build.  The original QNX build is in qnx/.

CC      ?= cc
CSTD    ?= -std=c11
# glibc hides POSIX functions (strdup, getopt, ...) under strict -std=c11
FEATURES = -D_DEFAULT_SOURCE
WARN     = -Wall -Wno-parentheses -Wno-dangling-else -Wno-missing-braces -Wno-switch \
	-Wno-format-truncation
# GCC finds overflows and unchecked reads that clang misses (#119); keep them
# errors so the Linux build stops on a new one.
ifneq ($(shell $(CC) --version 2>/dev/null | grep -c 'Free Software Foundation'),0)
WARN    += -Werror=format-overflow -Werror=maybe-uninitialized \
	-Werror=unused-result -Werror=array-parameter
endif
CFLAGS  ?= -g -O2
# Data paths default to the current directory; override with e.g.
#   make DEFS='-DWORKDIR=\"/var/lib/scepter\"'
DEFS    ?=
# Passwords are hashed with libxcrypt's bcrypt (#82): libcrypt-dev on Linux,
# `brew install libxcrypt` on macOS (keg-only, so point at it).
ifeq ($(shell uname -s),Darwin)
XCRYPT  ?= $(shell brew --prefix libxcrypt 2>/dev/null)
CRYPTINC = -I$(XCRYPT)/include
CRYPTLIB = -L$(XCRYPT)/lib -lcrypt
else
CRYPTLIB = -lcrypt
endif
# The game database (gamedb.c, #71) uses the system SQLite: the macOS SDK's,
# or libsqlite3-dev on Linux.  The droplet has the libsqlite3-0 runtime.
SQLITELIB = -lsqlite3
LIBS     = $(CRYPTLIB) $(SQLITELIB)

OBJS = scepter.o cmd1.o cmd2.o cmd3.o cmd4.o db.o execute.o \
	dbstore.o edit1.o edit2.o gamedb.o io.o ipc.o log.o main.o matt.o \
	mon.o obj.o room.o special.o spells.o telnet.o user.o rand.o

all: scepterd mkgame upgradegame dmadmin

scepterd: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

# World compiler; must be built from the same headers as scepterd.
mkgame: mkgame.o dbstore.o gamedb.o
	$(CC) $(CFLAGS) -o $@ mkgame.o dbstore.o gamedb.o $(SQLITELIB)

# Brings a game database up to this build's schema (tools/upgradegame.c).
upgradegame: tools/upgradegame.o gamedb.o
	$(CC) $(CFLAGS) -o $@ tools/upgradegame.o gamedb.o $(SQLITELIB)

tools/upgradegame.o: tools/upgradegame.c gamedb.h
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) -I. -c tools/upgradegame.c -o $@

%.o: %.c
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) -c $<

user.o: user.c
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) $(CRYPTINC) -c $<

$(OBJS) mkgame.o: common.h scepter.h proto.h gamedb.h

# Runs one function of the database module, for tools/datatest.py.
dbtest: tools/dbtest.o gamedb.o
	$(CC) $(CFLAGS) -o $@ tools/dbtest.o gamedb.o $(SQLITELIB)

tools/dbtest.o: tools/dbtest.c gamedb.h
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) -I. -c tools/dbtest.c -o $@

# dbtest with a test-only migration 2, to migrate a whole game.
dbtest2: tools/dbtest.o tools/gamedb2.o
	$(CC) $(CFLAGS) -o $@ tools/dbtest.o tools/gamedb2.o $(SQLITELIB)

tools/gamedb2.o: gamedb.c gamedb.h
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) -DTestMigration -c gamedb.c -o $@

# Combat simulator (tools/fightsim.c); links the engine without main.o.
SIMOBJS = $(filter-out main.o,$(OBJS)) tools/fightsim.o

fightsim: $(SIMOBJS)
	$(CC) $(CFLAGS) -o $@ $(SIMOBJS) $(LIBS)

tools/fightsim.o: tools/fightsim.c main.c common.h scepter.h proto.h gamedb.h
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) -I. -c tools/fightsim.c -o $@

# DM-only and bad-data security tests (tools/dmtest.c); links the engine
# without main.o, like fightsim.
DMOBJS = $(filter-out main.o,$(OBJS)) tools/dmtest.o

dmtest: $(DMOBJS)
	$(CC) $(CFLAGS) -o $@ $(DMOBJS) $(LIBS)

tools/dmtest.o: tools/dmtest.c main.c common.h scepter.h proto.h gamedb.h
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) -I. -c tools/dmtest.c -o $@

# Manages a game's DMs from outside the game (tools/dmadmin.c, #139); links
# the engine without main.o, like fightsim.
ADMINOBJS = $(filter-out main.o,$(OBJS)) tools/dmadmin.o

dmadmin: $(ADMINOBJS)
	$(CC) $(CFLAGS) -o $@ $(ADMINOBJS) $(LIBS)

tools/dmadmin.o: tools/dmadmin.c main.c common.h scepter.h proto.h gamedb.h
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) -I. -c tools/dmadmin.c -o $@

# Prints a game's whole saved state (tools/dumpgame.c); links the engine
# without main.o, like fightsim.
DUMPOBJS = $(filter-out main.o,$(OBJS)) tools/dumpgame.o

dumpgame: $(DUMPOBJS)
	$(CC) $(CFLAGS) -o $@ $(DUMPOBJS) $(LIBS)

tools/dumpgame.o: tools/dumpgame.c main.c common.h scepter.h proto.h gamedb.h
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) -I. -c tools/dumpgame.c -o $@

# Golden dumps of whole games (tools/dumptest.py); the move to SQLite (#71)
# must keep them byte for byte.
check-dump: mkgame dmtest dumpgame
	python3 -I tools/dumptest.py .

# Each world file must match its JSON copy (tools/worldjson.py), and mkgame
# must check areas and coordinates (tools/worldtest.py).
check-worlds: mkgame
	@for w in var/*/*.world; do \
		python3 tools/worldjson.py check $$w $$(dirname $$w)/world.json || exit 1; \
	done
	python3 -I tools/worldtest.py ./mkgame var/alpha/alpha.world

# Telnet front end tests over raw sockets (tools/nettest.py).
check-net: scepterd mkgame
	python3 -I tools/nettest.py ./scepterd ./mkgame var/alpha/alpha.world

# The same server played by TinTin++ (skipped if tt++ is not installed).
check-tintin: scepterd mkgame
	sh tools/tintintest.sh ./scepterd ./mkgame var/alpha/alpha.world

# Data file headers, tools/upgradegame, mkgame's game directory and the game
# database (tools/datatest.py).
check-data: scepterd mkgame upgradegame fightsim dmtest dumpgame dbtest dbtest2
	python3 -I tools/datatest.py . var/alpha/alpha.world

# dmadmin, and a DM it makes playing in the console harness
# (tools/dmadmintest.py).
check-dmadmin: scepterd mkgame dmadmin dmtest dumpgame
	python3 -I tools/dmadmintest.py . var/alpha/alpha.world

# Player-reachable security bugs (tools/securitytest.py).  Run it on a build
# that makes memory errors visible:
#   make CFLAGS='-g -O1 -fno-common -fsanitize=address,undefined -ftrivial-auto-var-init=pattern'
# The pattern fill turns an uninitialized pointer (#11) into a sure crash.
# -fno-common gives globals such as B1 the redzones that catch an overflow (#119).
check-security: scepterd mkgame dmtest
	python3 -I tools/securitytest.py ./scepterd ./mkgame var/alpha/alpha.world

clean:
	rm -f scepterd mkgame fightsim dmtest dumpgame dbtest dbtest2 upgradegame dmadmin \
		$(OBJS) mkgame.o tools/fightsim.o tools/dmtest.o tools/dumpgame.o \
		tools/dbtest.o tools/gamedb2.o tools/upgradegame.o tools/dmadmin.o

# The web site for sceptish.pbbgaming.com (docs/deploy.md), built into
# out/site.
site: site/index.html site/avatar.webp var/havenford/world-map.html
	mkdir -p out/site
	cp site/index.html site/avatar.webp out/site/
	cp var/havenford/world-map.html out/site/map.html
	magick site/avatar.webp -resize 192x192 -strip -colors 128 out/site/icon.png

.PHONY: all clean check-worlds site check-net check-tintin check-data check-security \
	check-dump check-dmadmin
