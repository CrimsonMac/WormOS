CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
override CFLAGS += -std=gnu17 -Wall -Wextra -I../common
LDFLAGS  += -Wl,-z,relro,-z,now
GTK_CFLAGS = $(shell $(PKG_CONFIG) --cflags gtk4)
GTK_LIBS   = $(shell $(PKG_CONFIG) --libs gtk4)

PREFIX   ?= /usr
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share
LIBDIR   ?= $(PREFIX)/lib

all: wormlogin wormos-greeter

wormlogin: wormlogin.c ../common/password.c ../common/password.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ wormlogin.c ../common/password.c -lcrypt

wormos-greeter: greeter.c
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(LDFLAGS) -o $@ greeter.c $(GTK_LIBS)

install: all
	install -Dm755 wormlogin $(DESTDIR)$(LIBDIR)/worm/wormlogin
	install -Dm755 greeter-session $(DESTDIR)$(LIBDIR)/worm/greeter-session
	install -Dm755 wormos-greeter $(DESTDIR)$(BINDIR)/wormos-greeter
	install -Dm644 greeter.css $(DESTDIR)$(DATADIR)/wormos/greeter/greeter.css
	install -Dm644 -t $(DESTDIR)$(DATADIR)/wormos/greeter/labwc \
		labwc/rc.xml labwc/menu.xml labwc/environment

clean:
	rm -f wormlogin wormos-greeter

.PHONY: all install clean
