#!/bin/sh
# A panic in an export returns to the export's stub however the library's C
# and the runtime were each compiled for control-flow protection.
#
# On x86 an export stub saves its context with GCC's __builtin_setjmp, and a
# panic jumped back with __builtin_longjmp from the runtime
# (aether_jump_to). The two builtins share a buffer whose layout depends on
# -fcf-protection: with it, slot 2 holds the shadow-stack pointer and slot 3
# the stack pointer; without it, slot 2 holds the stack pointer. The stub is
# compiled by whatever builds the library, the runtime by whatever built
# libaether, and the two differ in practice: Ubuntu's gcc protects by
# default, Arch's does not, and a release runtime is built on Ubuntu. On
# CachyOS (gcc 16, an N100) a library's panic ran the runtime's protected
# longjmp against an unprotected stub's buffer: it took the stack pointer
# for a shadow-stack pointer and ran incsspq, which faults (SIGILL) on a CPU
# with CET where shadow stacks are off (RexxAE's test_resource_cleanup.py).
# The other way round, it loads the stack pointer from a slot that holds 0.
#
# So the library's generated C is built here twice, with
# -fcf-protection=full and with -fcf-protection=none, against the one
# runtime: one of the two differs from the runtime whichever way it was
# built. Each must return the zero value with AETHER_PANICKED (1), take the
# next call, and return normally from a call that does not panic.
# x86 with a compiler that takes the flag; elsewhere the stub uses the C
# library's setjmp and there is nothing to mismatch.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
CC="${CC:-cc}"
export AETHER_HOME="$ROOT"
export AETHER_STACK_TRACE=0

[ -x "$AE" ] || { echo "  [SKIP] emit_lib_panic_cf_protection: $AE not built"; exit 0; }
case "$(uname -m 2>/dev/null)" in
    x86_64|amd64|i?86) ;;
    *) echo "  [SKIP] emit_lib_panic_cf_protection: not x86 (the stub uses the C library's setjmp)"; exit 0 ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

printf 'int main(void) { return 0; }\n' > "$TMP/probe.c"
if ! "$CC" -fcf-protection=full -c "$TMP/probe.c" -o "$TMP/probe.o" > /dev/null 2>&1; then
    echo "  [SKIP] emit_lib_panic_cf_protection: $CC does not take -fcf-protection"
    exit 0
fi

if ! "$AETHERC" --emit=lib "$SCRIPT_DIR/boom.ae" "$TMP/boom.c" > "$TMP/aetherc.log" 2>&1; then
    echo "  [FAIL] emit_lib_panic_cf_protection: aetherc --emit=lib failed"
    tail -5 "$TMP/aetherc.log" | sed 's/^/        /'
    exit 1
fi

fail=0
want='boom 0 outcome 1; boom 0 outcome 1; plain 1 outcome 0'
for cf in full none; do
    # shellcheck disable=SC2046
    if ! "$CC" -O2 -fcf-protection=$cf -I"$ROOT/include" -I"$ROOT/runtime" $("$AE" cflags --cflags) \
            "$TMP/boom.c" "$SCRIPT_DIR/host.c" -o "$TMP/host_$cf" \
            $("$AE" cflags --libs) > "$TMP/cc_$cf.log" 2>&1; then
        echo "  [FAIL] emit_lib_panic_cf_protection: the host did not build with -fcf-protection=$cf"
        head -10 "$TMP/cc_$cf.log" | sed 's/^/        /'
        fail=1
        continue
    fi
    rc=0
    "$TMP/host_$cf" > "$TMP/out_$cf" 2> "$TMP/err_$cf" || rc=$?
    got="$(tr -d '\r' < "$TMP/out_$cf")"
    if [ "$rc" -ne 0 ] || [ "$got" != "$want" ]; then
        echo "  [FAIL] emit_lib_panic_cf_protection: the library's C built with -fcf-protection=$cf:"
        echo "      exit $rc (132 is SIGILL, 139 SIGSEGV), printed: $got"
        echo "      want: $want"
        head -5 "$TMP/err_$cf" | sed 's/^/        /'
        fail=1
    fi
done

if [ "$fail" = 0 ]; then
    echo "  [PASS] emit_lib_panic_cf_protection: a panicking export returns AETHER_PANICKED with the library's C built with and without -fcf-protection"
fi
exit $fail
