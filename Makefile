# Scepter of Goth -- macOS/Linux build.  The original QNX build is in qnx/.

CC      ?= cc
CSTD    ?= -std=c11
# glibc hides POSIX functions (strdup, getopt, ...) under strict -std=c11
FEATURES = -D_DEFAULT_SOURCE
WARN     = -Wall -Wno-parentheses -Wno-dangling-else -Wno-missing-braces -Wno-switch
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

OBJS = scepter.o cmd1.o cmd2.o cmd3.o cmd4.o datafile.o db.o execute.o \
	edit1.o edit2.o io.o ipc.o log.o main.o matt.o \
	mon.o obj.o room.o special.o spells.o telnet.o user.o rand.o

all: scepterd mkgame upgradegame

scepterd: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(CRYPTLIB)

# World compiler; must be built from the same headers as scepterd.
mkgame: mkgame.o datafile.o
	$(CC) $(CFLAGS) -o $@ mkgame.o datafile.o

# Converts an older game directory to this build's data format.
upgradegame: tools/upgradegame.o datafile.o
	$(CC) $(CFLAGS) -o $@ tools/upgradegame.o datafile.o

tools/upgradegame.o: tools/upgradegame.c common.h scepter.h proto.h
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) -I. -c tools/upgradegame.c -o $@

%.o: %.c
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) -c $<

user.o: user.c
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) $(CRYPTINC) -c $<

$(OBJS) mkgame.o: common.h scepter.h proto.h

# Combat simulator (tools/fightsim.c); links the engine without main.o.
SIMOBJS = $(filter-out main.o,$(OBJS)) tools/fightsim.o

fightsim: $(SIMOBJS)
	$(CC) $(CFLAGS) -o $@ $(SIMOBJS) $(CRYPTLIB)

tools/fightsim.o: tools/fightsim.c main.c common.h scepter.h proto.h
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) -I. -c tools/fightsim.c -o $@

# DM-only and bad-data security tests (tools/dmtest.c); links the engine
# without main.o, like fightsim.
DMOBJS = $(filter-out main.o,$(OBJS)) tools/dmtest.o

dmtest: $(DMOBJS)
	$(CC) $(CFLAGS) -o $@ $(DMOBJS) $(CRYPTLIB)

tools/dmtest.o: tools/dmtest.c main.c common.h scepter.h proto.h
	$(CC) $(CSTD) $(FEATURES) $(WARN) $(CFLAGS) $(DEFS) -I. -c tools/dmtest.c -o $@

# Each world file must match its JSON copy (tools/worldjson.py).
check-worlds:
	@for w in var/*/*.world; do \
		python3 tools/worldjson.py check $$w $$(dirname $$w)/world.json || exit 1; \
	done

# Telnet front end tests over raw sockets (tools/nettest.py).
check-net: scepterd mkgame
	python3 -I tools/nettest.py ./scepterd ./mkgame var/alpha/alpha.world

# The same server played by TinTin++ (skipped if tt++ is not installed).
check-tintin: scepterd mkgame
	sh tools/tintintest.sh ./scepterd ./mkgame var/alpha/alpha.world

# Data file headers and tools/upgradegame (tools/datatest.py).
check-data: scepterd mkgame upgradegame fightsim dmtest
	python3 -I tools/datatest.py . var/alpha/alpha.world

# Player-reachable security bugs (tools/securitytest.py).  Run it on a build
# that makes memory errors visible:
#   make CFLAGS='-g -O1 -fsanitize=address,undefined -ftrivial-auto-var-init=pattern'
# The pattern fill turns an uninitialized pointer (#11) into a sure crash.
check-security: scepterd mkgame dmtest
	python3 -I tools/securitytest.py ./scepterd ./mkgame var/alpha/alpha.world

clean:
	rm -f scepterd mkgame fightsim dmtest upgradegame $(OBJS) mkgame.o tools/fightsim.o \
		tools/dmtest.o tools/upgradegame.o

.PHONY: all clean check-worlds check-net check-tintin check-data check-security
