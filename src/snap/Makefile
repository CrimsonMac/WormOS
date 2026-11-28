CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=gnu17 -Wall -Wextra -D_FORTIFY_SOURCE=3 -fstack-protector-strong
LDFLAGS += -Wl,-z,relro,-z,now

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

MONO_CFLAGS ?=
MONO_LIBS   ?= -lmonocypher

OBJS = snap.o store.o btrfs.o diff.o restore.o boot.o stage.o menu.o status.o manifest.o seal.o sha256.o util.o

all: snap

snap: $(OBJS)
	$(CC) $(CFLAGS) $(MONO_CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(MONO_LIBS)

seal.o: seal.c
	$(CC) $(CFLAGS) $(MONO_CFLAGS) -c -o $@ seal.c

$(OBJS): snap.h sha256.h

install: snap
	install -Dm755 snap $(DESTDIR)$(BINDIR)/snap

clean:
	rm -f snap $(OBJS)

.PHONY: all install clean
