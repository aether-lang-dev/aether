#!/bin/sh
# contrib/host/rhombus end-to-end smoke test (STATIC-link model).
#
# Rhombus runs on the same embedded Racket CS VM as contrib.host.racket and
# uses the SAME bridge source (Rhombus is a #lang on the Racket runtime). Same
# Racket env-var gate as host_racket, PLUS it needs the `rhombus` package
# installed in that Racket (raco pkg install rhombus). If Rhombus isn't
# installed, the driver's first eval comes back as an "error:" string; we
# detect that and SKIP rather than FAIL.
#
#   AETHER_RACKET_INCLUDE  - dir with chezscheme.h + racketcs.h
#   AETHER_RACKET_LIB      - path to libracketcs.a
#   AETHER_RACKET_BOOT_DIR - dir with petite.boot / scheme.boot / racket.boot
#   AETHER_RACKET_COLLECTS - racket collects dir (must reach the rhombus pkg)
#
# Linux/macOS only — host bridges aren't built on the Windows matrix.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s)" in
    Linux|Darwin) ;;
    *) echo "  [SKIP] host_rhombus: not Linux/Darwin ($(uname -s))"; exit 0 ;;
esac

if [ -z "$AETHER_RACKET_INCLUDE" ] || [ -z "$AETHER_RACKET_LIB" ] || \
   [ -z "$AETHER_RACKET_BOOT_DIR" ]; then
    echo "  [SKIP] AETHER_RACKET_INCLUDE / _LIB / _BOOT_DIR unset"
    echo "         (needs a built Racket CS with the rhombus package)"
    exit 0
fi
if [ ! -f "$AETHER_RACKET_INCLUDE/chezscheme.h" ] || \
   [ ! -f "$AETHER_RACKET_INCLUDE/racketcs.h" ]; then
    echo "  [SKIP] AETHER_RACKET_INCLUDE=$AETHER_RACKET_INCLUDE missing chezscheme.h/racketcs.h"
    exit 0
fi
if [ ! -f "$AETHER_RACKET_LIB" ]; then
    echo "  [SKIP] AETHER_RACKET_LIB=$AETHER_RACKET_LIB not found"; exit 0
fi
if [ ! -x "$AE" ]; then echo "  [SKIP] host_rhombus: $AE not built"; exit 0; fi

CC="${CC:-cc}"
TMPDIR_T="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_T"' EXIT

GEN_C="$TMPDIR_T/uses_rhombus.c"
if ! AETHER_HOME="$ROOT" "$ROOT/build/aetherc" "$SCRIPT_DIR/uses_rhombus.ae" "$GEN_C" \
        >"$TMPDIR_T/aetherc.log" 2>&1; then
    echo "  [FAIL] host_rhombus: aetherc failed"; cat "$TMPDIR_T/aetherc.log"; exit 1
fi
if ! $CC -c -O2 -DAETHER_HAS_RACKET -DAETHER_HAS_SANDBOX \
        -I"$ROOT" -I"$AETHER_RACKET_INCLUDE" \
        "$ROOT/contrib/host/racket/aether_host_racket.c" \
        -o "$TMPDIR_T/host_racket.o" 2>"$TMPDIR_T/bridge.log"; then
    echo "  [FAIL] host_rhombus: bridge compile failed"; head -15 "$TMPDIR_T/bridge.log"; exit 1
fi
INCS=""
for d in $(find "$ROOT/runtime" "$ROOT/std" -type d 2>/dev/null); do
    INCS="$INCS -I$d"
done
RKLIBS="-lm -ldl -lpthread -lrt -lncurses -lz"
[ "$(uname -s)" = "Darwin" ] && RKLIBS="-lm -ldl -lpthread -lncurses -lz"
# Distro-packaged Racket CS 9.x references the system liblz4 (see the
# identical probe in test_host_racket.sh for the full story).
if command -v nm >/dev/null 2>&1 && \
        nm "$AETHER_RACKET_LIB" 2>/dev/null | grep -q ' U LZ4'; then
    RKLIBS="$RKLIBS -llz4"
fi
if ! $CC -O2 -I"$ROOT" $INCS -I"$AETHER_RACKET_INCLUDE" \
        "$GEN_C" "$TMPDIR_T/host_racket.o" "$ROOT/build/libaether.a" \
        "$AETHER_RACKET_LIB" -rdynamic $RKLIBS \
        -o "$TMPDIR_T/uses_rhombus" 2>"$TMPDIR_T/link.log"; then
    echo "  [FAIL] host_rhombus: link failed"; head -20 "$TMPDIR_T/link.log"; exit 1
fi

ACTUAL="$TMPDIR_T/actual.txt"
if ! AETHER_RACKET_BOOT_DIR="$AETHER_RACKET_BOOT_DIR" \
     AETHER_RACKET_COLLECTS="${AETHER_RACKET_COLLECTS:-}" \
     AETHER_RACKET_CONFIG="${AETHER_RACKET_CONFIG:-}" \
     "$TMPDIR_T/uses_rhombus" >"$ACTUAL" 2>"$TMPDIR_T/err.log"; then
    echo "  [FAIL] host_rhombus: program exited non-zero"
    head -20 "$TMPDIR_T/err.log"; head -10 "$ACTUAL"; exit 1
fi

# Rhombus package not installed: the fib eval returns an "error:" mentioning
# the missing #lang / collection. Treat as SKIP, not FAIL.
if grep -qiE "collection not found|cannot load|standard-module-name|rhombus" "$ACTUAL" && \
   ! grep -Fxq "PASS: rhombus host fib + error contract + cross-VM k-v" "$ACTUAL"; then
    echo "  [SKIP] host_rhombus: rhombus package not installed in the embedded Racket"
    echo "         (raco pkg install rhombus, then ensure AETHER_RACKET_COLLECTS covers it)"
    exit 0
fi

if grep -Fxq "PASS: rhombus host fib + error contract + cross-VM k-v" "$ACTUAL"; then
    echo "  [PASS] contrib.host.rhombus: fib + error contract + cross-VM k-v"
else
    echo "  [FAIL] host_rhombus: driver did not reach PASS"
    echo "--- actual ---"; cat "$ACTUAL"; exit 1
fi
