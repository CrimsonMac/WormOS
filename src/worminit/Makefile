CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=gnu17 -Wall -Wextra -D_FORTIFY_SOURCE=3 -fstack-protector-strong
LDFLAGS += -Wl,-z,relro,-z,now

PREFIX  ?= /usr
SBINDIR ?= $(PREFIX)/sbin

all: worminit

worminit: worminit.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: worminit
	install -Dm755 worminit $(DESTDIR)$(SBINDIR)/worminit

clean:
	rm -f worminit

.PHONY: all install clean
