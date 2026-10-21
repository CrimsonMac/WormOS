CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
override CFLAGS += -std=gnu17 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags gtk4)
LDFLAGS  += -Wl,-z,relro,-z,now
LIBS      = $(shell $(PKG_CONFIG) --libs gtk4)

PREFIX   ?= /usr
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share

all: wormos-snapshots

wormos-snapshots: snapshots.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ snapshots.c $(LIBS)

install: wormos-snapshots
	install -Dm755 wormos-snapshots $(DESTDIR)$(BINDIR)/wormos-snapshots
	install -Dm644 snapshots.css $(DESTDIR)$(DATADIR)/wormos/snapshots/snapshots.css
	install -Dm644 org.wormos.Snapshots.desktop \
		$(DESTDIR)$(DATADIR)/applications/org.wormos.Snapshots.desktop
	install -Dm644 org.wormos.Snapshots.svg \
		$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/org.wormos.Snapshots.svg

clean:
	rm -f wormos-snapshots

.PHONY: all install clean
