CC      ?= cc
CFLAGS  ?= -O2
override CFLAGS += -std=gnu17 -Wall -Wextra -I../common
LDFLAGS += -Wl,-z,relro,-z,now

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

OBJS = bh.o box.o relay.o sandbox.o ../common/ns.o

all: bh

bh: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(OBJS): bh.h ../common/ns.h

install: bh
	install -Dm755 bh $(DESTDIR)$(BINDIR)/bh
	ln -sf bh $(DESTDIR)$(BINDIR)/blackhole

clean:
	rm -f bh $(OBJS)

.PHONY: all install clean
