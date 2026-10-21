CC      ?= cc
CFLAGS  ?= -O2
override CFLAGS += -std=gnu17 -Wall -Wextra
LDFLAGS += -Wl,-z,relro,-z,now

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

all: worm power

OBJS = worm.o fw.o panic.o status.o anon.o

worm: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(OBJS): worm.h

power: power.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ power.c

install: worm power
	install -Dm755 worm $(DESTDIR)$(BINDIR)/worm
	install -Dm4755 power $(DESTDIR)$(PREFIX)/lib/worm/power

clean:
	rm -f worm power $(OBJS)

.PHONY: all install clean
