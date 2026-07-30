#!/bin/sh
# #1302: ae fmt CI gate, three tiers.
#
#   1. Format check: every .ae under std/ examples/ tests/ is
#      canonically formatted (`ae fmt --check` exits 0).
#   2. Idempotence: fmt(fmt(x)) == fmt(x) over a sampled corpus.
#   3. IR preservation: compile(fmt(x)) emits byte-identical C to
#      compile(x), modulo #line directives, over the same sample.
#
# docs/formatter.md asserts properties 2 and 3; before this gate they
# were verified by hand and nothing stopped a formatter or emitter
# change from silently breaking them. Tier 1 keeps checked-in source
# canonical so `ae fmt` diffs never mix with logic diffs.
#
# The tier 2/3 sample is deterministic (every Nth file of the sorted
# corpus) so CI cost stays bounded while repeated runs cover the same
# files; the full corpus was verified when the gate landed (443/443
# byte-identical) and any formatter change re-runs this sample.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] || [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] fmt_gate: toolchain not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

cd "$ROOT" || exit 1

# Tier 1: canonical formatting.
if ! "$AE" fmt --check std examples tests > "$TMPDIR/check.txt" 2>&1; then
    echo "  [FAIL] fmt_gate: files are not canonically formatted (run: ae fmt std examples tests)"
    sed 's/^/    /' "$TMPDIR/check.txt" | head -15
    exit 1
fi

# Deterministic sample for tiers 2 and 3: every 12th program file.
find examples tests -name '*.ae' -type f 2>/dev/null | sort | \
    awk 'NR % 12 == 1' > "$TMPDIR/sample.txt"

TOTAL=0
IR_CHECKED=0
while IFS= read -r f; do
    TOTAL=$((TOTAL + 1))
    base="$TMPDIR/$(echo "$f" | tr '/' '_')"

    # Tier 2: idempotence. fmt writes in place, so run on copies.
    cp "$f" "$base.once.ae"
    "$AE" fmt "$base.once.ae" >/dev/null 2>&1
    cp "$base.once.ae" "$base.twice.ae"
    "$AE" fmt "$base.twice.ae" >/dev/null 2>&1
    if [ "$(cksum < "$base.once.ae")" != "$(cksum < "$base.twice.ae")" ]; then
        echo "  [FAIL] fmt_gate: fmt is not idempotent on $f"
        exit 1
    fi

    # Tier 3: IR preservation. Deliberate-reject fixtures don't
    # compile; skip them for this tier (they still passed tiers 1+2).
    # The formatted copy must sit NEXT TO the original: imports
    # resolve relative to the source file's directory, so a copy in
    # $TMPDIR would fail cross-module fixtures for the wrong reason.
    if "$AETHERC" "$f" "$base.orig.c" >/dev/null 2>&1; then
        sibling="$(dirname "$f")/._fmt_gate_tmp_$$.ae"
        cp "$base.once.ae" "$sibling"
        if ! "$AETHERC" "$sibling" "$base.fmt.c" >/dev/null 2>&1; then
            rm -f "$sibling"
            echo "  [FAIL] fmt_gate: $f compiles but its formatted copy does not"
            exit 1
        fi
        rm -f "$sibling"
        sum_o=$(grep -v '^#line' "$base.orig.c" | cksum)
        sum_f=$(grep -v '^#line' "$base.fmt.c" | cksum)
        if [ "$sum_o" != "$sum_f" ]; then
            echo "  [FAIL] fmt_gate: formatting $f changed the emitted C"
            exit 1
        fi
        IR_CHECKED=$((IR_CHECKED + 1))
    fi
    rm -f "$base.once.ae" "$base.twice.ae" "$base.orig.c" "$base.fmt.c"
done < "$TMPDIR/sample.txt"

echo "  [PASS] fmt_gate: tree canonical; idempotent on $TOTAL sampled files; IR-preserving on $IR_CHECKED"
exit 0
