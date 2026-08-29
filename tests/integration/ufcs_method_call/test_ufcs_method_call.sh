#!/bin/sh
# Issue #928: method-call-on-value (UFCS). `x.f(args)` desugars to
# `f(x, args)` when `f` is a free function whose first parameter type
# matches typeof(x). Implemented as a strict last-resort fallback, so it
# only fires on dotted calls that would otherwise be "Undefined function"
# — module-qualified calls and struct/fnptr-field dispatch keep priority.
#
# Covers: the positive transcript (chained call-result receiver, stored-
# value receiver, pointer receiver), precedence (a real module call still
# resolves), and the two ways UFCS must DECLINE (no such free function;
# first-param type mismatch) — both of which must surface a clean
# Undefined-function error, not a miscompile.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

# --- Positive: the probe program runs and prints the pinned transcript --
# stdout only. This pins the PROGRAM's transcript, and compiler diagnostics
# go to stderr -- folding them in with 2>&1 made the comparison depend on
# whether the toolchain happened to be chatty. It did once #1780 stopped
# `ae run` swallowing compiler warnings: probe.ae trips a pre-existing
# "unresolved type in codegen" warning (aetherc emits it on main too; `ae`
# was hiding it), and a diagnostic that has nothing to do with UFCS started
# failing a UFCS transcript. stderr is captured separately so a genuine error
# is still shown when something goes wrong.
err_file="${TMPDIR:-/tmp}/ae_ufcs_probe_err.$$"
out=$(AETHER_HOME="" "$AE" run "$SCRIPT_DIR/probe.ae" 2>"$err_file")
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "ufcs_method_call: FAIL (probe errored, rc=$rc)"
    echo "$out" | head -20 | sed 's/^/  /'
    sed -n '1,20p' "$err_file" | sed 's/^/  stderr: /'
    rm -f "$err_file"
    exit 1
fi
rm -f "$err_file"
expected="eq ok
eq ok
eq ok
eq ok
count 3"
if [ "$out" != "$expected" ]; then
    echo "ufcs_method_call: FAIL (transcript mismatch)"
    echo "  --- got ---";      echo "$out"      | sed 's/^/  /'
    echo "  --- want ---";     echo "$expected" | sed 's/^/  /'
    exit 1
fi

# --- Precedence: a genuine module call must NOT be hijacked by UFCS ------
cat > /tmp/ae_ufcs_mod.ae << 'EOF'
import std.string
main() {
    s = "hello"
    println("${string.length(s)}")
}
EOF
mod_out=$(AETHER_HOME="" "$AE" run /tmp/ae_ufcs_mod.ae 2>&1)
mod_rc=$?
rm -f /tmp/ae_ufcs_mod.ae
if [ "$mod_rc" -ne 0 ] || [ "$mod_out" != "5" ]; then
    echo "ufcs_method_call: FAIL (module call regressed: rc=$mod_rc out=[$mod_out])"
    exit 1
fi

# --- Decline 1: no matching free function -> clean Undefined error -------
cat > /tmp/ae_ufcs_nomatch.ae << 'EOF'
struct Foo { x }
main() {
    f = Foo{ x: 1 }
    _ = f.no_such_method(2)
}
EOF
nm_out=$(AETHER_HOME="" "$AE" build /tmp/ae_ufcs_nomatch.ae -o /tmp/ae_ufcs_nomatch_out 2>&1)
rm -f /tmp/ae_ufcs_nomatch.ae /tmp/ae_ufcs_nomatch_out
if ! echo "$nm_out" | grep -q "Undefined function"; then
    echo "ufcs_method_call: FAIL (no-match UFCS did not produce Undefined-function error)"
    echo "$nm_out" | head -10 | sed 's/^/  /'
    exit 1
fi

# --- Decline 2: first-param type mismatch -> must NOT rewrite ------------
cat > /tmp/ae_ufcs_mismatch.ae << 'EOF'
struct A { x }
struct B { y }
takes_b(b: B, n: int) -> int { return b.y + n }
main() {
    a = A{ x: 1 }
    _ = a.takes_b(2)
}
EOF
mm_out=$(AETHER_HOME="" "$AE" build /tmp/ae_ufcs_mismatch.ae -o /tmp/ae_ufcs_mismatch_out 2>&1)
rm -f /tmp/ae_ufcs_mismatch.ae /tmp/ae_ufcs_mismatch_out
if ! echo "$mm_out" | grep -q "Undefined function"; then
    echo "ufcs_method_call: FAIL (type-mismatched receiver was wrongly rewritten)"
    echo "$mm_out" | head -10 | sed 's/^/  /'
    exit 1
fi

echo "ufcs_method_call: PASS"
exit 0
