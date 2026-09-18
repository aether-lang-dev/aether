#!/bin/sh
# Verifies the --emit=lib + --with=fs sandbox story documented in
# contrib/templating/liquid/README.md.
#
# `import contrib.templating.liquid` pulls in `std.fs` transitively
# (for {% include %} / {% render %} / parse_file). Under --emit=lib
# without --with=fs, the import gate must reject this transitively.
# With --with=fs, the import must succeed.
#
# Cases:
#   1.  --emit=lib (no --with) rejects with the capability-empty error
#   2.  --emit=lib --with=fs accepts the import
#   3.  Direct `import std.fs` (without contrib.templating.liquid)
#       under --emit=lib without --with=fs still rejected — control
#       that confirms the gate is doing its job
#   4.  --emit=exe (the default) accepts the import — only --emit=lib
#       gates apply

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] liquid_sandbox_gate on Windows"; exit 0 ;;
esac

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR" || true' EXIT
pass=0; fail=0

# The probe program just imports the module and defines a no-op
# function so the compiler has something to emit. We're not exercising
# render — we're exercising the gate.
cat > "$TMPDIR/probe.ae" <<'EOF'
import contrib.templating.liquid

aether_noop(x: int) -> int { return x }
EOF

# Case 1: --emit=lib without --with=fs must reject.
if AETHER_HOME="$ROOT" "$ROOT/build/aetherc" --emit=lib "$TMPDIR/probe.ae" "$TMPDIR/probe.c" >"$TMPDIR/stdout" 2>"$TMPDIR/stderr"; then
    echo "  [FAIL] 1: --emit=lib accepted contrib.templating.liquid without --with=fs"
    fail=$((fail + 1))
elif grep -q "capability-empty" "$TMPDIR/stderr"; then
    echo "  [PASS] 1: --emit=lib rejected contrib.templating.liquid without --with=fs"
    pass=$((pass + 1))
else
    echo "  [FAIL] 1: rejected but message did not contain 'capability-empty'"
    cat "$TMPDIR/stderr"
    fail=$((fail + 1))
fi

# Case 2: --emit=lib --with=fs must accept.
if AETHER_HOME="$ROOT" "$ROOT/build/aetherc" --emit=lib --with=fs "$TMPDIR/probe.ae" "$TMPDIR/probe.c" >"$TMPDIR/stdout" 2>"$TMPDIR/stderr"; then
    echo "  [PASS] 2: --emit=lib --with=fs accepted contrib.templating.liquid"
    pass=$((pass + 1))
else
    echo "  [FAIL] 2: --emit=lib --with=fs rejected — expected success"
    cat "$TMPDIR/stderr"
    fail=$((fail + 1))
fi

# Case 3: control — direct `import std.fs` still rejected without --with=fs.
cat > "$TMPDIR/direct.ae" <<'EOF'
import std.fs

aether_noop(x: int) -> int { return x }
EOF

if AETHER_HOME="$ROOT" "$ROOT/build/aetherc" --emit=lib "$TMPDIR/direct.ae" "$TMPDIR/direct.c" >"$TMPDIR/stdout" 2>"$TMPDIR/stderr"; then
    echo "  [FAIL] 3 control: direct import std.fs accepted without --with=fs"
    fail=$((fail + 1))
elif grep -q "capability-empty" "$TMPDIR/stderr"; then
    echo "  [PASS] 3 control: direct import std.fs rejected (gate working)"
    pass=$((pass + 1))
else
    echo "  [FAIL] 3 control: rejected but wrong message"
    cat "$TMPDIR/stderr"
    fail=$((fail + 1))
fi

# Case 4: --emit=exe (default) accepts contrib.templating.liquid even
# without --with=fs — gates only apply in lib mode. We need a `main`
# for emit=exe.
cat > "$TMPDIR/exe.ae" <<'EOF'
import contrib.templating.liquid

main() {
    ctx = liquid.context_new()
    liquid.context_free(ctx)
}
EOF

if AETHER_HOME="$ROOT" "$ROOT/build/aetherc" "$TMPDIR/exe.ae" "$TMPDIR/exe.c" >"$TMPDIR/stdout" 2>"$TMPDIR/stderr"; then
    echo "  [PASS] 4: --emit=exe accepted contrib.templating.liquid without --with=fs"
    pass=$((pass + 1))
else
    echo "  [FAIL] 4: --emit=exe rejected — expected success"
    cat "$TMPDIR/stderr"
    fail=$((fail + 1))
fi

echo ""
if [ "$fail" -eq 0 ]; then
    echo "  [PASS] liquid_sandbox_gate: $pass/4"
    exit 0
else
    echo "  [FAIL] liquid_sandbox_gate: $pass passed, $fail failed"
    exit 1
fi
