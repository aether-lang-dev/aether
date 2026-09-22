#!/bin/sh
# #1986: `@c_include("header.h")` — a header a module needs in the generated
# translation unit.
#
# `@c_import` hands prototype ownership to a header, but the header still had
# to be put in the TU by the consumer's cflags, which a module cannot set. A
# module declares it itself now: codegen emits the include before anything it
# declares, once per header however many modules ask for it, and emits the
# module's own directory as an `// aether-include:` line that `ae` turns into
# a `-I` flag, so the header resolves wherever the module lives.
#
# That is what makes a `static inline` accessor actually inline — the reason
# the directive exists (std.intarr / floatarr / longarr; see
# tests/integration/packed_array_inline_access).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] c_include_directive: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"

mkdir -p "$tmp/lib/hdrmod" "$tmp/proj"
cat > "$tmp/lib/hdrmod/api.h" <<'C'
#ifndef HDRMOD_API_H
#define HDRMOD_API_H
/* A static inline helper: it has no linkable symbol, so the only way a
 * caller can reach it is to have this header in the same translation unit. */
static inline int hdr_twice(int v) { return v * 2; }
#endif
C
cat > "$tmp/lib/hdrmod/module.ae" <<'AE'
@c_include("api.h")
exports(twice)
extern hdr_twice(v: int) -> int @c_import
twice(v: int) -> int { return hdr_twice(v) }
AE
cat > "$tmp/proj/main.ae" <<'AE'
import hdrmod
main() { println("${hdrmod.twice(21)}") }
AE

# 1. it builds and runs: the header is in the TU and the call inlines
got="$(cd "$tmp/proj" && "$AE" run --lib "$tmp/lib" main.ae 2>&1 | tail -1)"
if [ "$got" != "42" ]; then
    echo "  [FAIL] c_include_directive: a module's @c_include header did not reach the build (got '$got')"
    fail=1
fi

# 2. the generated C says it plainly: the include by the name the module
#    wrote (so the file stays portable) and the directory as a header line
(cd "$tmp/proj" && "$AETHERC" --lib "$tmp/lib" main.ae out.c >/dev/null 2>&1)
if ! grep -q '^#include "api.h"$' "$tmp/proj/out.c"; then
    echo "  [FAIL] c_include_directive: the #include is missing from the generated C"
    fail=1
fi
if [ "$(grep -c '^#include "api.h"$' "$tmp/proj/out.c")" != "1" ]; then
    echo "  [FAIL] c_include_directive: the header was included more than once"
    fail=1
fi
if ! grep -q '^// aether-include: .*hdrmod$' "$tmp/proj/out.c"; then
    echo "  [FAIL] c_include_directive: the module's directory was not reported for -I"
    head -5 "$tmp/proj/out.c" | sed 's/^/        /'
    fail=1
fi

# 3. AST-derived like @link and @source: an import dropped by a losing
#    `when defined(...)` takes its include with it
cat > "$tmp/proj/cond.ae" <<'AE'
when defined(WITH_HDR) {
    import hdrmod
}
main() {
    when defined(WITH_HDR) { println("${hdrmod.twice(21)}") }
    println("plain")
}
AE
(cd "$tmp/proj" && "$AETHERC" --lib "$tmp/lib" cond.ae off.c >/dev/null 2>&1)
if grep -q 'api.h' "$tmp/proj/off.c"; then
    echo "  [FAIL] c_include_directive: a dropped import still contributed its @c_include"
    fail=1
fi
got="$(cd "$tmp/proj" && "$AE" run --lib "$tmp/lib" -D WITH_HDR cond.ae 2>&1 | tr '\n' ' ')"
if [ "$got" != "42 plain " ]; then
    echo "  [FAIL] c_include_directive: the conditional build (got '$got')"
    fail=1
fi

# 3b. the -I reaches the C compiler as ONE argument, quotes removed. `ae`
#     spawns the compiler directly (no shell), so its own tokenizer is the
#     quoting rule: a quote is syntax wherever it appears in a token, not
#     only at its start. `-I"dir"` used to reach gcc with the quotes still
#     in it, and it looked for a directory of that literal name -- invisible
#     on Windows, where the child CRT re-parses the command line and strips
#     them, and a hard failure everywhere else. A directory with a space in
#     it needs both halves of that to be right.
mkdir -p "$tmp/li b/spacemod"
cp "$tmp/lib/hdrmod/api.h" "$tmp/li b/spacemod/api.h"
cat > "$tmp/li b/spacemod/module.ae" <<'AE'
@c_include("api.h")
exports(twice)
extern hdr_twice(v: int) -> int @c_import
twice(v: int) -> int { return hdr_twice(v) }
AE
printf 'import spacemod\nmain() { println("${spacemod.twice(21)}") }\n' > "$tmp/proj/spaced.ae"
got="$(cd "$tmp/proj" && "$AE" run --lib "$tmp/li b" spaced.ae 2>&1 | tail -1)"
if [ "$got" != "42" ]; then
    echo "  [FAIL] c_include_directive: a module directory with a space did not reach -I (got '$got')"
    fail=1
fi

# 4. the cross path compiles the same generated C, so it needs the same -I
cross="cross build: zig not on PATH, skipped"
if command -v zig >/dev/null 2>&1; then
    if (cd "$tmp/proj" && "$AE" build --lib "$tmp/lib" main.ae --target=x86_64-linux-musl -o "$tmp/cross_main" >"$tmp/cross.log" 2>&1) && [ -f "$tmp/cross_main" ]; then
        cross="the cross path finds it too"
    else
        echo "  [FAIL] c_include_directive: ae build --target did not find the module's header"
        grep -i "error\|not found" "$tmp/cross.log" | head -3 | sed 's/^/        /'
        fail=1
    fi
fi

# 5. a header that does not exist fails at the C compiler naming the header,
#    not with something further downstream
cat > "$tmp/lib/hdrmod/bad.ae" <<'AE'
@c_include("no_such_header.h")
exports(nothing)
nothing() -> int { return 0 }
AE
mkdir -p "$tmp/lib/badmod"
mv "$tmp/lib/hdrmod/bad.ae" "$tmp/lib/badmod/module.ae"
printf 'import badmod\nmain() { println("${badmod.nothing()}") }\n' > "$tmp/proj/bad.ae"
out="$(cd "$tmp/proj" && "$AE" run --lib "$tmp/lib" bad.ae 2>&1)"
if ! printf '%s\n' "$out" | grep -q "no_such_header.h"; then
    echo "  [FAIL] c_include_directive: a missing header was not reported by name"
    printf '%s\n' "$out" | head -3 | sed 's/^/        /'
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] c_include_directive: a module's header reaches the TU (once), with its directory on the include path, and a dropped import takes it with; $cross"
fi
exit $fail
