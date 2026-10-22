CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
override CFLAGS += -std=gnu17 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags gtk4)
LDFLAGS  += -Wl,-z,relro,-z,now
LIBS      = $(shell $(PKG_CONFIG) --libs gtk4)

PREFIX   ?= /usr
BINDIR   ?= $(PREFIX)/bin
DATADIR  ?= $(PREFIX)/share

all: tor-browser

tor-browser: torbrowser.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ torbrowser.c $(LIBS)

install: tor-browser
	install -Dm755 tor-browser $(DESTDIR)$(BINDIR)/tor-browser
	install -Dm644 tor-browser.css $(DESTDIR)$(DATADIR)/wormos/tor-browser/tor-browser.css
	install -Dm644 org.wormos.TorBrowser.desktop \
		$(DESTDIR)$(DATADIR)/applications/org.wormos.TorBrowser.desktop
	install -Dm644 org.wormos.TorBrowser.svg \
		$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/org.wormos.TorBrowser.svg

clean:
	rm -f tor-browser

.PHONY: all install clean
