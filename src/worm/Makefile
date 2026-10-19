CC      ?= cc
CFLAGS  ?= -O2
override CFLAGS += -std=gnu17 -Wall -Wextra
LDFLAGS += -Wl,-z,relro,-z,now

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

all: worm

worm: worm.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: worm
	install -Dm755 worm $(DESTDIR)$(BINDIR)/worm

clean:
	rm -f worm

.PHONY: all install clean
