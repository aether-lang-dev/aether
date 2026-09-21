#!/bin/sh
# #2125: a module ships its own C with `@source("lanes.c")`.
#
# A module could declare its link flags (`@link`, #1549) but not its own C
# source, so every program importing a module with a C kernel had to pass
# `--extra path/to/lanes.c` or list it under every `[[bin]]`. Now the
# directive names a file relative to the module's directory; the compiler
# checks it exists, records it as a dependency (a warm cached `ae run`
# rebuilds when the C changes), and lists it as a `// aether-source:` line
# after the link header; `ae run` / `ae build` compile it into the program.
# A `-D`-dropped import drops its sources with its link flags.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] source_directive: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

# A private cache so the warm-hit case below is about THIS test's key.
export AETHER_CACHE_DIR="$tmp/cache"
export AETHER_HOME="$ROOT"

# The module directory has a space in its name: one path per header line.
mkdir -p "$tmp/my lib/lanes"
cat > "$tmp/my lib/lanes/module.ae" <<'AE'
@source("lanes.c")
exports(add4)
extern lanes_add4(a: int, b: int) -> int
add4(a: int, b: int) -> int { return lanes_add4(a, b) }
AE
cat > "$tmp/my lib/lanes/lanes.c" <<'C'
int lanes_add4(int a, int b) { return a + b + 4; }
C
mkdir -p "$tmp/proj"
cat > "$tmp/proj/main.ae" <<'AE'
import lanes
main() { println("${lanes.add4(1, 2)}") }
AE

# 1. the C is compiled in with no --extra and no aether.toml
got="$(cd "$tmp/proj" && "$AE" run --lib "$tmp/my lib" main.ae 2>&1 | tail -1)"
if [ "$got" != "7" ]; then
    echo "  [FAIL] source_directive: ae run did not compile the module's C (got '$got')"
    fail=1
fi

# 2. the header names the resolved file, one per line, after the link line
(cd "$tmp/proj" && "$AETHERC" --lib "$tmp/my lib" main.ae out.c >/dev/null 2>&1)
if ! head -3 "$tmp/proj/out.c" | grep -q "^// aether-source: .*my lib/lanes/lanes.c$"; then
    echo "  [FAIL] source_directive: generated C lacks the aether-source line"
    head -3 "$tmp/proj/out.c" | sed 's/^/        /'
    fail=1
fi

# 3. editing the C invalidates the warm cache
printf 'int lanes_add4(int a, int b) { return a + b + 5; }\n' > "$tmp/my lib/lanes/lanes.c"
got="$(cd "$tmp/proj" && "$AE" run --lib "$tmp/my lib" main.ae 2>&1 | tail -1)"
if [ "$got" != "8" ]; then
    echo "  [FAIL] source_directive: a cached run served the old C (got '$got', want 8)"
    fail=1
fi

# 4. ae build, and a -D-dropped import drops the source with it
cat > "$tmp/proj/cond.ae" <<'AE'
when defined(WITH_LANES) {
    import lanes
}
main() {
    when defined(WITH_LANES) { println("lanes ${lanes.add4(1, 2)}") }
    println("plain")
}
AE
got="$(cd "$tmp/proj" && "$AE" build --lib "$tmp/my lib" -D WITH_LANES cond.ae -o "$tmp/on" >/dev/null 2>&1 && "$tmp/on" 2>&1 | tr '\n' ' ')"
if [ "$got" != "lanes 8 plain " ]; then
    echo "  [FAIL] source_directive: ae build with -D WITH_LANES (got '$got')"
    fail=1
fi
(cd "$tmp/proj" && "$AETHERC" --lib "$tmp/my lib" cond.ae off.c >/dev/null 2>&1)
if head -3 "$tmp/proj/off.c" | grep -q "aether-source"; then
    echo "  [FAIL] source_directive: a dropped import still contributed its @source"
    fail=1
fi

# 5. a file that does not exist is an error at the directive, not gcc's
mkdir -p "$tmp/bad lib/lanes"
cp "$tmp/my lib/lanes/lanes.c" "$tmp/bad lib/lanes/lanes.c"
cat > "$tmp/bad lib/lanes/module.ae" <<'AE'
@source("lanes.c")
@source("missing.c")
exports(add4)
extern lanes_add4(a: int, b: int) -> int
add4(a: int, b: int) -> int { return lanes_add4(a, b) }
AE
out="$(cd "$tmp/proj" && "$AE" run --lib "$tmp/bad lib" main.ae 2>&1)"
rc_line="$(printf '%s\n' "$out" | grep -c '@source("missing.c"): no such file')"
if [ "$rc_line" != "1" ] || ! printf '%s\n' "$out" | grep -q "module.ae:2:1"; then
    echo "  [FAIL] source_directive: missing @source file not reported at the directive"
    printf '%s\n' "$out" | head -4 | sed 's/^/        /'
    fail=1
fi

# 6. @source in the entry file itself resolves beside it
cat > "$tmp/proj/shim.c" <<'C'
int shim_seven(void) { return 7; }
C
cat > "$tmp/proj/self.ae" <<'AE'
@source("shim.c")
extern shim_seven() -> int
main() { println("${shim_seven()}") }
AE
got="$(cd "$tmp" && "$AE" run proj/self.ae 2>&1 | tail -1)"
if [ "$got" != "7" ]; then
    echo "  [FAIL] source_directive: @source in the entry file (got '$got')"
    fail=1
fi

# 7. a cross build compiles the module's C as well (the zig path has its own
#    compile line; it linked without the module's symbols)
cross="cross build: zig not on PATH, skipped"
if command -v zig >/dev/null 2>&1; then
    if (cd "$tmp/proj" && "$AE" build --lib "$tmp/my lib" main.ae --target=x86_64-linux-musl -o "$tmp/cross_main" >"$tmp/cross.log" 2>&1) && [ -f "$tmp/cross_main" ]; then
        cross="cross build links the module C"
    else
        echo "  [FAIL] source_directive: ae build --target=x86_64-linux-musl did not link the module's C"
        grep -i "undefined\|error" "$tmp/cross.log" | head -3 | sed 's/^/        /'
        fail=1
    fi
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] source_directive: module C compiled in, header line, cache invalidation, -D drop, missing-file error, entry-file @source; $cross"
fi
exit $fail
