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

# A value of the Makefile's own, for a build with EXTRA_CFLAGS=$2. Run under
# `make` (the sweep is), the inner make inherits MAKEFLAGS and prints
# "Entering directory" lines into what it echoes, so it starts clean.
make_var() {
    printf 'ae-print-var:\n\t@echo $(%s)\n' "$1" |
        MAKEFLAGS= MAKELEVEL= ${MAKE:-make} --no-print-directory -s -f Makefile -f - ae-print-var \
            EXTRA_CFLAGS="$2"
}
# The sources each build compiles: a no-threads one has the cooperative
# scheduler in place of the threaded one.
srcs_of() {
    echo "$(make_var RUNTIME_SRC "$1") $(make_var STD_SRC "$1") $(make_var STD_REACTOR_SRC "$1") $(make_var COLLECTIONS_SRC "$1")"
}
CFLAGS_REQ="$(make_var AETHER_REQUIRED_CFLAGS "")"
if [ -z "$(srcs_of "")" ] || [ -z "$CFLAGS_REQ" ]; then
    echo "  [FAIL] no_threads_refs: could not read the libaether sources from the Makefile"
    exit 1
fi

# $1 = mode (threads | coop), $2 = extra flag
compile_all() {
    mkdir -p "$TMPDIR/$1"
    for src in $(srcs_of "$2"); do echo "$src"; done |
        xargs -P 4 -I{} sh -c 'o="$0/$(echo "$1" | tr / _).o"; $2 -c -O0 $3 $4 "$1" -o "$o" 2>>"$0.err" || echo "$1" >>"$0.failed"' \
            "$TMPDIR/$1" {} "$CC" "$2" "$CFLAGS_REQ"
    if [ -s "$TMPDIR/$1.failed" ]; then
        echo "  [FAIL] no_threads_refs: $1 build did not compile:"
        sed 's/^/        /' "$TMPDIR/$1.failed" | head -5
        grep -m 5 'error' "$TMPDIR/$1.err" | sed 's/^/        /'
        exit 1
    fi
}

# Defined (D) and undefined (U) external symbols of the objects in $1.
symbols() {
    for o in "$TMPDIR/$1"/*.o; do nm -g "$o"; done |
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
