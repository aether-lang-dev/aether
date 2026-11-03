#!/bin/sh
# Integration test for multi-entry --lib search path (issue #413).
# Five cases — each covers one of the documented surfaces:
#
#   1. Single-entry --lib (today's behaviour, preserved)
#   2. Multi-entry --lib via PATH-style separator (`a:b` POSIX / `a;b` Win)
#   3. Multi-entry --lib via repeated flag (`--lib a --lib b`)
#   4. AETHER_LIB_DIR env var with the same separator-string shape
#   5. Precedence: left-most wins on module-name collisions
#   8-9. #3504: --lib and AETHER_LIB_DIR add after the project's lib/,
#        which stays first (they replaced it)
#
# All five fixtures live next to this script under dirA/ and dirB/:
#   dirA/foo    foo_origin() -> "A"
#   dirB/foo    foo_origin() -> "B"
#   dirB/bar    bar_origin() -> "bar-from-B"
# The main.ae driver imports both `foo` and `bar` and prints
# `foo=<x> bar=<y>` so each case asserts which directories the
# resolution walked.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

# Platform path separator (matches AETHER_LIB_PATH_SEP_CHAR in tools/ae.c).
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) SEP=";";;
    *)                                 SEP=":";;
esac

# On Windows / MSYS2, ae normalises MSYS2 POSIX-form paths (`/d/foo`)
# to native Windows form (`D:/foo`) at the lib-dir entry point — see
# `aether_lib_path_normalize` in compiler/aether_lib_path.h. That
# means `ae lib-path` will print `D:/a/...` even when invoked with
# `/d/a/...` argv. To compare ae's output byte-for-byte against the
# values we built in shell, we need to apply the SAME normalisation
# here. No-op on POSIX. Single source of truth — if the C-side
# function changes, this helper has to track it. uname-gated so the
# sed call is skipped entirely on Linux / macOS.
normalize_path() {
    case "$(uname -s 2>/dev/null)" in
        MINGW*|MSYS*|CYGWIN*|Windows_NT)
            # `/d/foo` → `D:/foo`. The character class is portable
            # across BSD sed (macOS) and GNU sed (Linux/MSYS2); the
            # `\u\1` upper-case substitution is GNU-only but only
            # this branch runs on MSYS2 where sed is GNU.
            printf '%s' "$1" | sed -E 's|^/([a-zA-Z])/|\U\1:/|'
            ;;
        *)
            printf '%s' "$1"
            ;;
    esac
}

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

pass=0; fail=0

run_case() {
    label="$1"; expected="$2"; shift 2
    out=$("$@" 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] $label: ae exited $rc"
        echo "    output: $out"
        fail=$((fail + 1)); return
    fi
    last=$(printf '%s\n' "$out" | tail -1)
    if [ "$last" = "$expected" ]; then
        echo "  [PASS] $label"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $label"
        echo "    expected: $expected"
        echo "    got:      $last"
        fail=$((fail + 1))
    fi
}

# Unset AETHER_LIB_DIR so it doesn't bleed in from the test harness
# environment.
unset AETHER_LIB_DIR

# Case 1: single-entry --lib (only dirA exists in this case — we
# expect the `bar` import to fail, so the test imports only `foo`).
# Use a small driver that just imports foo to keep the case clean.
mkdir -p "$TMPDIR/only_foo"
cat > "$TMPDIR/only_foo/main.ae" <<EOF
import foo
main() { println("foo=\${foo.foo_origin()}") }
EOF
run_case "single-entry --lib (today's shape)" \
    "foo=A" \
    "$AE" run "$TMPDIR/only_foo/main.ae" --lib "$SCRIPT_DIR/dirA"

# Case 2: multi-entry separator-string. dirA first (foo from A),
# dirB second (bar from B).
run_case "multi-entry --lib via separator-string" \
    "foo=A bar=bar-from-B" \
    "$AE" run "$SCRIPT_DIR/main.ae" --lib "$SCRIPT_DIR/dirA${SEP}$SCRIPT_DIR/dirB"

# Case 3: repeated --lib flags compose. Same outcome as case 2.
run_case "multi-entry via repeated --lib flag" \
    "foo=A bar=bar-from-B" \
    "$AE" run "$SCRIPT_DIR/main.ae" --lib "$SCRIPT_DIR/dirA" --lib "$SCRIPT_DIR/dirB"

# Case 4: AETHER_LIB_DIR env var with the same separator shape.
run_case "AETHER_LIB_DIR env var (separator-string)" \
    "foo=A bar=bar-from-B" \
    env AETHER_LIB_DIR="$SCRIPT_DIR/dirA${SEP}$SCRIPT_DIR/dirB" "$AE" run "$SCRIPT_DIR/main.ae"

# Case 5: precedence — flip the order, dirB first. foo now resolves
# to B (B's foo wins via left-most-first), bar still from B.
run_case "left-most wins on collision" \
    "foo=B bar=bar-from-B" \
    "$AE" run "$SCRIPT_DIR/main.ae" --lib "$SCRIPT_DIR/dirB${SEP}$SCRIPT_DIR/dirA"

# Case 6: trailing-slash normalisation — `--lib ./lib/` and
# `--lib ./lib` resolve to the same entry, lookup paths stay
# clean (`<entry>/<module>.ae` not `./lib//<module>.ae`).
run_case "trailing-slash normalised" \
    "foo=A bar=bar-from-B" \
    "$AE" run "$SCRIPT_DIR/main.ae" --lib "$SCRIPT_DIR/dirA/${SEP}$SCRIPT_DIR/dirB/"

# Case 7: `ae lib-path` introspection. Print the resolved chain and
# assert each entry appears on its own line, in order. Strip any
# stray `\r` from `out` defensively — ae forces LF-only output via
# binary-mode stdout on Windows now, but a shell or terminal layer
# could still inject a CR; we don't want this assertion to fail on
# something that looks identical to a human eye. Apply the same
# POSIX→Windows path normalisation ae uses when building `expected`
# (no-op on POSIX) so the byte-comparison succeeds on every host. The
# project's `lib` comes first, the --lib entries after it (#3504).
out=$("$AE" lib-path --lib "$SCRIPT_DIR/dirA${SEP}$SCRIPT_DIR/dirB" 2>&1 | tr -d '\r')
expected="lib
$(normalize_path "$SCRIPT_DIR/dirA")
$(normalize_path "$SCRIPT_DIR/dirB")"
if [ "$out" = "$expected" ]; then
    echo "  [PASS] ae lib-path introspection"
    pass=$((pass + 1))
else
    echo "  [FAIL] ae lib-path introspection"
    echo "    expected:"
    printf '      %s\n' "$expected"
    echo "    got:"
    printf '      %s\n' "$out"
    fail=$((fail + 1))
fi

# Cases 8 and 9 (#3504): a program that imports a module of its own lib/
# and one from elsewhere names only the other. --lib, and AETHER_LIB_DIR,
# add to the search path after the project's lib/; they used to replace
# it, and `greet` was not found.
mkdir -p "$TMPDIR/proj/lib/greet" "$TMPDIR/proj/other"
cat > "$TMPDIR/proj/lib/greet/module.ae" <<EOF
exports(hi)

hi() -> string {
    return "hi"
}
EOF
cat > "$TMPDIR/proj/main.ae" <<EOF
import greet
import foo
main() { println("\${greet.hi()} foo=\${foo.foo_origin()}") }
EOF
run_case "--lib adds after the project's lib/" \
    "hi foo=A" \
    sh -c "cd '$TMPDIR/proj' && '$AE' run main.ae --lib '$SCRIPT_DIR/dirA'"
run_case "AETHER_LIB_DIR adds after the project's lib/" \
    "hi foo=A" \
    sh -c "cd '$TMPDIR/proj' && AETHER_LIB_DIR='$SCRIPT_DIR/dirA' '$AE' run main.ae"

echo
echo "run_lib_path: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
