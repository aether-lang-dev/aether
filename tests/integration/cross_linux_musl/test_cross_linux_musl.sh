#!/bin/sh
# `ae build --target=*-linux-musl` — the static, floor-free Linux targets.
#
# A glibc build carries a GLIBC symbol version from whatever machine produced
# it and refuses to start on an older distro. zig bundles musl and links it
# statically by default, so the musl artifact has no libc version floor and
# runs on any Linux of the same architecture. That difference is the whole
# point of the target existing, so this asserts it rather than merely
# asserting the build succeeds.
#
# Asserts:
#   - x86_64/aarch64-linux-musl are accepted, and the amd64/arm64 spellings
#   - the artifact is STATICALLY linked and names no GLIBC_ version symbol
#   - the gnu targets are unaffected (still dynamic) — the musl rows must not
#     have changed what -linux means
#   - on an x86_64 Linux host the musl binary actually runs
#
# NB on cost: each executable build recompiles the runtime and stdlib for the
# target, so this does the minimum: two musl builds (one per arch), one gnu
# build for contrast, and cheap name-only checks for the aliases.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] cross_linux_musl: ae not built"; exit 0; }
command -v zig >/dev/null 2>&1 || { echo "  [SKIP] cross_linux_musl: no zig"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP" || :; return 0; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

printf 'main() {\n    println("musl-ok")\n    return 0\n}\n' > "$TMP/hello.ae"

# --- x86_64 musl: the full set of claims -----------------------------------
"$AE" build "$TMP/hello.ae" --target=x86_64-linux-musl -o "$TMP/x64" \
    >"$TMP/b1.log" 2>&1 || { sed -n '1,10p' "$TMP/b1.log"; fail "x86_64-linux-musl did not build"; }

desc=$(file -b "$TMP/x64" 2>/dev/null || echo "")
case "$desc" in
    *"statically linked"*) ;;
    *) fail "x86_64-linux-musl is not statically linked: $desc" ;;
esac
case "$desc" in
    *x86-64*) ;;
    *) fail "x86_64-linux-musl produced the wrong architecture: $desc" ;;
esac

# The floor itself: a glibc build references versioned GLIBC_ symbols, a musl
# one must reference none. This is what "runs on any Linux" reduces to.
if strings "$TMP/x64" 2>/dev/null | grep -q 'GLIBC_'; then
    fail "musl artifact names a GLIBC_ version symbol — it would carry a floor"
fi

# --- the gnu target must be unchanged --------------------------------------
"$AE" build "$TMP/hello.ae" --target=x86_64-linux -o "$TMP/gnu" \
    >"$TMP/b2.log" 2>&1 || { sed -n '1,10p' "$TMP/b2.log"; fail "x86_64-linux did not build"; }
case "$(file -b "$TMP/gnu" 2>/dev/null)" in
    *"dynamically linked"*) ;;
    *) fail "x86_64-linux should still be dynamically linked (musl rows changed it)" ;;
esac

# --- aarch64 musl ----------------------------------------------------------
"$AE" build "$TMP/hello.ae" --target=aarch64-linux-musl -o "$TMP/a64" \
    >"$TMP/b3.log" 2>&1 || { sed -n '1,10p' "$TMP/b3.log"; fail "aarch64-linux-musl did not build"; }
desc=$(file -b "$TMP/a64" 2>/dev/null || echo "")
case "$desc" in
    *"statically linked"*) ;;
    *) fail "aarch64-linux-musl is not statically linked: $desc" ;;
esac
case "$desc" in
    *aarch64*) ;;
    *) fail "aarch64-linux-musl produced the wrong architecture: $desc" ;;
esac

# --- spelling aliases ------------------------------------------------------
# Name recognition only: --emit=csrc never invokes the toolchain, so this
# costs nothing while still failing if an alias is missing from the map.
for alias in amd64-linux-musl arm64-linux-musl; do
    "$AE" build "$TMP/hello.ae" --target="$alias" --emit=csrc -o "$TMP/alias.c" \
        >"$TMP/b4.log" 2>&1 || { sed -n '1,6p' "$TMP/b4.log"; fail "alias $alias not recognised"; }
done

# --- it runs, where the host can run it ------------------------------------
if [ "$(uname -s)" = "Linux" ] && [ "$(uname -m)" = "x86_64" ]; then
    out=$("$TMP/x64" 2>&1) || fail "the musl binary did not run"
    [ "$out" = "musl-ok" ] || fail "musl binary printed '$out', want 'musl-ok'"
    echo "  [PASS] cross_linux_musl: static, no GLIBC floor, and runs here"
else
    echo "  [PASS] cross_linux_musl: static and no GLIBC floor (not run: host is $(uname -s)/$(uname -m))"
fi
