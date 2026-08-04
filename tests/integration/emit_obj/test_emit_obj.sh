#!/bin/sh
# Regression (#1243): `ae build --emit=obj` compiles an .ae straight to an object
# a C build can link, so the generated C never needs to be checked in. Committing
# it invites a stale link: edit the .ae, restore the .c to keep the diff clean,
# and a plain `make` builds the old code.
#
# Asserts the object is real and linkable, not merely that the command exits 0.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] emit_obj: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

if ! "$AE" build "$SCRIPT_DIR/lib.ae" --emit=obj -o "$TMPDIR/lib.o" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] emit_obj: --emit=obj did not build"
    sed 's/^/        /' "$TMPDIR/build.log" | head -10
    exit 1
fi

if [ ! -s "$TMPDIR/lib.o" ]; then
    echo "  [FAIL] emit_obj: no object produced"
    exit 1
fi

# The Aether functions must be linkable symbols in it.
for sym in add greet; do
    if ! nm "$TMPDIR/lib.o" 2>/dev/null | grep -qE "T _?$sym$"; then
        echo "  [FAIL] emit_obj: object does not export '$sym'"
        nm "$TMPDIR/lib.o" 2>/dev/null | head -8 | sed 's/^/        /'
        exit 1
    fi
done

# The point of the mode: link it into a C program and run it.
#
# The runtime comes from the in-tree archive by path, not from `-laether`:
# `ae cflags --libs` emits `-L<prefix>/lib -laether` for an *installed*
# runtime, and CI never runs `make install`, so that spelling fails on
# `cannot find -laether` even when the object under test is perfect.
#
# Every other flag from `ae cflags --libs` is kept: it is the right source of
# truth for the archive's transitive dependencies (OpenSSL, PCRE2, nghttp2, and
# the platform sockets library on Windows), and the `-L` paths that go with
# them are load-bearing, dropping those breaks `-lssl` on a Homebrew host.
# pthread is needed because libaether uses TLS and the actor scheduler; -lm
# covers math.h symbols pulled in by the stdlib.
LIB="$ROOT/build/libaether.a"
if [ ! -f "$LIB" ]; then
    echo "  [SKIP] emit_obj: libaether.a not built"
    exit 0
fi
DEPS=$("$AE" cflags --libs 2>/dev/null | tr ' ' '\n' \
       | grep -v '^-laether$' | tr '\n' ' ')
if ! sh -c "cc -o \"$TMPDIR/consume\" \"$SCRIPT_DIR/consume.c\" \"$TMPDIR/lib.o\" \"$LIB\" $DEPS -lpthread -lm" \
        >"$TMPDIR/link.log" 2>&1; then
    echo "  [FAIL] emit_obj: C consumer failed to link against the object"
    grep -v "^ld: warning" "$TMPDIR/link.log" | head -8 | sed 's/^/        /'
    exit 1
fi

out=$("$TMPDIR/consume" 2>&1)
if [ "$out" != "ok" ]; then
    echo "  [FAIL] emit_obj: C consumer printed '$out', want 'ok'"
    exit 1
fi

echo "  [PASS] emit_obj: .ae compiles to a linkable object a C program can call"
