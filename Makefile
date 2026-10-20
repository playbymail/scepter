# Scepter of Goth -- macOS/Linux build.  The original QNX build is in qnx/.

CC      ?= cc
CSTD    ?= -std=c11
WARN     = -Wall -Wno-parentheses -Wno-dangling-else -Wno-missing-braces -Wno-switch
CFLAGS  ?= -g -O2
# Data paths default to the current directory; override with e.g.
#   make DEFS='-DWORKDIR=\"/var/lib/scepter\"'
DEFS    ?=

OBJS = scepter.o cmd1.o cmd2.o cmd3.o cmd4.o db.o execute.o \
	edit1.o edit2.o io.o ipc.o log.o main.o matt.o \
	mon.o obj.o room.o special.o spells.o user.o rand.o

all: scepterd mkgame

scepterd: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

# World compiler; must be built from the same headers as scepterd.
mkgame: mkgame.o
	$(CC) $(CFLAGS) -o $@ mkgame.o

%.o: %.c
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(DEFS) -c $<

$(OBJS) mkgame.o: common.h scepter.h proto.h

# Combat simulator (tools/fightsim.c); links the engine without main.o.
SIMOBJS = $(filter-out main.o,$(OBJS)) tools/fightsim.o

fightsim: $(SIMOBJS)
	$(CC) $(CFLAGS) -o $@ $(SIMOBJS)

tools/fightsim.o: tools/fightsim.c main.c common.h scepter.h proto.h
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(DEFS) -I. -c tools/fightsim.c -o $@

# Each world file must match its JSON copy (tools/worldjson.py).
check-worlds:
	@for w in var/*/*.world; do \
		python3 tools/worldjson.py check $$w $$(dirname $$w)/world.json || exit 1; \
	done

clean:
	rm -f scepterd mkgame fightsim $(OBJS) mkgame.o tools/fightsim.o

.PHONY: all clean check-worlds
