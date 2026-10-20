CC      ?= cc
CFLAGS  ?= -O2
override CFLAGS += -std=gnu17 -Wall -Wextra
LDFLAGS += -Wl,-z,relro,-z,now

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

all: worm

OBJS = worm.o fw.o

worm: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(OBJS): worm.h

install: worm
	install -Dm755 worm $(DESTDIR)$(BINDIR)/worm

clean:
	rm -f worm $(OBJS)

.PHONY: all install clean
