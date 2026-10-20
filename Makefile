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
	mon.o obj.o room.o spells.o user.o rand.o

all: scepterd

scepterd: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(DEFS) -c $<

$(OBJS): common.h scepter.h proto.h

clean:
	rm -f scepterd $(OBJS)

.PHONY: all clean
