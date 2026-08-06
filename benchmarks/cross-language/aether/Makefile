AETHER_COMPILER = ../../../build/aetherc
AETHER_CLI      = ../../../build/ae
CC = gcc

# Include set from `ae cflags`, not a hand-written list, for the same reason
# the source list below reads the MANIFEST: a hand-maintained copy goes stale
# silently. It already did. When the public embedder header moved onto the
# include path (#1420), `runtime/libaether_caps.c` started including
# libaether.h by name and this Makefile's frozen -I list did not have it, so
# every Aether entry in the suite turned into "[SKIP] Build failed" while all
# ten other languages kept reporting numbers.
#
# `ae cflags` is the toolchain's own answer to "what -I do I need", so this
# cannot drift again.
AETHER_INCLUDES := $(shell $(AETHER_CLI) cflags --cflags 2>/dev/null)
CFLAGS = -O3 -march=native -flto -fno-plt $(AETHER_INCLUDES)
LDFLAGS = -flto -pthread -lm
# Runtime sources, taken from the build MANIFEST (regenerated on every
# `make stdlib`, the same set linked into libaether.a) so this list never
# drifts as the runtime grows. A hand-maintained subset went stale and the
# link broke on a newly-emitted symbol (`aether_capsicum_autosandbox`), which
# surfaced only as "Aether [SKIP] Build failed" in the suite. Compiled from
# source (not libaether.a) so the runtime gets the same `-O3 -march=native
# -flto` as the benchmark, for a fair comparison. Only `runtime/` entries are
# needed — these actor microbenchmarks don't touch std.
MANIFEST := ../../../build/MANIFEST
RUNTIME_SRCS := $(addprefix ../../../,$(shell grep '^runtime/' $(MANIFEST) 2>/dev/null))

.PHONY: all clean ping_pong counting thread_ring fork_join skynet

all: ping_pong counting thread_ring fork_join skynet

ping_pong: ping_pong.ae
	@echo "Compiling Aether ping_pong..."
	@$(AETHER_COMPILER) ping_pong.ae /tmp/ping_pong_aether.c
	@$(CC) $(CFLAGS) /tmp/ping_pong_aether.c $(RUNTIME_SRCS) -o ping_pong $(LDFLAGS)
	@echo "Done: ./ping_pong"

counting: counting.ae
	@echo "Compiling Aether counting..."
	@$(AETHER_COMPILER) counting.ae /tmp/counting_aether.c
	@$(CC) $(CFLAGS) /tmp/counting_aether.c $(RUNTIME_SRCS) -o counting $(LDFLAGS)
	@echo "Done: ./counting"

thread_ring: thread_ring.ae
	@echo "Compiling Aether thread_ring..."
	@$(AETHER_COMPILER) thread_ring.ae /tmp/thread_ring_aether.c
	@$(CC) $(CFLAGS) /tmp/thread_ring_aether.c $(RUNTIME_SRCS) -o thread_ring $(LDFLAGS)
	@echo "Done: ./thread_ring"

fork_join: fork_join.ae
	@echo "Compiling Aether fork_join..."
	@$(AETHER_COMPILER) fork_join.ae /tmp/fork_join_aether.c
	@$(CC) $(CFLAGS) /tmp/fork_join_aether.c $(RUNTIME_SRCS) -o fork_join $(LDFLAGS)
	@echo "Done: ./fork_join"

skynet: skynet.ae
	@echo "Compiling Aether skynet..."
	@$(AETHER_COMPILER) skynet.ae /tmp/skynet_aether.c
	@$(CC) $(CFLAGS) /tmp/skynet_aether.c $(RUNTIME_SRCS) -o skynet $(LDFLAGS)
	@echo "Done: ./skynet"

clean:
	rm -f ping_pong counting thread_ring fork_join skynet /tmp/*_aether.c*
