CC      ?= cc
CFLAGS  ?= -O2
override CFLAGS += -std=gnu17 -Wall -Wextra -I../common
LDFLAGS += -Wl,-z,relro,-z,now

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

all: worm power snapshots

OBJS = worm.o fw.o panic.o status.o anon.o

worm: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(OBJS): worm.h

# The setuid helpers for the desktop.
power: power.c admin.c admin.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ power.c admin.c ../common/password.c -lcrypt

snapshots: snapshots.c admin.c admin.h ../common/password.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ snapshots.c admin.c ../common/password.c -lcrypt

install: worm power snapshots
	install -Dm755 worm $(DESTDIR)$(BINDIR)/worm
	install -Dm4755 power $(DESTDIR)$(PREFIX)/lib/worm/power
	install -Dm4755 snapshots $(DESTDIR)$(PREFIX)/lib/worm/snapshots

clean:
	rm -f worm power snapshots $(OBJS)

.PHONY: all install clean
