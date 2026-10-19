CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=gnu17 -Wall -Wextra -D_FORTIFY_SOURCE=3 -fstack-protector-strong
LDFLAGS += -Wl,-z,relro,-z,now

PREFIX  ?= /usr
LIBDIR  ?= $(PREFIX)/lib/wormrd

all: wormrd

wormrd: wormrd.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: wormrd
	install -Dm755 wormrd $(DESTDIR)$(LIBDIR)/init

clean:
	rm -f wormrd

.PHONY: all install clean
