#!/bin/sh
# --emit=lib --with=<cap>: capability opt-in for host-owned library builds.
#
# The default posture (capability-empty, no --with flag) stays unchanged
# — that's pinned by the existing emit_lib_banned test. This test pins
# the opt-in direction:
#
#   1. --with=fs unlocks std.fs but still rejects std.net / std.os.
#   2. --with=net unlocks std.net / std.http / std.tcp but not std.fs.
#   3. --with=os unlocks std.os.
#   4. --with=fs,os unlocks both in one invocation.
#   5. --with=<unknown> is a hard error, not a silent no-op.
#   6. --with=fs WITHOUT --emit=lib is a no-op (no error, no effect).
#   7. --with=first-party unlocks fs / net / os in one go.
#   8. --with=all is an alias for --with=first-party.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
pass=0; fail=0

# Helper: run aetherc with a set of flags on a throwaway .ae that
# imports $mod. Expect either "accept" (exit 0) or "reject" (non-zero
# with the capability error pattern). $label is just for the report.
run_case() {
    label="$1"; mod="$2"; flags="$3"; expect="$4"
    printf 'import %s\nfoo(x: int) { return x }\n' "$mod" > "$TMPDIR/probe.ae"
    rm -f "$TMPDIR/probe.c" "$TMPDIR/stderr"
    AETHER_HOME="" "$ROOT/build/aetherc" $flags "$TMPDIR/probe.ae" "$TMPDIR/probe.c" \
        >"$TMPDIR/stdout" 2>"$TMPDIR/stderr"
    rc=$?

    if [ "$expect" = "accept" ]; then
        if [ "$rc" -eq 0 ]; then
            echo "  [PASS] $label"; pass=$((pass + 1))
        else
            echo "  [FAIL] $label — expected accept, got reject:"
            sed 's/^/    /' "$TMPDIR/stderr" | head -5
            fail=$((fail + 1))
        fi
    else
        if [ "$rc" -ne 0 ] && grep -q "capability-empty\|--with=" "$TMPDIR/stderr"; then
            echo "  [PASS] $label"; pass=$((pass + 1))
        else
            echo "  [FAIL] $label — expected reject (rc=$rc, stderr:)"
            sed 's/^/    /' "$TMPDIR/stderr" | head -5
            fail=$((fail + 1))
        fi
    fi
}

# ---- 1. --with=fs unlocks std.fs, still rejects std.net / std.os ----

run_case "--with=fs accepts std.fs"       std.fs  "--emit=lib --with=fs"  accept
run_case "--with=fs still rejects std.net" std.net "--emit=lib --with=fs"  reject
run_case "--with=fs still rejects std.os"  std.os  "--emit=lib --with=fs"  reject

# ---- 2. --with=net unlocks net/http/tcp, not fs ----

run_case "--with=net accepts std.net"  std.net  "--emit=lib --with=net" accept
run_case "--with=net accepts std.http" std.http "--emit=lib --with=net" accept
run_case "--with=net accepts std.tcp"  std.tcp  "--emit=lib --with=net" accept
run_case "--with=net still rejects std.fs" std.fs "--emit=lib --with=net" reject

# ---- 3. --with=os unlocks std.os ----

run_case "--with=os accepts std.os"    std.os  "--emit=lib --with=os"  accept

# ---- 4. --with=fs,os unlocks both in one invocation ----

run_case "--with=fs,os accepts std.fs" std.fs  "--emit=lib --with=fs,os" accept
run_case "--with=fs,os accepts std.os" std.os  "--emit=lib --with=fs,os" accept
run_case "--with=fs,os still rejects std.net" std.net "--emit=lib --with=fs,os" reject

# ---- 4b. --with=first-party unlocks fs / net / os ----

run_case "--with=first-party accepts std.fs"  std.fs  "--emit=lib --with=first-party" accept
run_case "--with=first-party accepts std.net" std.net "--emit=lib --with=first-party" accept
run_case "--with=first-party accepts std.os"  std.os  "--emit=lib --with=first-party" accept

# ---- 4c. --with=all is the same alias ----

run_case "--with=all accepts std.fs"  std.fs  "--emit=lib --with=all" accept
run_case "--with=all accepts std.net" std.net "--emit=lib --with=all" accept
run_case "--with=all accepts std.os"  std.os  "--emit=lib --with=all" accept

# ---- 5. --with=<unknown> is a hard error ----

printf 'foo(x: int) { return x }\n' > "$TMPDIR/probe.ae"
if AETHER_HOME="" "$ROOT/build/aetherc" --emit=lib --with=bogus \
    "$TMPDIR/probe.ae" "$TMPDIR/probe.c" >"$TMPDIR/stdout" 2>"$TMPDIR/stderr"; then
    echo "  [FAIL] --with=bogus was accepted"; fail=$((fail + 1))
elif grep -q "unknown capability" "$TMPDIR/stderr"; then
    echo "  [PASS] --with=bogus is rejected with a clear message"; pass=$((pass + 1))
else
    echo "  [FAIL] --with=bogus rejected, but without an unknown-capability message"
    sed 's/^/    /' "$TMPDIR/stderr" | head -5
    fail=$((fail + 1))
fi

# ---- 6. --with=fs without --emit=lib is a no-op (exe build) ----

printf 'import std.fs\nmain() { }\n' > "$TMPDIR/probe.ae"
if AETHER_HOME="" "$ROOT/build/aetherc" --with=fs "$TMPDIR/probe.ae" "$TMPDIR/probe.c" \
    >"$TMPDIR/stdout" 2>"$TMPDIR/stderr"; then
    echo "  [PASS] --with=fs without --emit=lib compiles (exe build)"
    pass=$((pass + 1))
else
    echo "  [FAIL] --with=fs without --emit=lib was rejected — expected success"
    sed 's/^/    /' "$TMPDIR/stderr" | head -5
    fail=$((fail + 1))
fi

echo ""
echo "emit_lib_with_capability: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
