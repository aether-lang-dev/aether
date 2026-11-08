#!/bin/sh
# A string literal holding a NUL goes to C as its bytes, typed `char*` as a
# C string literal is (#3039). Its bytes are a `static const char[]` beside
# the pinned string (#2520), and passing that array to an extern's `ptr`
# discarded the qualifier: -Wdiscarded-qualifiers, a build failure under
# -Werror (ae3d's builds, CI). The generated C must compile with -Wall
# -Werror, and C must still see every byte, the NUL included.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] nul_literal_to_c: ae not built"; exit 0; }
cc_cmd=${CC:-cc}
command -v "${cc_cmd%% *}" >/dev/null 2>&1 || { echo "  [SKIP] nul_literal_to_c: no C compiler (${CC:-cc})"; exit 0; }

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) EXE=".exe" ;;
    *) EXE="" ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# C reads the four bytes a, NUL, b, c.
cat > "$TMP/probe.c" <<'C'
int probe_len(void* p) {
    const char* s = (const char*)p;
    return (s[0] == 'a' && s[1] == 0 && s[2] == 'b' && s[3] == 'c') ? 4 : -1;
}
C

cat > "$TMP/nul.ae" <<'AE'
extern probe_len(p: ptr) -> int

// A ptr-typed result handed a literal: C reads its bytes too.
raw() -> ptr {
    return "a\0bc"
}

main() {
    println("${probe_len("a\0bc")} ${probe_len(raw())}")
    println("x\0y")
}
AE

cd "$TMP" || exit 1
"$AETHERC" nul.ae nul.c >aetherc.log 2>&1 || {
    echo "  [FAIL] nul_literal_to_c: aetherc refused nul.ae"; sed 's/^/    /' aetherc.log; exit 1; }
# shellcheck disable=SC2046
if ! ${CC:-cc} -fsyntax-only -Wall -Werror $(AETHER_HOME="$ROOT" "$AE" cflags --cflags) nul.c >gcc.log 2>&1; then
    echo "  [FAIL] nul_literal_to_c: the generated C does not compile with -Wall -Werror"
    sed 's/^/    /' gcc.log | head -12
    exit 1
fi
AETHER_HOME="$ROOT" "$AE" build nul.ae --extra probe.c -o nul >build.log 2>&1 || {
    echo "  [FAIL] nul_literal_to_c: ae build nul.ae"; sed 's/^/    /' build.log | head -12; exit 1; }
out="$("./nul$EXE" 2>&1 | head -1 | tr -d '\r')"
if [ "$out" != "4 4" ]; then
    echo "  [FAIL] nul_literal_to_c: C read \"$out\", want \"4 4\""
    exit 1
fi
echo "  [PASS] nul_literal_to_c: a literal with a NUL reaches C's ptr as char*, every byte intact, under -Werror"
