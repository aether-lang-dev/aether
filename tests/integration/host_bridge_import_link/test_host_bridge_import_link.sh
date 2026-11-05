#!/bin/sh
# Regression: `import contrib.host.<lang>` queues libaether_host_<lang>.a
# onto the gcc link line; a pure-Aether program does NOT.
#
# This is the import-driven linking from ctr_notes.md Bug 4: without it,
# `import contrib.host.tcl` resolved the headers (so the program
# compiled) but never linked the bridge .a, so the binary failed at
# runtime with `undefined symbol: tcl_run` — the bridge's OWN
# function, not a CPython symbol.
#
# We verify by capturing `ae build -v` output and grepping for the .a
# (positive) or its absence (negative). We do NOT attempt to RUN the
# produced positive binary — that needs libtcl at runtime, which
# the CI box may not have. The link-line check is portable and
# sufficient to prove the wiring.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

fail=0
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# --- Setup: ensure contrib bridges are built so the positive case has
# a .a to find. Skip the test gracefully when host_tcl wasn't built
# (e.g. because tcl-dev is unavailable on this host).
TCL_A="$ROOT/build/contrib/libaether_host_tcl.a"
INST_TCL_A="$ROOT/lib/aether/libaether_host_tcl.a"
if [ ! -f "$TCL_A" ] && [ ! -f "$INST_TCL_A" ]; then
    # Try to build it once.
    # Only the bridge this test links (#2757): every contrib module took
    # 70 to 80 s of the Windows sweep, for one archive.
    (cd "$ROOT" && MODULES=tcl bash tests/scripts/contrib_build.sh) >/dev/null 2>&1 || true
fi
if [ ! -f "$TCL_A" ] && [ ! -f "$INST_TCL_A" ]; then
    echo "  [SKIP] host_bridge_import_link: libaether_host_tcl.a not built (tcl-dev not installed?)"
    exit 0
fi

# --- Test A (positive): import contrib.host.tcl → .a in link line.
# We don't need the source to compile/link successfully — the link line
# is logged before the linker actually runs, so even a failing link
# proves what flags reached gcc. To avoid spurious "build failed" noise
# we redirect everything; we read the verbose log for the .a path.
cat > "$tmpdir/a.ae" <<'EOF'
import contrib.host.tcl

main() {
    tcl.run_sandboxed(0, "puts unreached")
}
EOF
"$AE" build "$tmpdir/a.ae" -o "$tmpdir/a_out" -v >"$tmpdir/a.log" 2>&1 || true

if grep -q 'libaether_host_tcl\.a' "$tmpdir/a.log"; then
    echo "  [PASS] host_bridge_import_link A: contrib.host.tcl import queues libaether_host_tcl.a"
else
    echo "  [FAIL] host_bridge_import_link A: bridge .a missing from link line despite the import"
    grep -E 'host bridge|libaether_host|\[cmd\]' "$tmpdir/a.log" | sed 's/^/    /' | head -5
    fail=1
fi

# --- Test B (negative): pure-Aether program → NO bridge .a on link
# line, AND the build must succeed + run. This guards the critical
# "do not blanket-link" constraint: a hello-world must not pick up
# libtcl runtime deps it doesn't need.
cat > "$tmpdir/b.ae" <<'EOF'
main() { println("hi") }
EOF
if ! "$AE" build "$tmpdir/b.ae" -o "$tmpdir/b_out" -v >"$tmpdir/b.log" 2>&1; then
    echo "  [FAIL] host_bridge_import_link B: pure-Aether build failed (unexpected)"
    sed 's/^/    /' "$tmpdir/b.log" | tail -5
    fail=1
elif grep -qE 'libaether_host_[a-z]+\.a' "$tmpdir/b.log"; then
    echo "  [FAIL] host_bridge_import_link B: pure-Aether build pulled in a bridge .a (blanket-link bug)"
    grep -E 'libaether_host_[a-z]+\.a' "$tmpdir/b.log" | sed 's/^/    /' | head -3
    fail=1
elif ! "$tmpdir/b_out" 2>&1 | grep -q '^hi$'; then
    echo "  [FAIL] host_bridge_import_link B: pure-Aether binary did not print 'hi'"
    "$tmpdir/b_out" 2>&1 | sed 's/^/    /' | head -3
    fail=1
else
    echo "  [PASS] host_bridge_import_link B: pure-Aether build has no bridge .a, runs cleanly"
fi

# --- Test C (hard error): `import contrib.host.nonexistent` → ae
# rejects the build with a clear actionable message.
cat > "$tmpdir/c.ae" <<'EOF'
import contrib.host.definitelynothere

main() { println("never") }
EOF
"$AE" build "$tmpdir/c.ae" -o "$tmpdir/c_out" >"$tmpdir/c.log" 2>&1
c_rc=$?
if [ "$c_rc" -eq 0 ]; then
    echo "  [FAIL] host_bridge_import_link C: missing bridge .a did not trigger hard error"
    fail=1
elif ! grep -q "libaether_host_definitelynothere.a" "$tmpdir/c.log"; then
    echo "  [FAIL] host_bridge_import_link C: error did not name the missing .a"
    sed 's/^/    /' "$tmpdir/c.log" | head -5
    fail=1
elif ! grep -q "make contrib\|install-contrib" "$tmpdir/c.log"; then
    echo "  [FAIL] host_bridge_import_link C: error did not point at remediation"
    sed 's/^/    /' "$tmpdir/c.log" | head -5
    fail=1
else
    echo "  [PASS] host_bridge_import_link C: missing bridge .a → actionable hard error"
fi

exit $fail
