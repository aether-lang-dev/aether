#!/bin/sh
# Regression (#3121): a no-threads libaether references nothing that only a
# threaded one defines, whatever the optimizer keeps.
#
# std/net/aether_http_server.c compiled `http_park_resume` in every build,
# and it calls `http_pool_submit_conn`, which aether_http_pool.c defines only
# with threads. Its one caller is threaded, so at -O2 GCC dropped the unused
# static and its reference with it; at -O0 GCC keeps both, and a no-threads
# stdlib's shared runtime did not link on Windows (an unresolved symbol in a
# Linux .so). Each libaether source, the Makefile's own list and flags, is
# compiled at -O0 with and without AETHER_NO_THREADING, and no symbol a
# no-threads object needs may be one only the threaded objects define. Clang
# drops an unused static at any level, so under clang this passes either
# way; the GCC legs are the ones it guards.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1
CC="${CC:-cc}"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# Each build is one `make` run over the Makefile's own variables -- its
# lists of libaether sources and its required flags, the no-threads one with
# the cooperative scheduler in place of the threaded one -- with a rule per
# source compiling it at -O0 into $TMPDIR/<mode>/. One run, not a `make` per
# variable and a shell per source: on Windows (MSYS2) each process start
# costs tens of milliseconds and each parse of the Makefile seconds, and that
# is where the old way spent most of its five-plus minutes there, past the
# sweep's timeout. Run under `make` (the sweep is), the inner make inherits
# MAKEFLAGS and prints "Entering directory" lines, so it starts clean.
#
# $1 = mode (threads | coop), $2 = extra flag
compile_all() {
    out="$TMPDIR/$1"
    mkdir -p "$out"
    printf '%b\n' \
        'NTR_SRCS := $(RUNTIME_SRC) $(STD_SRC) $(STD_REACTOR_SRC) $(COLLECTIONS_SRC)' \
        '$(if $(strip $(NTR_SRCS)),,$(error no libaether sources in the Makefile))' \
        '$(if $(strip $(AETHER_REQUIRED_CFLAGS)),,$(error no AETHER_REQUIRED_CFLAGS in the Makefile))' \
        'ntr_obj = $(NTR_OUT)/$(subst /,_,$(1:.c=.o))' \
        'define ntr_rule' \
        '$(call ntr_obj,$(1)): $(1)' \
        '\t$(NTR_CC) -c -O0 -pipe $(filter-out -MMD -MP,$(AETHER_REQUIRED_CFLAGS)) $(NTR_FLAG) $(1) -o $(call ntr_obj,$(1))' \
        'endef' \
        '$(foreach s,$(NTR_SRCS),$(eval $(call ntr_rule,$(s))))' \
        'ntr-all: $(foreach s,$(NTR_SRCS),$(call ntr_obj,$(s)))' |
        MAKEFLAGS= MAKELEVEL= ${MAKE:-make} --no-print-directory -s -k -j4 -f Makefile -f - ntr-all \
            NTR_CC="$CC" EXTRA_CFLAGS="$2" NTR_FLAG="$2" NTR_OUT="$out" >"$TMPDIR/$1.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "  [FAIL] no_threads_refs: $1 build did not compile:"
        grep -m 8 -i 'error' "$TMPDIR/$1.log" | sed 's/^/        /'
        exit 1
    fi
}

# Defined (D) and undefined (U) external symbols of the objects in $1, from
# one nm over them all (its "file.o:" headers are one field, so skipped).
symbols() {
    nm -g "$TMPDIR/$1"/*.o |
        awk '$1 == "U" { print "U " $2; next } NF == 3 && $2 != "U" { print "D " $3 }' | sort -u
}

compile_all threads ""
compile_all coop "-DAETHER_NO_THREADING"

symbols threads | sed -n 's/^D //p' | sort -u > "$TMPDIR/threads.defined"
symbols coop >"$TMPDIR/coop.syms"
sed -n 's/^D //p' "$TMPDIR/coop.syms" | sort -u > "$TMPDIR/coop.defined"
sed -n 's/^U //p' "$TMPDIR/coop.syms" | sort -u > "$TMPDIR/coop.needed"
comm -23 "$TMPDIR/threads.defined" "$TMPDIR/coop.defined" > "$TMPDIR/threads.only"
comm -12 "$TMPDIR/coop.needed" "$TMPDIR/threads.only" > "$TMPDIR/dangling"

if [ -s "$TMPDIR/dangling" ]; then
    echo "  [FAIL] no_threads_refs: a no-threads build references symbols only a threaded build defines:"
    sed 's/^/        /' "$TMPDIR/dangling"
    exit 1
fi
echo "  [PASS] no_threads_refs: a no-threads libaether at -O0 needs nothing only threads define"
