#!/bin/sh
# `ae bindgen consts` (#1245): object-like C macros that expand to scalar
# constants import as Aether consts; everything else is skipped visibly.
# The pipeline is preprocessor-only, so nested macros fold exactly as C
# sees them; the test's ground truth is bit-flag math agreeing across the
# language boundary.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || {
    echo "  [SKIP] bindgen_consts: no C compiler on PATH"
    exit 0
}

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

cat > "$TMPDIR/flags.h" <<'HEOF'
#ifndef FLAGS_H
#define FLAGS_H
#define F_A        (1<<0)
#define F_B        (1<<3)
#define F_BOTH     (F_A|F_B)
#define BIG        (512ll*1024*1024)
#define NEG        -1
#define NAME       "fix" "ture"
#define ESCAPED    "\x1b[0m"
#define RATIO      0.5
#define FN(x)      ((x)+1)
#define CASTED     ((void*)0)
#endif
HEOF

mkdir -p "$TMPDIR/proj/lib/flags"
# `set -e` must not swallow a bindgen failure before its stderr is shown:
# the harness prints nothing for a shell test that dies silently, which is
# what made the original Windows break read as "(no output)" in CI.
if ! "$AE" bindgen consts "$TMPDIR/flags.h" \
        -o "$TMPDIR/proj/lib/flags/module.ae" 2>"$TMPDIR/log"; then
    echo "  [FAIL] ae bindgen consts exited non-zero"; cat "$TMPDIR/log"; exit 1
fi
grep -q "8 consts imported" "$TMPDIR/log" || {
    echo "  [FAIL] expected 8 imports"; cat "$TMPDIR/log"; exit 1
}

# Skips must be visible, not silent.
grep -q "Skipped" "$TMPDIR/proj/lib/flags/module.ae" || {
    echo "  [FAIL] skipped macros not listed"; exit 1
}
grep -q "CASTED" "$TMPDIR/proj/lib/flags/module.ae" || {
    echo "  [FAIL] CASTED should appear in the skip list"; exit 1
}

cd "$TMPDIR/proj"
cat > main.ae <<'AEOF'
import flags

extern exit(code: int)

main() {
    both = flags.F_A | flags.F_B
    if both != flags.F_BOTH {
        println("FAIL: flag math disagrees: ${both} vs ${flags.F_BOTH}")
        exit(1)
    }
    if flags.BIG != 536870912 {
        println("FAIL: suffixed literal wrong: ${flags.BIG}")
        exit(1)
    }
    if flags.NEG != 0 - 1 {
        println("FAIL: negative wrong")
        exit(1)
    }
    if flags.NAME != "fixture" {
        println("FAIL: string concat wrong: ${flags.NAME}")
        exit(1)
    }
    println("ok")
}
AEOF

OUT=$("$AE" run main.ae 2>"$TMPDIR/runlog" | tail -1)
if [ "$OUT" != "ok" ]; then
    echo "  [FAIL] generated module misbehaved: $OUT"
    cat "$TMPDIR/runlog"
    exit 1
fi

# --match narrows the surface.
if ! "$AE" bindgen consts "$TMPDIR/flags.h" --match F_ \
        -o "$TMPDIR/narrow.ae" 2>"$TMPDIR/log2"; then
    echo "  [FAIL] ae bindgen consts --match exited non-zero"
    cat "$TMPDIR/log2"; exit 1
fi
grep -q "3 consts imported" "$TMPDIR/log2" || {
    echo "  [FAIL] --match F_ should import exactly F_A, F_B, F_BOTH"
    cat "$TMPDIR/log2"; exit 1
}

echo "  [PASS] bindgen_consts: 8 imports, visible skips, flag math agrees, --match narrows"
