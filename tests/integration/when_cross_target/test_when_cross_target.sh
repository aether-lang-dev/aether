#!/bin/sh
# `when target.os` / `when target.arch` choose the arm for the machine a build
# is FOR, not the machine compiling it. `ae build --target=<triple>` passes
# --target-os / --target-arch to aetherc (tools/ae_cross.c
# cross_target_os_arch); without them every cross build saw the host, so a
# Linux machine building for Windows took the `target.os == "linux"` arm.
#
# --emit=csrc never invokes a toolchain, so this needs no zig: it reads which
# arms survived into the generated C.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT" && AETHERC="$AETHERC$EXE_EXT"
[ -x "$AE" ] || { echo "  [SKIP] when_cross_target: ae not built"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || :' EXIT
pass=0
fail=0
ok() { echo "  [PASS] when_cross_target: $1"; pass=$((pass + 1)); }
bad() { echo "  [FAIL] when_cross_target: $1"; fail=$((fail + 1)); }

# target  expected os  expected arch
check() {
    t="$1"; want_os="$2"; want_arch="$3"
    out="$tmp/$t.c"
    if ! (cd "$tmp" && "$AE" build "$SCRIPT_DIR/probe.ae" --target="$t" --emit=csrc -o "$out" > "$tmp/$t.log" 2>&1); then
        bad "$t: ae build --emit=csrc failed"
        sed 's/^/        /' "$tmp/$t.log" | head -6
        return
    fi
    c="$out"
    [ -f "$c" ] || c="$out.c"
    os=$(grep -oE 'TARGET_OS=[a-z0-9_]+' "$c" | sort -u | tr '\n' ' ')
    arch=$(grep -oE 'TARGET_ARCH=[a-z0-9_]+' "$c" | sort -u | tr '\n' ' ')
    if [ "$os" = "TARGET_OS=$want_os " ] && [ "$arch" = "TARGET_ARCH=$want_arch " ]; then
        ok "$t takes the $want_os / $want_arch arms"
    else
        bad "$t: want TARGET_OS=$want_os TARGET_ARCH=$want_arch, the C has: $os$arch"
    fi
}

check x86_64-windows windows x86_64
check aarch64-macos darwin aarch64
check aarch64-linux-musl linux aarch64
check x86_64-freebsd freebsd x86_64
check wasm32-wasi wasm wasm
check arm64-linux-musl linux aarch64

# A name the compiler does not know is an error, not a silent else.
if "$AETHERC" --target-os=lunix "$SCRIPT_DIR/probe.ae" "$tmp/x.c" > "$tmp/bad.log" 2>&1; then
    bad "an unknown --target-os was accepted"
elif grep -q "unknown --target-os 'lunix'" "$tmp/bad.log"; then
    ok "an unknown --target-os is rejected by name"
else
    bad "an unknown --target-os failed without naming it"
    sed 's/^/        /' "$tmp/bad.log" | head -4
fi

echo ""
echo "when cross target: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
