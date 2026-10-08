#!/bin/sh
# A heap-string struct field assigned from a heap-tracked local that holds a
# BORROWED value at runtime must move the source's runtime ownership into the
# field tracker, not hard-code it to 1. Hard-coding 1 made the destructor free
# a value the program never owned (a literal -> free of rodata), aborting at
# teardown. This is the closure/FFI-callback shape a downstream HTML sanitizer
# hit: a hook returns a literal, threaded through `-> string` helpers into a
# heap-boxed attr field, double-freed on free.
#
# Running to completion is half the assertion (a double free / rodata free
# aborts with SIGSEGV or SIGABRT). The emitted ownership move is checked
# directly too, so this means something on CI platforms with no leak checker.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
[ -x "$AE" ] || { echo "  [SKIP] heap_field_borrowed_var_no_double_free: build/ae missing"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

# 1. Generated C moves the source var's runtime tracker into the field, rather
#    than writing a literal 1. The borrowed store must NOT read `= 1`.
if ! "$AETHERC" "$SCRIPT_DIR/prog.ae" "$TMP/out.c" > "$TMP/gen.log" 2>&1; then
    echo "  [FAIL] heap_field_borrowed_var_no_double_free: codegen failed"
    sed 's/^/        /' "$TMP/gen.log" | head -8
    exit 1
fi
if ! grep -q "_heap_value = _heap_borrowed" "$TMP/out.c"; then
    echo "  [FAIL] heap_field_borrowed_var_no_double_free: borrowed store did not move the runtime tracker"
    echo "         expected '..._heap_value = _heap_borrowed; _heap_borrowed = 0;'"
    grep -n "value = borrowed\|_heap_value" "$TMP/out.c" | sed 's/^/        /' | head -6
    exit 1
fi
if grep -q "value = borrowed; ap->_heap_value = 1" "$TMP/out.c"; then
    echo "  [FAIL] heap_field_borrowed_var_no_double_free: borrowed store still hard-codes _heap_value = 1"
    exit 1
fi

# 1b. The mirror image: a var holding a GENUINELY owned value must reach the
#     field tracker as 1, or the destructor frees nothing and the buffer leaks.
#     For a var whose value escapes into a struct field the assignment used to
#     leave `_heap_<var>` at its stale 0, so the move carried a 0 and the leak
#     was silent on every platform without a leak checker. Asserted on the
#     generated C so it fails on those platforms too.
# The assignment is the plain one (`_heap_runtime_owned = 1` right after it)
# or, once the field store takes the local (#2564), the reassignment that
# frees the old value first; either way the tracker ends at 1.
if ! grep -qE "runtime_owned = build_owned\(7\);( if \(_heap_runtime_owned\) aether_heap_str_free\(_tmp_old\);)? _heap_runtime_owned = 1" "$TMP/out.c"; then
    echo "  [FAIL] heap_field_borrowed_var_no_double_free: an owned value that escapes"
    echo "         into a field left its tracker stale, so the field tracker takes 0"
    echo "         and the destructor never frees it"
    grep -n "runtime_owned" "$TMP/out.c" | sed 's/^/        /' | head -6
    exit 1
fi
if ! grep -q "_heap_value = _heap_runtime_owned" "$TMP/out.c"; then
    echo "  [FAIL] heap_field_borrowed_var_no_double_free: owned store did not move the runtime tracker"
    grep -n "value = runtime_owned\|_heap_value" "$TMP/out.c" | sed 's/^/        /' | head -6
    exit 1
fi

# 2. It runs to completion — the actual bug was a teardown abort. Build a real
#    binary and run it; a double free / rodata free crashes here.
if ! "$AE" build "$SCRIPT_DIR/prog.ae" -o "$TMP/prog" > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] heap_field_borrowed_var_no_double_free: build failed"
    sed 's/^/        /' "$TMP/build.log" | head -8
    exit 1
fi
if ! "$TMP/prog" > "$TMP/run.out" 2>&1; then
    echo "  [FAIL] heap_field_borrowed_var_no_double_free: program aborted at runtime (rc $?)"
    sed 's/^/        /' "$TMP/run.out" | head -8
    exit 1
fi
if ! grep -q "^done$" "$TMP/run.out"; then
    echo "  [FAIL] heap_field_borrowed_var_no_double_free: did not reach 'done'"
    sed 's/^/        /' "$TMP/run.out" | head -8
    exit 1
fi

# 3. Freed exactly once, where a leak checker is available. leaks(1) is the one
#    that works on macOS; Linux CI covers the same program under Valgrind.
if [ "$(uname -s)" = Darwin ] && command -v leaks >/dev/null 2>&1; then
    if ! MallocStackLogging=1 leaks --atExit -- "$TMP/prog" > "$TMP/leaks.out" 2>&1; then
        if grep -qE "[1-9][0-9]* leak(s)? for" "$TMP/leaks.out"; then
            echo "  [FAIL] heap_field_borrowed_var_no_double_free: the owned value leaked"
            grep -E "leaks? for" "$TMP/leaks.out" | sed 's/^/        /' | head -3
            exit 1
        fi
    fi
fi

echo "  [PASS] heap_field_borrowed_var_no_double_free: borrowed field-store moves runtime ownership; no teardown double-free"
exit 0
