#!/bin/sh
# #2148: `ae run`, `ae check` and `ae test` read the project's aether.toml
# from a subdirectory, without changing the program's cwd.
#
# `ae build app.ae` from src/ found the manifest by walking up and
# chdir-ing (#280); `ae run app.ae`, `ae check app.ae` and `ae test` from
# the same src/ read aether.toml in the cwd only, found none, and compiled
# with no [build] defines, no [[bin]] extra_sources and no [dependencies]
# roots — the same file, two programs, no diagnostic. They now locate the
# manifest the way the walk-up does (a manifest in the cwd wins, the walk
# stops at .git) and read it where it is; a path the manifest states
# relative to itself (a [[bin]] path, an extra_sources entry, src/main.ae)
# is resolved from the invocation directory, and the program still starts
# in that directory — `ae run` must not move a script's ./data.txt.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] manifest_from_subdirectory: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"
export AETHER_CACHE_DIR="$tmp/cache"

mkdir -p "$tmp/proj/src" "$tmp/proj/tests"
cat > "$tmp/proj/aether.toml" <<'EOF'
[project]
name = "subdir"
version = "0.0.0"

[build]
defines = "FEATURE"

[[bin]]
name = "app"
path = "src/app.ae"
extra_sources = ["src/shim.c"]
EOF
printf 'int shim_seven(void) { return 7; }\n' > "$tmp/proj/src/shim.c"
cat > "$tmp/proj/src/app.ae" <<'AE'
import std.fs
extern shim_seven() -> int
main() {
    _ = fs.write("here.txt", "x")
    when defined(FEATURE) { println("feature") }
    println("shim ${shim_seven()}")
}
AE
cat > "$tmp/proj/src/main.ae" <<'AE'
main() {
    when defined(FEATURE) { println("main feature") }
    println("main")
}
AE
cat > "$tmp/proj/tests/test_feature.ae" <<'AE'
main() {
    seen = 0
    when defined(FEATURE) { seen = 1 }
    if seen != 1 { println("FEATURE not seen"); exit(1) }
}
AE

# 1. ae run from src/: defines and extra_sources apply; the program's cwd is src/
got="$(cd "$tmp/proj/src" && "$AE" run app.ae 2>&1 | grep -v '^\[' | tr '\n' ' ')"
if [ "$got" != "feature shim 7 " ]; then
    echo "  [FAIL] manifest_from_subdirectory: ae run from src/ (got '$got')"
    fail=1
fi
if [ ! -f "$tmp/proj/src/here.txt" ] || [ -f "$tmp/proj/here.txt" ]; then
    echo "  [FAIL] manifest_from_subdirectory: the program did not start in src/ (here.txt landed elsewhere)"
    fail=1
fi

# 2. ae check from src/: the symbol reaches aetherc (a region that cannot
#    type-check is checked only with the manifest's define)
cat > "$tmp/proj/src/gated.ae" <<'AE'
main() {
    when defined(FEATURE) { println(no_such_function()) }
    println("ok")
}
AE
if (cd "$tmp/proj/src" && "$AE" check gated.ae >/dev/null 2>&1); then
    echo "  [FAIL] manifest_from_subdirectory: ae check from src/ ignored [build] defines"
    fail=1
fi

# 3. project mode from src/: the manifest's src/main.ae
got="$(cd "$tmp/proj/src" && "$AE" run 2>&1 | grep -v '^\[' | tr '\n' ' ')"
if [ "$got" != "main feature main " ]; then
    echo "  [FAIL] manifest_from_subdirectory: project-mode ae run from src/ (got '$got')"
    fail=1
fi

# 4. ae test from a subdirectory sees the manifest's defines
if ! (cd "$tmp/proj/src" && "$AE" test ../tests/test_feature.ae >"$tmp/test.log" 2>&1); then
    echo "  [FAIL] manifest_from_subdirectory: ae test from src/ did not see [build] defines"
    head -5 "$tmp/test.log" | sed 's/^/        /'
    fail=1
fi

# 5. a manifest in the cwd wins over an ancestor's
mkdir -p "$tmp/proj/nested"
cat > "$tmp/proj/nested/aether.toml" <<'EOF'
[build]
defines = "INNER"
EOF
cat > "$tmp/proj/nested/n.ae" <<'AE'
main() {
    when defined(INNER) { println("inner") }
    when defined(FEATURE) { println("outer") }
    println("done")
}
AE
got="$(cd "$tmp/proj/nested" && "$AE" run n.ae 2>&1 | grep -v '^\[' | tr '\n' ' ')"
if [ "$got" != "inner done " ]; then
    echo "  [FAIL] manifest_from_subdirectory: a manifest in the cwd did not win (got '$got')"
    fail=1
fi

# 6. the walk stops at a repository boundary: a checkout with no manifest
#    does not adopt an ancestor's
mkdir -p "$tmp/proj/repo/.git" "$tmp/proj/repo/deep"
cp "$tmp/proj/nested/n.ae" "$tmp/proj/repo/deep/n.ae"
got="$(cd "$tmp/proj/repo/deep" && "$AE" run n.ae 2>&1 | grep -v '^\[' | tr '\n' ' ')"
if [ "$got" != "done " ]; then
    echo "  [FAIL] manifest_from_subdirectory: the walk crossed a .git boundary (got '$got')"
    fail=1
fi

# 7. a nested subdirectory: the [[bin]] match and extra_sources still apply
#    (the sub-path below the manifest is spelled with '/', whatever getcwd
#    returned), and a [patch] path is read relative to the manifest
mkdir -p "$tmp/proj/src/net" "$tmp/proj/vendor/dep.local/mods/greeter"
printf 'int net_nine(void) { return 9; }\n' > "$tmp/proj/src/net/shim.c"
cat > "$tmp/proj/src/net/n.ae" <<'AE'
import greeter
extern net_nine() -> int
main() { println("${greeter.hi()} ${net_nine()}") }
AE
printf '[package]\nmodules = "mods/greeter"\n' > "$tmp/proj/vendor/dep.local/aether.toml"
printf 'exports(hi)\nhi() -> string { return "dep" }\n' > "$tmp/proj/vendor/dep.local/mods/greeter/module.ae"
cat >> "$tmp/proj/aether.toml" <<'EOF'

[[bin]]
name = "n"
path = "src/net/n.ae"
extra_sources = ["src/net/shim.c"]

[dependencies]
"dep.local" = "0.0.0"

[patch]
"dep.local" = { path = "vendor/dep.local" }
EOF
got="$(cd "$tmp/proj/src/net" && "$AE" run n.ae 2>&1 | grep -v '^\[' | grep -v '^Overriding' | tr '\n' ' ')"
if [ "$got" != "dep 9 " ]; then
    echo "  [FAIL] manifest_from_subdirectory: nested subdirectory (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] manifest_from_subdirectory: run/check/test read the ancestor manifest from a subdirectory, program cwd unchanged, cwd manifest wins, walk stops at .git"
fi
exit $fail
