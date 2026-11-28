# Fuzz targets for the parsers that read untrusted or root-only input
# (libFuzzer, clang): make -C tests/fuzz [run SECONDS=60]
CC       = clang
SAN      = -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all
override CFLAGS += -std=gnu17 -g -O1 $(SAN) -I../../src/common -I../../src/snap -I../../src/wh
MONO_CFLAGS ?=
MONO_LIBS   ?= -lmonocypher
SECONDS ?= 60
TARGETS = relay index seal manifest moat
WRAP = -Wl,--wrap=die

all: $(TARGETS)

relay: relay.c ../../src/bh/relay.c
	$(CC) $(CFLAGS) -o $@ relay.c

index: index.c common.h ../../src/wh/repo.c ../../src/wh/pkg.c
	$(CC) $(CFLAGS) $(MONO_CFLAGS) $(WRAP) -o $@ index.c ../../src/wh/repo.c \
		../../src/wh/pkg.c ../../src/wh/util.c ../../src/wh/sig.c ../../src/wh/db.c \
		../../src/snap/sha256.c $(MONO_LIBS)

seal: seal.c common.h ../../src/snap/seal.c
	$(CC) $(CFLAGS) $(MONO_CFLAGS) $(WRAP) -o $@ seal.c ../../src/snap/util.c \
		../../src/snap/sha256.c $(MONO_LIBS)

manifest: manifest.c common.h ../../src/snap/manifest.c
	$(CC) $(CFLAGS) $(WRAP) -o $@ manifest.c ../../src/snap/manifest.c \
		../../src/snap/util.c ../../src/snap/sha256.c

moat: moat.c common.h ../../src/worm/anon.c
	$(CC) $(CFLAGS) -o $@ moat.c

# Each target for SECONDS, starting from its seeds in corpus/NAME; new
# finds go to a scratch copy (work/NAME), never into the repository.
run: $(TARGETS)
	@status=0; for t in $(TARGETS); do \
		mkdir -p work/$$t && cp corpus/$$t/* work/$$t/ 2>/dev/null; \
		echo "== $$t"; \
		./$$t -max_total_time=$(SECONDS) -timeout=10 -rss_limit_mb=2048 \
			-print_final_stats=1 work/$$t > $$t.log 2>&1 || { status=1; tail -40 $$t.log; }; \
		grep -E 'stat::number_of_executed_units|DONE' $$t.log | tail -2; \
	done; exit $$status

clean:
	rm -f $(TARGETS) *.log crash-* leak-* timeout-* oom-*
	rm -rf work

.PHONY: all run clean
