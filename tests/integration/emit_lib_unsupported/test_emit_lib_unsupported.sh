#!/bin/sh
# Verifies that --emit=lib:
#   (a) emits aether_<name> for functions with ABI-representable params
#   (b) skips stub + emits warning for functions with unsupported params
# Exercised by compiling config.ae to C, grepping the generated C and
# the aetherc stderr.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] test_emit_lib_unsupported on Windows"; exit 0 ;;
esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
pass=0; fail=0

# Compile .ae to C in lib mode, capturing stderr.
cd "$SCRIPT_DIR"
AETHER_HOME="" "$ROOT/build/aetherc" --emit=lib config.ae "$TMPDIR/config.c" \
    >"$TMPDIR/stdout.log" 2>"$TMPDIR/stderr.log"
status=$?

if [ "$status" -ne 0 ]; then
    echo "  [FAIL] aetherc returned $status — expected success"
    cat "$TMPDIR/stderr.log"
    fail=$((fail + 1))
else
    # Check (a) — the good function has a stub.
    if grep -q "^int32_t aether_good_fn(" "$TMPDIR/config.c"; then
        echo "  [PASS] aether_good_fn stub emitted"
        pass=$((pass + 1))
    else
        echo "  [FAIL] aether_good_fn stub missing from generated C"
        fail=$((fail + 1))
    fi

    # Check (b) — the closure-taking function is absent.
    if grep -q "aether_takes_closure" "$TMPDIR/config.c"; then
        echo "  [FAIL] aether_takes_closure stub should NOT have been emitted"
        fail=$((fail + 1))
    else
        echo "  [PASS] aether_takes_closure stub correctly skipped"
        pass=$((pass + 1))
    fi

    # Check (c) — a warning mentions the skipped function.
    if grep -q "takes_closure" "$TMPDIR/stderr.log" && \
       grep -q "skipping alias stub\|representable in the" "$TMPDIR/stderr.log"; then
        echo "  [PASS] warning emitted for unsupported param type"
        pass=$((pass + 1))
    else
        echo "  [FAIL] expected warning about takes_closure not emitted"
        echo "--- stderr was:"
        cat "$TMPDIR/stderr.log"
        fail=$((fail + 1))
    fi

    # Check (d) — tuple return: no aether_returns_tuple alias (#277).
    if grep -q "aether_returns_tuple" "$TMPDIR/config.c"; then
        echo "  [FAIL] aether_returns_tuple alias should NOT have been emitted"
        fail=$((fail + 1))
    else
        echo "  [PASS] tuple-return helper correctly skipped (#277)"
        pass=$((pass + 1))
    fi

    # Check (e) — tuple-return warning mentions the function.
    if grep -q "returns_tuple" "$TMPDIR/stderr.log" && \
       grep -q "tuple" "$TMPDIR/stderr.log"; then
        echo "  [PASS] warning emitted for tuple return type (#277)"
        pass=$((pass + 1))
    else
        echo "  [FAIL] expected warning about tuple return not emitted"
        cat "$TMPDIR/stderr.log"
        fail=$((fail + 1))
    fi

    # Check (f) — trailing-underscore helper: no alias even though its
    # parameter and return types ARE ABI-representable. Marks file-local
    # by convention. Closes #279.
    if grep -q "aether_helper_" "$TMPDIR/config.c"; then
        echo "  [FAIL] aether_helper_ alias should NOT have been emitted (trailing-_ convention)"
        fail=$((fail + 1))
    else
        echo "  [PASS] trailing-underscore helper correctly skipped (#279)"
        pass=$((pass + 1))
    fi

    # Check (g) — trailing-underscore helper is emitted as `static`.
    if grep -qE "static (AETHER_MAYBE_UNUSED )?(int|const char\*|void) helper_\(" "$TMPDIR/config.c"; then
        echo "  [PASS] helper_ emitted with static linkage (#279)"
        pass=$((pass + 1))
    else
        echo "  [FAIL] helper_ not emitted with static linkage"
        grep " helper_(" "$TMPDIR/config.c" | head -3
        fail=$((fail + 1))
    fi
fi

echo ""
echo "emit_lib_unsupported: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
