#!/bin/sh
# `ae build --target=riscv64-linux-musl` and `loongarch64-linux-musl`.
#
# Both go through zig's bundled musl like the x86_64/aarch64 musl targets, so
# the claims are the same: the target name is accepted, the artifact is a
# static ELF for that machine, and it runs. "Runs" is checked under qemu-user
# where the host has it. The program is an actor exchange rather than a
# hello-world, so a run exercises the multicore scheduler's threads and
# atomics on the target, not just printing.
#
# QEMU before 8.1 cannot run LoongArch binaries from current toolchains at all
# (an illegal instruction before main, even for plain C), so the LoongArch run
# needs 8.1 or newer and is skipped, with a note, on older ones.
#
# Cost: two cold cross builds (about 40 s each on a fresh zig cache).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] cross_riscv64_loongarch64: ae not built"; exit 0; }
command -v zig >/dev/null 2>&1 || { echo "  [SKIP] cross_riscv64_loongarch64: no zig"; exit 0; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP" || :; return 0; }
trap cleanup EXIT
fail() { echo "  [FAIL] cross_riscv64_loongarch64: $1"; exit 1; }

cat > "$TMP/ping.ae" <<'AE'
message Ping { n: int }
message Done {}

actor Counter {
    state total = 0
    receive {
        Ping(n) -> { total = total + n }
        Done() -> { println("total ${total}") }
    }
}

main() {
    c = spawn(Counter())
    i = 1
    while i <= 100 {
        c ! Ping { n: i }
        i = i + 1
    }
    c ! Done {}
    wait_for_idle()
}
AE
WANT="total 5050"

qemu_at_least() {   # <qemu> <major> <minor>
    v=$("$1" --version 2>/dev/null | head -1 | sed -n 's/.*version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p')
    [ -n "$v" ] || return 1
    set -- $v "$2" "$3"
    [ "$1" -gt "$3" ] || { [ "$1" -eq "$3" ] && [ "$2" -ge "$4" ]; }
}

ran=""
skipped=""
check() {   # <target> <file(1) machine text> <qemu arch> <min qemu major> <min minor>
    t="$1"
    "$AE" build "$TMP/ping.ae" --target="$t" -o "$TMP/$t" > "$TMP/$t.log" 2>&1 ||
        { sed -n '1,10p' "$TMP/$t.log"; fail "$t did not build"; }
    desc=$(file -b "$TMP/$t" 2>/dev/null || echo "")
    case "$desc" in *"statically linked"*) ;; *) fail "$t is not statically linked: $desc" ;; esac
    case "$desc" in *"$2"*) ;; *) fail "$t produced the wrong machine: $desc" ;; esac
    # The first of qemu-<arch>-static / qemu-<arch> on PATH that is new enough:
    # a host can carry an old one under one name and a current one under the
    # other.
    q=""
    old=""
    for c in "qemu-$3-static" "qemu-$3"; do
        command -v "$c" > /dev/null 2>&1 || continue
        if qemu_at_least "$c" "$4" "$5"; then q="$c"; break; fi
        old="$c"
    done
    if [ -z "$q" ] && [ -z "$old" ]; then
        skipped="$skipped $t(no qemu-$3)"
    elif [ -z "$q" ]; then
        skipped="$skipped $t($old older than $4.$5)"
    else
        out=$("$q" "$TMP/$t" 2>&1) || fail "$t did not run under $q: $out"
        [ "$out" = "$WANT" ] || fail "$t printed '$out' under $q, want '$WANT'"
        ran="$ran $t"
    fi
}

check riscv64-linux-musl "RISC-V" riscv64 0 0
check loongarch64-linux-musl "LoongArch" loongarch64 8 1

# The Go/Debian spelling. --emit=csrc never invokes the toolchain, so this
# costs nothing while still failing if the alias is missing from the map.
"$AE" build "$TMP/ping.ae" --target=loong64-linux-musl --emit=csrc -o "$TMP/alias.c" \
    > "$TMP/alias.log" 2>&1 || { sed -n '1,6p' "$TMP/alias.log"; fail "alias loong64-linux-musl not recognised"; }

msg="static ELF for both"
[ -n "$ran" ] && msg="$msg; ran under qemu:$ran"
[ -n "$skipped" ] && msg="$msg; not run:$skipped"
echo "  [PASS] cross_riscv64_loongarch64: $msg"
