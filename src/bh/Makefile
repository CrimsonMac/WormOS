CC      ?= cc
CFLAGS  ?= -O2
override CFLAGS += -std=gnu17 -Wall -Wextra -I../common
LDFLAGS += -Wl,-z,relro,-z,now

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin
LIBDIR  ?= $(PREFIX)/lib/bh

OBJS = bh.o box.o session.o relay.o sandbox.o limits.o net.o vm.o ../common/ns.o

all: bh limits vm-init

bh: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

# Setuid root: sets up the cgroups of users' blackholes.
limits: limits-helper.o limits.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ limits-helper.o limits.o

# The guest's init in --vm blackholes: static, for any /usr.
vm-init: vm-init.c
	$(CC) $(CFLAGS) -static -o $@ vm-init.c

$(OBJS) limits-helper.o: bh.h ../common/ns.h

install: all
	install -Dm755 bh $(DESTDIR)$(BINDIR)/bh
	ln -sf bh $(DESTDIR)$(BINDIR)/blackhole
	install -Dm4755 limits $(DESTDIR)$(LIBDIR)/limits
	install -Dm755 vm-init $(DESTDIR)$(LIBDIR)/vm-init

clean:
	rm -f bh limits vm-init $(OBJS) limits-helper.o

.PHONY: all install clean
