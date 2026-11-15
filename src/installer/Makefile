CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
override CFLAGS += -std=gnu17 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags gtk4)
LDFLAGS  += -Wl,-z,relro,-z,now
LIBS      = $(shell $(PKG_CONFIG) --libs gtk4)

PREFIX   ?= /usr
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share

all: wormos-installer

wormos-installer: installer.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ installer.c $(LIBS)

install: wormos-installer
	install -Dm755 wormos-installer $(DESTDIR)$(BINDIR)/wormos-installer
	install -Dm644 installer.css $(DESTDIR)$(DATADIR)/wormos/installer/installer.css
	install -Dm644 org.wormos.Installer.desktop \
		$(DESTDIR)$(DATADIR)/applications/org.wormos.Installer.desktop
	install -Dm644 org.wormos.Installer.svg \
		$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/org.wormos.Installer.svg

clean:
	rm -f wormos-installer

.PHONY: all install clean
