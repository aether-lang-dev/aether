#!/bin/sh
# #1663 (ask: target-wasm-omits-share-aether-on-user-prefix): `ae build
# --target=wasm` must resolve runtime sources under <prefix>/share/aether on an
# INSTALLED tree.
#
# tc.root means two different things: the repo root in dev mode, the install
# PREFIX otherwise. Sources sit directly under it in the first case and under
# share/aether/ in the second. The native build path appended share/aether/
# itself; the wasm source and include lists used a bare tc.root, so an
# installed `ae` composed <prefix>/runtime/... and emcc reported every runtime
# file missing:
#
#   emcc: error: /home/user/.local/bin/../runtime/scheduler/aether_scheduler_coop.c:
#         No such file or directory
#
# A DEV TREE CANNOT REPRODUCE THIS — there tc.root IS the repo root and
# <root>/runtime is correct, which is why CI stayed green while every installed
# user was broken. So this test installs to a temp prefix and drives the
# installed binary. That is the whole point of it; running it against
# ./build/ae would assert nothing.
#
# emcc is NOT required. A stub on PATH answers --version and then checks that
# every .c it was handed exists, which is precisely the property at issue —
# the generated C was always fine, only the source list was wrong.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] wasm_installed_prefix_paths on Windows (POSIX install layout)"
        exit 0 ;;
esac

command -v make >/dev/null 2>&1 || { echo "  [SKIP] no make on PATH"; exit 0; }

TMP="${TMPDIR:-/tmp}/ae_wasmprefix_$$"
mkdir -p "$TMP/bin"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "  FAIL: $1"; exit 1; }

# A stub emcc that verifies its inputs instead of compiling them.
cat > "$TMP/bin/emcc" <<'STUB'
#!/bin/sh
case "$1" in --version) echo "emcc (stub) 0.0"; exit 0 ;; esac
missing=0; total=0
for a in "$@"; do
  case "$a" in
    *.c) total=$((total+1)); [ -f "$a" ] || { missing=$((missing+1)); echo "MISSING: $a"; } ;;
  esac
done
echo "SOURCES: $total MISSING: $missing"
exit 1
STUB
chmod +x "$TMP/bin/emcc"

# Install to a throwaway prefix. This is the only way to exercise the
# installed-tree path resolution.
if ! (cd "$ROOT" && make install PREFIX="$TMP/prefix" >"$TMP/install.log" 2>&1); then
    tail -15 "$TMP/install.log"
    fail "make install PREFIX=... did not succeed"
fi

[ -f "$TMP/prefix/share/aether/runtime/scheduler/aether_scheduler_coop.c" ] \
    || fail "the install did not place runtime sources under share/aether (layout changed?)"

printf 'main() { println("hi") }\n' > "$TMP/hi.ae"

out="$(cd "$TMP" && PATH="$TMP/bin:$PATH" "$TMP/prefix/bin/ae" build --target=wasm hi.ae -o "$TMP/hi" 2>&1 || true)"

echo "$out" | grep -q 'SOURCES:' \
    || { echo "$out" | tail -10 | sed 's/^/    /';
         fail "the wasm build never reached emcc — cannot judge the source list"; }

if echo "$out" | grep -q 'MISSING: /'; then
    echo "$out" | grep 'MISSING: /' | head -4 | sed 's/^/    /'
    fail "wasm source paths do not resolve on an installed tree (missing share/aether)"
fi

echo "$out" | grep -q 'MISSING: 0' \
    || { echo "$out" | grep 'SOURCES:' | sed 's/^/    /';
         fail "emcc was handed source files that do not exist"; }

count="$(echo "$out" | sed -n 's/.*SOURCES: \([0-9]*\).*/\1/p' | head -1)"
[ "${count:-0}" -gt 10 ] \
    || fail "only ${count:-0} sources passed to emcc — the list looks truncated"

echo "  PASS: wasm_installed_prefix_paths: $count runtime sources resolve under <prefix>/share/aether"
