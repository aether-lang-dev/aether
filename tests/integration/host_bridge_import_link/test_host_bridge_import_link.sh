#!/bin/sh
# Regression: `import contrib.host.<lang>` queues libaether_host_<lang>.a
# onto the gcc link line; a pure-Aether program does NOT.
#
# This is the import-driven linking from ctr_notes.md Bug 4: without it,
# `import contrib.host.<lang>` resolved the module (so the program
# compiled) but never linked the bridge .a, so the binary failed with
# an undefined bridge symbol (`tcl_run`, back when the Tcl bridge was
# here) — the bridge's OWN function, not the hosted language's.
#
# The fixture is contrib.host.stunt, the test double: its bridge needs no
# library to build, so the positive case runs on every machine. (The
# language bridges moved to aether-guest-languages.)
#
# We verify by capturing `ae build -v` output and grepping for the .a
# (positive) or its absence (negative). We do NOT attempt to RUN the
# produced positive binary; the link-line check is portable and
# sufficient to prove the wiring.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

fail=0
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# --- Setup: ensure the stunt bridge is built so the positive case has
# a .a to find.
STUNT_A="$ROOT/build/contrib/libaether_host_stunt.a"
INST_STUNT_A="$ROOT/lib/aether/libaether_host_stunt.a"
if [ ! -f "$STUNT_A" ] && [ ! -f "$INST_STUNT_A" ]; then
    # Try to build it once.
    # Only the bridge this test links (#2757): every contrib module took
    # 70 to 80 s of the Windows sweep, for one archive.
    (cd "$ROOT" && MODULES=stunt bash tests/scripts/contrib_build.sh) >/dev/null 2>&1 || true
fi
if [ ! -f "$STUNT_A" ] && [ ! -f "$INST_STUNT_A" ]; then
    # stunt's bridge needs only libc and the in-tree runtime: not building
    # it is a failure, not a missing dependency.
    echo "  [FAIL] host_bridge_import_link: libaether_host_stunt.a did not build"
    exit 1
fi

# --- Test A (positive): import contrib.host.stunt → .a in link line.
# We don't need the source to compile/link successfully — the link line
# is logged before the linker actually runs, so even a failing link
# proves what flags reached gcc. To avoid spurious "build failed" noise
# we redirect everything; we read the verbose log for the .a path.
cat > "$tmpdir/a.ae" <<'EOF'
import contrib.host.stunt

main() {
    stunt.run("open-etc-passwd succeed")
}
EOF
"$AE" build "$tmpdir/a.ae" -o "$tmpdir/a_out" -v >"$tmpdir/a.log" 2>&1 || true

if grep -q 'libaether_host_stunt\.a' "$tmpdir/a.log"; then
    echo "  [PASS] host_bridge_import_link A: contrib.host.stunt import queues libaether_host_stunt.a"
else
    echo "  [FAIL] host_bridge_import_link A: bridge .a missing from link line despite the import"
    grep -E 'host bridge|libaether_host|\[cmd\]' "$tmpdir/a.log" | sed 's/^/    /' | head -5
    fail=1
fi

# --- Test B (negative): pure-Aether program → NO bridge .a on link
# line, AND the build must succeed + run. This guards the critical
# "do not blanket-link" constraint: a hello-world must not pick up
# a bridge it doesn't call.
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
