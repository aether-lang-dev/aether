#!/bin/sh
# Regression: runtime/coop_runtime_sources.txt, the runtime the wasm and
# ESP32 (cooperative) builds link, is closed under std.bytes, std.mem,
# std.strbuilder and std.sync. Their C was not on the list, so a program
# importing them failed to link (aether_strbuilder_new, aether_bytes_data,
# aether_sync_atomic_new ... undefined).
#
# probe.ae is translated by aetherc and built on the host with the
# cooperative defines against ONLY the files the list names, then run.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
CC="${CC:-cc}"
NAME=coop_runtime_modules

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: $AE not built"; exit 0; }
[ -x "$AETHERC" ] || [ -x "$AETHERC.exe" ] || { echo "  [SKIP] $NAME: $AETHERC not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

# Include flags only (the same filter finalize_drop_panic uses); the
# runtime comes from the list, not from libaether.
FLAGS="$(AETHER_HOME="$ROOT" "$AE" cflags | tr -d '\r' | sed 's/-laether_main//')"
INCS="$(printf '%s\n' "$FLAGS" | tr ' ' '\n' | awk '
    /^-I/ || /^-f/ { print; next }
    /^-idirafter$/ { getline d; print "-idirafter " d; next }
    /^-idirafter/ { print }' | tr '\n' ' ')"
COOP="-DAETHER_NO_THREADING -DAETHER_NO_FILESYSTEM -DAETHER_NO_NETWORKING"
# Windows system libraries the runtime links against (the Makefile's
# WIN_LINK_LIBS, less -static); e.g. the TLS trust store needs crypt32.
SYSLIBS="-pthread -lm"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) SYSLIBS="$SYSLIBS -lws2_32 -lcrypt32 -lgdi32 -luser32 -ladvapi32 -lbcrypt -ldbghelp" ;;
esac

if ! AETHER_HOME="$ROOT" "$AETHERC" "$SCRIPT_DIR/probe.ae" "$TMP/probe.c" > "$TMP/aetherc.log" 2>&1; then
    echo "  [FAIL] $NAME: aetherc could not translate probe.ae"
    head -10 "$TMP/aetherc.log" | sed 's/^/        /'
    exit 1
fi

mkdir -p "$TMP/coop"
sed -e '/^#/d' -e '/^[[:space:]]*$/d' "$ROOT/runtime/coop_runtime_sources.txt" | tr -d '\r' |
    while read -r src; do
        if ! $CC -O1 $COOP $INCS -c "$ROOT/$src" -o "$TMP/coop/$(echo "$src" | tr / _).o" >> "$TMP/coop.log" 2>&1; then
            echo "$src" >> "$TMP/coop.failed"
        fi
    done
# shellcheck disable=SC2086
if [ -s "$TMP/coop.failed" ] ||
   ! $CC -O1 $COOP $INCS -c "$TMP/probe.c" -o "$TMP/probe.o" >> "$TMP/coop.log" 2>&1 ||
   ! $CC "$TMP/probe.o" "$TMP"/coop/*.o -o "$TMP/probe" $SYSLIBS >> "$TMP/coop.log" 2>&1; then
    echo "  [FAIL] $NAME: the program does not build against runtime/coop_runtime_sources.txt alone"
    [ -s "$TMP/coop.failed" ] && sed 's/^/        did not compile: /' "$TMP/coop.failed"
    grep -m 12 -iE 'error|undefined|_aether_' "$TMP/coop.log" | sed 's/^/        /'
    exit 1
fi

got="$("$TMP/probe" 2>&1 | tr -d '\r')"
want="coop modules: n=42 sum=1016 cas=1 cell=11"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] $NAME: ran, but printed:"
    printf '%s\n' "$got" | head -6 | sed 's/^/        /'
    echo "      want: $want"
    exit 1
fi
echo "  [PASS] $NAME: a program importing std.bytes, std.mem, std.strbuilder and std.sync links against the cooperative runtime list alone, and runs"
