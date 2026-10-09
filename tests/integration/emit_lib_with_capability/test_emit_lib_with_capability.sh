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

# ---- 7. a program's own extern is the extern capability ----
#
# `extern system(...)` reaches what `import std.os` would, so without
# --with=extern a capability-empty library may not declare one: not in the
# entry file, not with @extern, not in a local module. std/contrib modules'
# own externs are unaffected (they sit behind the gates above).

ext_case() {   # <name> <dir> <entry> <flags> <accept|reject>
    name="$1"; dir="$2"; entry="$3"; flags="$4"; want="$5"
    if (cd "$dir" && AETHER_HOME="" "$ROOT/build/aetherc" $flags "$entry" "$TMPDIR/ext.c" \
            >"$TMPDIR/stdout" 2>"$TMPDIR/stderr"); then got=accept; else got=reject; fi
    if [ "$got" != "$want" ]; then
        echo "  [FAIL] $name: $got, want $want"; sed 's/^/    /' "$TMPDIR/stderr" | head -5
        fail=$((fail + 1))
    elif [ "$want" = reject ] && ! grep -q "without --with=extern" "$TMPDIR/stderr"; then
        echo "  [FAIL] $name: rejected, but not by the extern gate"; sed 's/^/    /' "$TMPDIR/stderr" | head -5
        fail=$((fail + 1))
    else
        echo "  [PASS] $name"; pass=$((pass + 1))
    fi
}

mkdir -p "$TMPDIR/ext" "$TMPDIR/extmod"
printf 'extern system(cmd: string) -> int\n\nf() -> int {\n    return system("true")\n}\n' > "$TMPDIR/ext/plain.ae"
printf '@extern("system") run_it(cmd: string) -> int\n\nf() -> int {\n    return run_it("true")\n}\n' > "$TMPDIR/ext/at.ae"
printf 'import std.json\n\nf(s: string) -> int {\n    return 1\n}\n' > "$TMPDIR/ext/stdonly.ae"
printf 'extern fopen(path: string, mode: string) -> ptr\n\nopen_it(p: string) -> ptr {\n    return fopen(p, "r")\n}\n' > "$TMPDIR/extmod/helper.ae"
printf 'import helper\n\ng(p: string) -> int {\n    if helper.open_it(p) == null { return 0 }\n    return 1\n}\n' > "$TMPDIR/extmod/main.ae"

ext_case "an extern in the entry file is rejected"        "$TMPDIR/ext"    plain.ae    "--emit=lib"                 reject
ext_case "an @extern(...) binding is rejected"            "$TMPDIR/ext"    at.ae       "--emit=lib"                 reject
ext_case "an extern in a local module is rejected"        "$TMPDIR/extmod" main.ae     "--emit=lib"                 reject
ext_case "--with=extern accepts it"                       "$TMPDIR/ext"    plain.ae    "--emit=lib --with=extern"   accept
ext_case "--with=extern accepts a local module's extern"  "$TMPDIR/extmod" main.ae     "--emit=lib --with=extern"   accept
ext_case "--with=all includes extern"                     "$TMPDIR/ext"    plain.ae    "--emit=lib --with=all"      accept
ext_case "--with=fs,net,os does not include extern"       "$TMPDIR/ext"    plain.ae    "--emit=lib --with=fs,net,os" reject
ext_case "std modules' own externs are not the program's" "$TMPDIR/ext"    stdonly.ae  "--emit=lib"                 accept
ext_case "an exe build is unaffected"                     "$TMPDIR/ext"    plain.ae    ""                           accept

echo ""
echo "emit_lib_with_capability: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
