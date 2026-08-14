#!/bin/sh
# contrib/host/racket end-to-end smoke test (STATIC-link model).
#
# Racket CS has no shared libracketcs to dlopen — embedding is by static-linking
# the libracketcs.a from a built Racket CS tree, plus its boot images. So this
# test gates on the embedding pieces being present and pointed at by env:
#   AETHER_RACKET_INCLUDE  - dir with chezscheme.h + racketcs.h (compile-time)
#   AETHER_RACKET_LIB      - path to libracketcs.a (static-linked)
#   AETHER_RACKET_BOOT_DIR - dir with petite.boot / scheme.boot / racket.boot
#   AETHER_RACKET_COLLECTS - racket collects dir (runtime; for `racket`)
# CI machines won't have these, so the test no-ops there (host bridges must
# never break CI on machines without the runtime). When set, it builds the
# racket bridge object, links it + libracketcs.a into uses_racket.ae (compiled
# to C by `ae`), and runs it.
#
# Linux/macOS only — host bridges aren't built on the Windows matrix.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s)" in
    Linux|Darwin) ;;
    *) echo "  [SKIP] host_racket: not Linux/Darwin ($(uname -s))"; exit 0 ;;
esac

if [ -z "$AETHER_RACKET_INCLUDE" ] || [ -z "$AETHER_RACKET_LIB" ] || \
   [ -z "$AETHER_RACKET_BOOT_DIR" ]; then
    echo "  [SKIP] AETHER_RACKET_INCLUDE / _LIB / _BOOT_DIR unset"
    echo "         (needs a built Racket CS: \`make cs\` in a racket checkout,"
    echo "          then point these at its headers / libracketcs.a / *.boot dir)"
    exit 0
fi
if [ ! -f "$AETHER_RACKET_INCLUDE/chezscheme.h" ] || \
   [ ! -f "$AETHER_RACKET_INCLUDE/racketcs.h" ]; then
    echo "  [SKIP] AETHER_RACKET_INCLUDE=$AETHER_RACKET_INCLUDE missing chezscheme.h/racketcs.h"
    exit 0
fi
if [ ! -f "$AETHER_RACKET_LIB" ]; then
    echo "  [SKIP] AETHER_RACKET_LIB=$AETHER_RACKET_LIB not found"
    exit 0
fi
if [ ! -x "$AE" ]; then echo "  [SKIP] host_racket: $AE not built"; exit 0; fi

CC="${CC:-cc}"
TMPDIR_T="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_T"' EXIT

# 1. Compile uses_racket.ae -> C via aetherc (no link; we link by hand with
#    libracketcs.a, which `ae build`'s generic path doesn't yet add).
GEN_C="$TMPDIR_T/uses_racket.c"
if ! AETHER_HOME="$ROOT" "$ROOT/build/aetherc" "$SCRIPT_DIR/uses_racket.ae" "$GEN_C" \
        >"$TMPDIR_T/aetherc.log" 2>&1; then
    echo "  [FAIL] host_racket: aetherc failed"; cat "$TMPDIR_T/aetherc.log"; exit 1
fi

# 2. Build the racket bridge object (real mode).
if ! $CC -c -O2 -DAETHER_HAS_RACKET -DAETHER_HAS_SANDBOX \
        -I"$ROOT" -I"$AETHER_RACKET_INCLUDE" \
        "$ROOT/contrib/host/racket/aether_host_racket.c" \
        -o "$TMPDIR_T/host_racket.o" 2>"$TMPDIR_T/bridge.log"; then
    echo "  [FAIL] host_racket: bridge compile failed"; head -15 "$TMPDIR_T/bridge.log"; exit 1
fi

# 3. Link the program + bridge + runtime + libracketcs.a + Racket's system deps.
#    The generated C #includes runtime/std headers by bare name, so mirror
#    `ae build`'s include walk: one -I per dir under runtime/ and std/.
INCS=""
for d in $(find "$ROOT/runtime" "$ROOT/std" -type d 2>/dev/null); do
    INCS="$INCS -I$d"
done
RKLIBS="-lm -ldl -lpthread -lrt -lncurses -lz"
[ "$(uname -s)" = "Darwin" ] && RKLIBS="-lm -ldl -lpthread -lncurses -lz"
# Distro-packaged Racket CS 9.x (e.g. Arch racket 9.2) links ChezScheme's
# compress-io against the SYSTEM liblz4, leaving LZ4F_*/LZ4_* undefined in
# libracketcs.a; a self-built tree bundles lz4 and has none. Probe the
# archive and link what it actually needs, so both kinds keep working.
if command -v nm >/dev/null 2>&1 && \
        nm "$AETHER_RACKET_LIB" 2>/dev/null | grep -q ' U LZ4'; then
    RKLIBS="$RKLIBS -llz4"
fi
if ! $CC -O2 -I"$ROOT" $INCS -I"$AETHER_RACKET_INCLUDE" \
        "$GEN_C" "$TMPDIR_T/host_racket.o" "$ROOT/build/libaether.a" \
        "$AETHER_RACKET_LIB" -rdynamic $RKLIBS \
        -o "$TMPDIR_T/uses_racket" 2>"$TMPDIR_T/link.log"; then
    echo "  [FAIL] host_racket: link failed"; head -20 "$TMPDIR_T/link.log"; exit 1
fi

# 4. Run it.
ACTUAL="$TMPDIR_T/actual.txt"
if ! AETHER_RACKET_BOOT_DIR="$AETHER_RACKET_BOOT_DIR" \
     AETHER_RACKET_COLLECTS="${AETHER_RACKET_COLLECTS:-}" \
     AETHER_RACKET_CONFIG="${AETHER_RACKET_CONFIG:-}" \
     "$TMPDIR_T/uses_racket" >"$ACTUAL" 2>"$TMPDIR_T/err.log"; then
    echo "  [FAIL] host_racket: program exited non-zero"
    head -20 "$TMPDIR_T/err.log"; head -10 "$ACTUAL"; exit 1
fi

if grep -Fxq "PASS: racket host fib k-v map + eval round-trip" "$ACTUAL"; then
    echo "  [PASS] contrib.host.racket: fib + error contract + k-v round-trip"
else
    echo "  [FAIL] host_racket: driver did not reach PASS"
    echo "--- actual ---"; cat "$ACTUAL"; exit 1
fi
