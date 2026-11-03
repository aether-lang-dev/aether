#!/bin/sh
# -DAETHER_ACCT_FILTER_LOG2=<n> (6..24, default 18) sizes the execution
# accounts' address filter (runtime/aether_resource_caps.c), 32 KiB of .bss
# by default, for a small-RAM target such as the ESP32.
#
#   1. a value outside 6..24 is refused at compile time (#error);
#   2. -DAETHER_ACCT_FILTER_LOG2=8 makes the filter 32 bytes and the
#      default 32 KiB, asserted with sizeof at compile time;
#   3. a 32-byte filter, where most frees are false positives that take
#      the lock to find no entry, changes no answer: the account and
#      interrupt hosts of emit_lib_accounts and emit_lib_interrupt (their
#      fixtures, run here against the library linked with a LOG2=8
#      aether_resource_caps.o ahead of libaether.a) print what they print
#      with the default filter, and the accounts one the 11 lines it should.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
CC="${CC:-cc}"
NAME=acct_filter_log2
export AETHER_HOME="$ROOT"
export AETHER_STACK_TRACE=0
ACC="$ROOT/tests/integration/emit_lib_accounts"
INT="$ROOT/tests/integration/emit_lib_interrupt"

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: $AE not built"; exit 0; }
[ -f "$ROOT/build/libaether.a" ] || { echo "  [SKIP] $NAME: build/libaether.a not built"; exit 0; }

. "$ROOT/tests/lib/same_bytes.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

# shellcheck disable=SC2046
CFLAGS="-I$ROOT/include -I$ROOT/runtime $("$AE" cflags --cflags | tr -d '\r')"
# shellcheck disable=SC2046
LIBS="$("$AE" cflags --libs | tr -d '\r') -lpthread"
CAPS="$ROOT/runtime/aether_resource_caps.c"

fail() {
    echo "  [FAIL] $NAME: $1"
    shift
    for f in "$@"; do [ -f "$f" ] && head -10 "$f" | sed 's/^/        /'; done
    exit 1
}

# shellcheck disable=SC2086
$CC -O1 $CFLAGS -DAETHER_ACCT_FILTER_LOG2=8 -c "$CAPS" -o "$TMP/caps8.o" > "$TMP/cc8.log" 2>&1 ||
    fail "aether_resource_caps.c does not build with -DAETHER_ACCT_FILTER_LOG2=8" "$TMP/cc8.log"
# shellcheck disable=SC2086
$CC -O1 $CFLAGS -c "$CAPS" -o "$TMP/caps18.o" > "$TMP/cc18.log" 2>&1 ||
    fail "aether_resource_caps.c does not build" "$TMP/cc18.log"

# 1. out of range
for bad in 5 25; do
    # shellcheck disable=SC2086
    if $CC -O1 $CFLAGS -DAETHER_ACCT_FILTER_LOG2=$bad -c "$CAPS" -o "$TMP/bad.o" > "$TMP/bad.log" 2>&1; then
        fail "-DAETHER_ACCT_FILTER_LOG2=$bad was accepted; 6..24 is the range"
    fi
done

# 2. the filter's size, checked by the compiler: a probe includes the file
# (g_filter is static there) and asserts sizeof g_filter. Exact on every
# platform; symbol tables are not (COFF objects give no sizes, and their
# sections each start at address 0). 2^8 bits is 32 bytes; 2^18 is 32 KiB.
cat > "$TMP/size_probe.c" <<'PROBE'
#include "aether_resource_caps.c"
_Static_assert(sizeof g_filter == EXPECT_BYTES, "g_filter is not EXPECT_BYTES bytes");
PROBE
# shellcheck disable=SC2086
$CC -O1 $CFLAGS -DAETHER_ACCT_FILTER_LOG2=8 -DEXPECT_BYTES=32 -c "$TMP/size_probe.c" -o "$TMP/probe8.o" > "$TMP/probe8.log" 2>&1 ||
    fail "the filter is not 32 bytes with LOG2=8" "$TMP/probe8.log"
# shellcheck disable=SC2086
$CC -O1 $CFLAGS -DEXPECT_BYTES=32768 -c "$TMP/size_probe.c" -o "$TMP/probe18.o" > "$TMP/probe18.log" 2>&1 ||
    fail "the filter is not 32768 bytes by default" "$TMP/probe18.log"
size_note="32 bytes at LOG2=8, 32768 by default"

# 3. the hosts, with the default filter and with the small one.
"$AETHERC" --emit=lib --with=extern "$ACC/accts.ae" "$TMP/accts.c" > "$TMP/accts.log" 2>&1 ||
    fail "aetherc --emit=lib failed on accts.ae" "$TMP/accts.log"
"$AETHERC" --emit=lib --with=extern "$INT/work.ae" "$TMP/work.c" > "$TMP/work.log" 2>&1 ||
    fail "aetherc --emit=lib failed on work.ae" "$TMP/work.log"

for v in 18 8; do
    # shellcheck disable=SC2086
    extra=""; [ "$v" = 8 ] && extra="$TMP/caps8.o"
    # shellcheck disable=SC2086
    $CC -O1 -DHOST_STATIC $CFLAGS "$TMP/accts.c" "$ACC/hooks.c" "$ACC/host.c" $extra \
        -o "$TMP/acc$v" $LIBS > "$TMP/ccacc$v.log" 2>&1 ||
        fail "the accounts host did not build (filter LOG2=$v)" "$TMP/ccacc$v.log"
    # shellcheck disable=SC2086
    $CC -O1 $CFLAGS "$TMP/work.c" "$INT/host.c" $extra -o "$TMP/int$v" $LIBS > "$TMP/ccint$v.log" 2>&1 ||
        fail "the interrupt host did not build (filter LOG2=$v)" "$TMP/ccint$v.log"
    "$TMP/acc$v" 2>"$TMP/acc$v.err" | tr -d '\r' > "$TMP/acc$v.out"
    "$TMP/int$v" 2>"$TMP/int$v.err" | tr -d '\r' > "$TMP/int$v.out"
done

[ "$(wc -l < "$TMP/acc18.out" | tr -d ' ')" = 11 ] ||
    fail "the accounts host printed other than 11 lines with the default filter" "$TMP/acc18.out"
same_bytes "$TMP/acc18.out" "$TMP/acc8.out" ||
    fail "the accounts host answers differently with LOG2=8 (default first, then LOG2=8)" "$TMP/acc18.out" "$TMP/acc8.out"
[ "$(wc -l < "$TMP/int18.out" | tr -d ' ')" = 6 ] ||
    fail "the interrupt host printed other than 6 lines with the default filter" "$TMP/int18.out"
same_bytes "$TMP/int18.out" "$TMP/int8.out" ||
    fail "the interrupt host answers differently with LOG2=8" "$TMP/int8.out"

echo "  [PASS] $NAME: LOG2 outside 6..24 is refused, the filter shrinks ($size_note), and the accounts and interrupt hosts answer the same with a 32-byte filter as with the default"
