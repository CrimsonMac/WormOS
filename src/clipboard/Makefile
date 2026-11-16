CC       ?= cc
CFLAGS   ?= -O2
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
override CFLAGS += -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -I. $(shell $(PKG_CONFIG) --cflags wayland-client)
LDFLAGS  += -Wl,-z,relro,-z,now
LIBS      = $(shell $(PKG_CONFIG) --libs wayland-client)

PREFIX   ?= /usr
BINDIR   ?= $(PREFIX)/bin

PROTOCOLS = ext-data-control-v1 wlr-virtual-pointer-unstable-v1
HEADERS   = $(PROTOCOLS:%=%-client-protocol.h)
PROTO_SRC = $(PROTOCOLS:%=%-protocol.c)
SRCS      = clip.c agent.c
OBJS      = $(SRCS:.c=.o) $(PROTO_SRC:.c=.o)

all: clip

%-client-protocol.h: protocol/%.xml
	$(WAYLAND_SCANNER) client-header $< $@

%-protocol.c: protocol/%.xml
	$(WAYLAND_SCANNER) private-code $< $@

$(SRCS:.c=.o): clip.h $(HEADERS)

clip: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

install: clip
	install -Dm755 clip $(DESTDIR)$(BINDIR)/clip

clean:
	rm -f clip *.o $(HEADERS) $(PROTO_SRC)

.PHONY: all install clean
