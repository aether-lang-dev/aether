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
#
# The gate's cost is process starts, which are slow on Windows: about 20
# per sampled file took it past the runner's 120-second limit there (135 s
# on an idle machine). So tier 2 runs `ae` twice for the whole sample, and
# tier 3 compiles each file twice; the third compile, which tells an
# emission fault from a formatter one, runs only when the two differ.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] || [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] fmt_gate: toolchain not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"

# The IR tier copies each sampled file to a sibling NEXT TO IT, inside the
# source tree, because imports resolve relative to the source file's
# directory (the reason is at the copy). That sibling was removed on the
# normal path and on the explicit failure paths -- but not when the script
# is KILLED, and a sweep that times out or is interrupted does exactly
# that. What it leaves behind is a stray `.ae` inside tests/, which the
# next run then samples and `ae fmt` then reports as unformatted: one
# interrupted run poisons the tree for every run after it. (Found as a
# real leftover: tests/integration/extern_tuple_var_passthrough/
# ._fmt_gate_tmp_964.ae.)
#
# So the trap takes the sibling too, and covers the signals a kill
# actually sends rather than EXIT alone.
SIBLING=""
fmt_gate_cleanup() {
    rm -rf "$TMPDIR"
    [ -n "$SIBLING" ] && rm -f "$SIBLING"
    return 0
}
# EXIT cleans up. The signals clean up AND EXIT: a trap handler that just
# returns resumes the script, so on the sweep's SIGTERM this would have
# carried on with its scratch directory already deleted, skipped the rest
# in silence and reported [PASS]. A killed run that says it passed is worse
# than the leftover the trap was added for. 128+N is the shell's own
# convention for "died of signal N".
trap 'fmt_gate_cleanup' EXIT
trap 'fmt_gate_cleanup; exit 129' HUP
trap 'fmt_gate_cleanup; exit 130' INT
trap 'fmt_gate_cleanup; exit 143' TERM

# ... and a tree already poisoned by an earlier interrupted run heals
# itself here, loudly, rather than failing the gate for a file nobody
# wrote.

# Print the first differing lines of two files with line numbers,
# then a byte-level hex first-difference. awk/od-based because the
# Windows MSYS2 CI shell ships no diff/cmp (diffutils not installed).
# The hex pass exists because a text compare can read two files as
# equal while their bytes differ (embedded NUL truncates awk's line
# strings); od gives ground truth.
fmt_gate_dump_diff() {
    awk 'NR==FNR { a[FNR] = $0; n = FNR; next }
         FNR<=n && $0 != a[FNR] && shown < 12 {
             printf "    line %d\n      A: %s\n      B: %s\n", FNR, a[FNR], $0
             shown++
         }
         END { if (FNR != n) printf "    (line counts differ: %d vs %d)\n", n, FNR }' \
        "$1" "$2"
    od -An -v -tx1 "$1" | tr -s ' ' '\n' | grep -v '^$' > "${1}.hex"
    od -An -v -tx1 "$2" | tr -s ' ' '\n' | grep -v '^$' > "${2}.hex"
    awk 'NR==FNR { a[FNR] = $0; n = FNR; next }
         FNR<=n && $0 != a[FNR] && !hit {
             hit = FNR
             printf "    first byte difference at offset %d: %s vs %s\n", FNR - 1, a[FNR], $0
         }
         END {
             if (!hit && FNR != n) printf "    (byte counts differ: %d vs %d)\n", n, FNR
             else if (!hit) printf "    (byte streams identical: %d bytes)\n", n
         }' "${1}.hex" "${2}.hex"
    # Context: 32 bytes around the first difference from each file.
    off=$(awk 'NR==FNR { a[FNR] = $0; n = FNR; next }
               FNR<=n && $0 != a[FNR] { print FNR - 1; exit }' "${1}.hex" "${2}.hex")
    if [ -n "$off" ]; then
        start=$((off > 16 ? off - 16 : 0))
        echo "    A context:"; od -An -c -j "$start" -N 48 "$1" | sed 's/^/      /'
        echo "    B context:"; od -An -c -j "$start" -N 48 "$2" | sed 's/^/      /'
    fi
    rm -f "${1}.hex" "${2}.hex"
}

# Checksum of a generated-C file with #line directives stripped.
# awk writes a temp then cksum reads by redirect: no pipe reading a
# freshly-written file, and callers can recompute for the settle
# recheck above.
fmt_gate_ir_sum() {
    awk '!/^#line/' "$1" > "$1.strip"
    cksum < "$1.strip"
    rm -f "$1.strip"
}

cd "$ROOT" || exit 1

# A tree poisoned by an earlier interrupted run heals itself here, loudly,
# rather than failing the gate for a file nobody wrote. After the cd, so the
# relative paths mean what they say whatever the caller's directory was.
stale="$(find std examples tests -name '._fmt_gate_tmp_*.ae' 2>/dev/null || true)"
if [ -n "$stale" ]; then
    echo "  [note] fmt_gate: removing leftovers from an interrupted run:"
    printf '%s\n' "$stale" | sed 's/^/           /'
    printf '%s\n' "$stale" | while read -r leftover; do rm -f "$leftover"; done
fi

# Tier 1: canonical formatting.
if ! "$AE" fmt --check std examples tests > "$TMPDIR/check.txt" 2>&1; then
    echo "  [FAIL] fmt_gate: files are not canonically formatted (run: ae fmt std examples tests)"
    sed 's/^/    /' "$TMPDIR/check.txt" | head -15
    exit 1
fi

# Deterministic sample for tiers 2 and 3: every 12th program file.
# LC_ALL=C pins collation so every platform samples the SAME files;
# locale-dependent sort orders gave Windows a different sample than
# Linux on the first CI round.
find examples tests -name '*.ae' -type f 2>/dev/null | LC_ALL=C sort | \
    awk 'NR % 12 == 1' > "$TMPDIR/sample.txt"

# Tier 2: idempotence. fmt writes in place, so it runs on numbered copies
# (copy N is line N of the sample); once formatted, a copy is canonical
# exactly when fmt(fmt(x)) == fmt(x), which `ae fmt --check` answers for
# all of them in one run.
mkdir "$TMPDIR/idem"
TOTAL=0
while IFS= read -r f; do
    TOTAL=$((TOTAL + 1))
    cp "$f" "$TMPDIR/idem/$TOTAL.ae"
done < "$TMPDIR/sample.txt"
"$AE" fmt "$TMPDIR/idem" >/dev/null 2>"$TMPDIR/idem_fmt.txt"
"$AE" fmt --check "$TMPDIR/idem" >"$TMPDIR/idem_check.txt" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    if [ $rc -eq 1 ]; then
        echo "  [FAIL] fmt_gate: fmt is not idempotent on:"
        sed 's|.*/\([0-9]*\)\.ae$|\1|' "$TMPDIR/idem_check.txt" | while read -r n; do
            echo "    $(sed -n "${n}p" "$TMPDIR/sample.txt")"
        done
    else
        echo "  [FAIL] fmt_gate: ae fmt could not reformat its own output"
        sed 's/^/    /' "$TMPDIR/idem_check.txt" "$TMPDIR/idem_fmt.txt" | head -10
    fi
    exit 1
fi

# Tier 3: IR preservation.
IR_CHECKED=0
n=0
while IFS= read -r f; do
    n=$((n + 1))
    base="$TMPDIR/s$n"

    # Compile a sibling copy, format IT in place, compile again, compare:
    # both compiles use the SAME file name, so path-derived emission
    # differences cancel and the diff isolates exactly what formatting
    # changed. The sibling must sit NEXT TO the original: imports resolve
    # relative to the source file's directory, so a copy in $TMPDIR would
    # fail cross-module fixtures for the wrong reason. Deliberate-reject
    # fixtures don't compile; skip them for this tier (they still passed
    # tiers 1+2).
    sibling="$(dirname "$f")/._fmt_gate_tmp_$$.ae"
    SIBLING="$sibling"   # so a kill between here and the rm below still cleans up
    cp "$f" "$sibling"
    if "$AETHERC" "$sibling" "$base.orig.c" >/dev/null 2>&1; then
        "$AE" fmt "$sibling" >/dev/null 2>&1
        if ! "$AETHERC" "$sibling" "$base.fmt.c" >/dev/null 2>&1; then
            rm -f "$sibling"
            echo "  [FAIL] fmt_gate: $f compiles but its formatted copy does not"
            exit 1
        fi
        sum_o=$(fmt_gate_ir_sum "$base.orig.c")
        sum_f=$(fmt_gate_ir_sum "$base.fmt.c")
        if [ "$sum_o" != "$sum_f" ]; then
            # Recheck after a settle: on the Windows CI runners a
            # checksum pipeline reading a JUST-written file can see a
            # stale/short read through the MSYS2 layer while a later
            # read sees the full content (proven by od showing the
            # "differing" files byte-identical). A real emission
            # difference still differs on the second read.
            sleep 1
            sum_o=$(fmt_gate_ir_sum "$base.orig.c")
            sum_f=$(fmt_gate_ir_sum "$base.fmt.c")
        fi
        if [ "$sum_o" != "$sum_f" ]; then
            # Which invariant broke: compile the UNFORMATTED file a second
            # time. If two compiles of identical input already differ, it
            # is an emission-determinism failure, not a formatter one, and
            # is reported as such (first seen on Windows CI, where only
            # this attribution shows which invariant actually broke).
            cp "$f" "$sibling"
            if "$AETHERC" "$sibling" "$base.orig2.c" >/dev/null 2>&1 &&
               [ "$(fmt_gate_ir_sum "$base.orig2.c")" != "$sum_o" ]; then
                echo "  [FAIL] fmt_gate: two compiles of UNFORMATTED $f differ (emission nondeterminism, not a formatter fault)"
                echo "    sizes: $(wc -c < "$base.orig.c") vs $(wc -c < "$base.orig2.c") bytes"
                fmt_gate_dump_diff "$base.orig.c" "$base.orig2.c"
            elif cmp -s "$f" "$TMPDIR/idem/$n.ae" 2>/dev/null; then
                echo "  [FAIL] fmt_gate: formatting left $f byte-identical yet its C differs (emission instability)"
                fmt_gate_dump_diff "$base.orig.c" "$base.fmt.c"
            else
                echo "  [FAIL] fmt_gate: formatting $f changed the emitted C"
                fmt_gate_dump_diff "$base.orig.c" "$base.fmt.c"
            fi
            rm -f "$sibling"
            exit 1
        fi
        IR_CHECKED=$((IR_CHECKED + 1))
    fi
    rm -f "$sibling" "$base.orig.c" "$base.fmt.c"
    SIBLING=""
done < "$TMPDIR/sample.txt"

echo "  [PASS] fmt_gate: tree canonical; idempotent on $TOTAL sampled files; IR-preserving on $IR_CHECKED"
exit 0
