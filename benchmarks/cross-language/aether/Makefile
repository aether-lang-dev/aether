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
OPT = -O3 -march=native -flto -fno-plt
CFLAGS = $(OPT) $(AETHER_INCLUDES)
# The generated C calls std as well as the runtime (string_retain and
# string_release for string values, aether_write_bytes for println). std is
# taken from libaether.a, built with the defines its sources need (PCRE2,
# OpenSSL, zlib), with the archive's own runtime members taken out: the
# runtime is the one compiled below, and only it. `ae cflags --libs` gives the
# system libraries, less libaether itself.
#
# With the whole archive on the line the runtime was defined twice on ELF:
# GCC's LTO plugin does not list what top-level asm defines, and the
# rendezvous records are such asm (runtime/aether_rv_mark.h), so the link
# took the archive's aether_resource_caps.o for its anchor, beside the
# compiled one, and every symbol in it was then defined twice.
STD_LIB := ../build/aether-std/libaether_std.a
AETHER_LIBS := $(filter-out -laether,$(shell $(AETHER_CLI) cflags --libs 2>/dev/null))
LDFLAGS = -flto -pthread -lm $(STD_LIB) $(AETHER_LIBS)
# Runtime sources, taken from the build MANIFEST (regenerated on every
# `make stdlib`, the same set linked into libaether.a) so this list never
# drifts as the runtime grows. A hand-maintained subset went stale and the
# link broke on a newly-emitted symbol (`aether_capsicum_autosandbox`), which
# surfaced only as "Aether [SKIP] Build failed" in the suite. Compiled from
# source (not libaether.a) so the runtime gets the same `-O3 -march=native
# -flto` as the benchmark, for a fair comparison; std comes from the archive
# above.
MANIFEST := ../../../build/MANIFEST
RUNTIME_SRCS := $(addprefix ../../../,$(shell grep '^runtime/' $(MANIFEST) 2>/dev/null))

.PHONY: all clean flags ping_pong counting thread_ring fork_join skynet

all: ping_pong counting thread_ring fork_join skynet

$(STD_LIB): ../../../build/libaether.a $(MANIFEST)
	@mkdir -p $(dir $@)
	@cp ../../../build/libaether.a $@
	@ar d $@ $(notdir $(RUNTIME_SRCS:.c=.o))

ping_pong: ping_pong.ae $(STD_LIB)
	@echo "Compiling Aether ping_pong..."
	@$(AETHER_COMPILER) ping_pong.ae /tmp/ping_pong_aether.c
	@$(CC) $(CFLAGS) /tmp/ping_pong_aether.c $(RUNTIME_SRCS) -o ping_pong $(LDFLAGS)
	@echo "Done: ./ping_pong"

counting: counting.ae $(STD_LIB)
	@echo "Compiling Aether counting..."
	@$(AETHER_COMPILER) counting.ae /tmp/counting_aether.c
	@$(CC) $(CFLAGS) /tmp/counting_aether.c $(RUNTIME_SRCS) -o counting $(LDFLAGS)
	@echo "Done: ./counting"

thread_ring: thread_ring.ae $(STD_LIB)
	@echo "Compiling Aether thread_ring..."
	@$(AETHER_COMPILER) thread_ring.ae /tmp/thread_ring_aether.c
	@$(CC) $(CFLAGS) /tmp/thread_ring_aether.c $(RUNTIME_SRCS) -o thread_ring $(LDFLAGS)
	@echo "Done: ./thread_ring"

fork_join: fork_join.ae $(STD_LIB)
	@echo "Compiling Aether fork_join..."
	@$(AETHER_COMPILER) fork_join.ae /tmp/fork_join_aether.c
	@$(CC) $(CFLAGS) /tmp/fork_join_aether.c $(RUNTIME_SRCS) -o fork_join $(LDFLAGS)
	@echo "Done: ./fork_join"

skynet: skynet.ae $(STD_LIB)
	@echo "Compiling Aether skynet..."
	@$(AETHER_COMPILER) skynet.ae /tmp/skynet_aether.c
	@$(CC) $(CFLAGS) /tmp/skynet_aether.c $(RUNTIME_SRCS) -o skynet $(LDFLAGS)
	@echo "Done: ./skynet"

clean:
	rm -rf ping_pong counting thread_ring fork_join skynet /tmp/*_aether.c* $(dir $(STD_LIB))

# What the runner records with the results (run_benchmarks.ae).
flags:
	@echo "$(CC) $(OPT) / -flto (includes and libraries from ae cflags)"
