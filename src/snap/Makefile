CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=gnu17 -Wall -Wextra -D_FORTIFY_SOURCE=3 -fstack-protector-strong
LDFLAGS += -Wl,-z,relro,-z,now

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

OBJS = snap.o store.o btrfs.o diff.o restore.o boot.o menu.o manifest.o sha256.o util.o

all: snap

snap: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(OBJS): snap.h sha256.h

install: snap
	install -Dm755 snap $(DESTDIR)$(BINDIR)/snap

clean:
	rm -f snap $(OBJS)

.PHONY: all install clean
