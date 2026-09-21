#!/bin/sh
# An extra C source whose path contains a space: `--extra "my dir/shim.c"`
# and `extra_sources = ["my dir/shim.c"]`.
#
# The extras travelled as one space-separated string that went onto the C
# compiler command verbatim, so a path with a space became two arguments
# and the build failed with "cc1: fatal error: dir/shim.c: No such file".
# A project under `C:\Users\First Last\` hit it on every extra. An entry
# with a space is now stored quoted (extras_append) and every reader — the
# compiler command, the cache key, the clobber check — reads entries back
# through one tokenizer (extras_next).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] extra_sources_spaces: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"
export AETHER_CACHE_DIR="$tmp/cache"

mkdir -p "$tmp/proj/sp dir"
printf 'int shim_value(void) { return 7; }\n' > "$tmp/proj/sp dir/shim.c"
cat > "$tmp/proj/ex.ae" <<'AE'
extern shim_value() -> int
main() { println("${shim_value()}") }
AE

# 1. --extra with a space, on run and build
got="$(cd "$tmp/proj" && "$AE" run --extra "sp dir/shim.c" ex.ae 2>&1 | tail -1)"
[ "$got" = "7" ] || { echo "  [FAIL] extra_sources_spaces: ae run --extra 'sp dir/shim.c' (got '$got')"; fail=1; }
if (cd "$tmp/proj" && "$AE" build ex.ae --extra "sp dir/shim.c" -o "$tmp/ex1" >/dev/null 2>&1) && [ -x "$tmp/ex1" ]; then
    got="$("$tmp/ex1")"
    [ "$got" = "7" ] || { echo "  [FAIL] extra_sources_spaces: built program printed '$got'"; fail=1; }
else
    echo "  [FAIL] extra_sources_spaces: ae build --extra 'sp dir/shim.c' failed"
    fail=1
fi

# 2. the edited shim invalidates the cache (the key reads the entry back)
printf 'int shim_value(void) { return 8; }\n' > "$tmp/proj/sp dir/shim.c"
got="$(cd "$tmp/proj" && "$AE" run --extra "sp dir/shim.c" ex.ae 2>&1 | tail -1)"
[ "$got" = "8" ] || { echo "  [FAIL] extra_sources_spaces: cached run served the old shim (got '$got')"; fail=1; }

# 3. extra_sources in aether.toml
cat > "$tmp/proj/aether.toml" <<'EOF'
[[bin]]
name = "ex"
path = "ex.ae"
extra_sources = ["sp dir/shim.c"]
EOF
if (cd "$tmp/proj" && "$AE" build ex.ae -o "$tmp/ex2" >/dev/null 2>&1) && [ -x "$tmp/ex2" ]; then
    got="$("$tmp/ex2")"
    [ "$got" = "8" ] || { echo "  [FAIL] extra_sources_spaces: toml build printed '$got'"; fail=1; }
else
    echo "  [FAIL] extra_sources_spaces: ae build with extra_sources = [\"sp dir/shim.c\"] failed"
    fail=1
fi
rm -f "$tmp/proj/aether.toml"

# 4. the clobber check still sees the entry: -o naming the shim's own
#    directory + basename would write the generated C over it
if (cd "$tmp/proj" && "$AE" build ex.ae --extra "sp dir/shim.c" -o "sp dir/shim" >"$tmp/clobber.log" 2>&1); then
    echo "  [FAIL] extra_sources_spaces: a build that writes its C over 'sp dir/shim.c' was not refused"
    fail=1
elif ! grep -q "would be written over an --extra input" "$tmp/clobber.log"; then
    echo "  [FAIL] extra_sources_spaces: the clobber refusal did not name the --extra input"
    head -3 "$tmp/clobber.log" | sed 's/^/        /'
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] extra_sources_spaces: --extra and extra_sources paths with a space build, invalidate the cache and are clobber-checked"
fi
exit $fail
