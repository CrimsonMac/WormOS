CC      ?= cc
CFLAGS  ?= -O2
# override: these must survive a CFLAGS given on the command line.
override CFLAGS += -std=gnu17 -Wall -Wextra -I../snap
LDFLAGS += -Wl,-z,relro,-z,now

# Monocypher: the static library by default. A build for the build host
# passes its sources instead (see tools/wrepo).
MONO_CFLAGS ?=
MONO_LIBS   ?= -lmonocypher

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

SRCS = wh.c repo.c db.c pkg.c util.c sig.c ../snap/sha256.c

all: wh

wh: $(SRCS) wh.h sig.h
	$(CC) $(CFLAGS) $(MONO_CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(MONO_LIBS)

install: wh
	install -Dm755 wh $(DESTDIR)$(BINDIR)/wh

clean:
	rm -f wh

.PHONY: all install clean
