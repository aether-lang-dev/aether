#!/bin/sh
# `ae run`, `ae check` and `ae test` take `-D NAME` / `-DNAME` and read the
# project's `[build] defines`, like `ae build`; `ae build` reads them after
# the walk-up to aether.toml, so a subdirectory sees them too.
#
# Before: `ae run -D X app.ae` reported "File not found: X", `ae run app.ae
# -DX` skipped the flag without a word, and neither `ae run` nor `ae check`
# nor `ae test` read `[build] defines` — so a `when defined(TEST_SERVER)`
# region the manifest declares was in the binary `ae build` produced and
# silently absent from `ae run` of the same file: no error, a different
# program. `ae build sub/app.ae` from `sub/` had the same gap, because the
# manifest was read before the chdir to the project root.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] run_build_symbols: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"
export AETHER_CACHE_DIR="$tmp/cache"

mkdir -p "$tmp/proj/src"
cat > "$tmp/proj/src/app.ae" <<'AE'
main() {
    when defined(FEATURE) { println("feature") }
    println("base")
}
AE
# Type-checks only WITHOUT the symbol: the region names a function that
# does not exist, so `ae check -D FEATURE` must fail and plain `ae check`
# must pass — the symbol reached aetherc, not just the driver.
cat > "$tmp/proj/src/gated.ae" <<'AE'
main() {
    when defined(FEATURE) { println(no_such_function()) }
    println("ok")
}
AE

run_out() { (cd "$tmp/proj" && "$AE" run "$@" 2>&1 | grep -v '^\[' | tr '\n' ' ') ; }

# 1. -D NAME and -DNAME on ae run; absent → base only
got="$(run_out -D FEATURE src/app.ae)"
[ "$got" = "feature base " ] || { echo "  [FAIL] run_build_symbols: ae run -D FEATURE (got '$got')"; fail=1; }
got="$(run_out src/app.ae -DFEATURE)"
[ "$got" = "feature base " ] || { echo "  [FAIL] run_build_symbols: ae run -DFEATURE after the file (got '$got')"; fail=1; }
got="$(run_out src/app.ae)"
[ "$got" = "base " ] || { echo "  [FAIL] run_build_symbols: ae run without the symbol (got '$got')"; fail=1; }

# 2. ae check: the symbol reaches aetherc
if (cd "$tmp/proj" && "$AE" check -D FEATURE src/gated.ae >/dev/null 2>&1); then
    echo "  [FAIL] run_build_symbols: ae check -D FEATURE passed a region that cannot type-check"
    fail=1
fi
if ! (cd "$tmp/proj" && "$AE" check src/gated.ae >/dev/null 2>&1); then
    echo "  [FAIL] run_build_symbols: ae check without the symbol failed"
    fail=1
fi

# 3. `-D` with no name is an error, not a file lookup
out="$(cd "$tmp/proj" && "$AE" run src/app.ae -D 2>&1)"
if ! printf '%s\n' "$out" | grep -q "needs a symbol name"; then
    echo "  [FAIL] run_build_symbols: bare -D not reported (got '$out')"
    fail=1
fi

# 4. [build] defines from aether.toml: run, check, and build from a subdirectory
cat > "$tmp/proj/aether.toml" <<'EOF'
[project]
name = "symbols"
version = "0.0.0"

[build]
defines = "FEATURE"
EOF
got="$(run_out src/app.ae)"
[ "$got" = "feature base " ] || { echo "  [FAIL] run_build_symbols: ae run ignored [build] defines (got '$got')"; fail=1; }
if (cd "$tmp/proj" && "$AE" check src/gated.ae >/dev/null 2>&1); then
    echo "  [FAIL] run_build_symbols: ae check ignored [build] defines"
    fail=1
fi
if (cd "$tmp/proj/src" && "$AE" build app.ae -o "$tmp/sub_app" >/dev/null 2>&1) && [ -x "$tmp/sub_app" ]; then
    got="$("$tmp/sub_app" | tr '\n' ' ')"
    [ "$got" = "feature base " ] || { echo "  [FAIL] run_build_symbols: ae build from a subdirectory missed [build] defines (got '$got')"; fail=1; }
else
    echo "  [FAIL] run_build_symbols: ae build from the subdirectory failed"
    fail=1
fi

# 5. ae test: -D and the manifest's symbols reach the test build (the test
#    program exits 1 unless both are present; ae test hides its output)
mkdir -p "$tmp/proj/tests"
cat > "$tmp/proj/tests/test_feature.ae" <<'AE'
main() {
    seen = 0
    when defined(FEATURE) { seen = seen + 1 }
    when defined(EXTRA) { seen = seen + 10 }
    if seen != 11 { println("seen ${seen}"); exit(1) }
}
AE
if ! (cd "$tmp/proj" && "$AE" test -D EXTRA tests/test_feature.ae >"$tmp/test.log" 2>&1); then
    echo "  [FAIL] run_build_symbols: ae test -D EXTRA with [build] defines did not see both symbols"
    head -6 "$tmp/test.log" | sed 's/^/        /'
    fail=1
fi
if (cd "$tmp/proj" && "$AE" test tests/test_feature.ae >/dev/null 2>&1); then
    echo "  [FAIL] run_build_symbols: ae test without -D EXTRA passed a test that needs it"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] run_build_symbols: -D on run/check/test, [build] defines everywhere, build from a subdirectory"
fi
exit $fail
