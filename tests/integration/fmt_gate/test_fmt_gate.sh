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
# Tier 3 is six processes a file (copy, compile, format, compile, compare,
# remove), down from thirteen (#2596).

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

# Whether two generated-C files are the same once #line directives are
# stripped: line for line, and as many lines. One awk reads both files by
# name (no pipe reading a freshly-written file) and callers can ask again
# for the settle recheck below. It replaced a checksum of each file (awk to
# a temp, cksum, rm, under a command substitution), eight processes a pair
# and over a third of the gate's time on Windows (#2596); the comparison is
# the same, and exact rather than by checksum.
fmt_gate_ir_same() {
    awk 'FNR == NR { if (!/^#line/) a[++n] = $0; next }
         !/^#line/ { if (++m > n || $0 != a[m]) { differ = 1; exit } }
         END { exit (differ || m != n) }' "$1" "$2"
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

# Tier 2: idempotence. fmt writes in place, so it runs on copies, under the
# same relative paths in $TMPDIR/idem; once formatted, a copy is canonical
# exactly when fmt(fmt(x)) == fmt(x), which `ae fmt --check` answers for
# all of them in one run. One tar copies the whole sample, where a `cp` per
# file was a process start each (#2596). Both runs name every file (one per
# argument, so a path keeps its spaces) rather than walking the copy, which
# would pass over a file under a hidden directory such as
# custom_lib_dir/.mylib; and what --check lists is then the sample's path.
mkdir "$TMPDIR/idem"
TOTAL=$(awk 'END { print NR }' "$TMPDIR/sample.txt")
if ! tar cf - -T "$TMPDIR/sample.txt" | (cd "$TMPDIR/idem" && tar xf -) ||
   [ "$(find "$TMPDIR/idem" -type f | wc -l | tr -d ' ')" != "$TOTAL" ]; then
    echo "  [FAIL] fmt_gate: could not copy the $TOTAL sampled files for the idempotence tier"
    exit 1
fi
saved_ifs=$IFS
IFS='
'
set -f
# shellcheck disable=SC2046
set -- $(cat "$TMPDIR/sample.txt")
set +f
IFS=$saved_ifs
(cd "$TMPDIR/idem" && "$AE" fmt "$@") >/dev/null 2>"$TMPDIR/idem_fmt.txt"
(cd "$TMPDIR/idem" && "$AE" fmt --check "$@") >"$TMPDIR/idem_check.txt" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    if [ $rc -eq 1 ]; then
        echo "  [FAIL] fmt_gate: fmt is not idempotent on:"
        sed 's/^/    /' "$TMPDIR/idem_check.txt"
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
    # (Every sampled path has a directory part: it comes from `find examples
    # tests`. Cut with an expansion, not a `dirname` process per file.)
    sibling="${f%/*}/._fmt_gate_tmp_$$.ae"
    SIBLING="$sibling"   # so a kill between here and the rm below still cleans up
    cp "$f" "$sibling"
    if "$AETHERC" "$sibling" "$base.orig.c" >/dev/null 2>&1; then
        "$AE" fmt "$sibling" >/dev/null 2>&1
        if ! "$AETHERC" "$sibling" "$base.fmt.c" >/dev/null 2>&1; then
            rm -f "$sibling"
            echo "  [FAIL] fmt_gate: $f compiles but its formatted copy does not"
            exit 1
        fi
        ir_same=1
        if ! fmt_gate_ir_same "$base.orig.c" "$base.fmt.c"; then
            # Recheck after a settle: on the Windows CI runners a
            # checksum pipeline reading a JUST-written file can see a
            # stale/short read through the MSYS2 layer while a later
            # read sees the full content (proven by od showing the
            # "differing" files byte-identical). A real emission
            # difference still differs on the second read.
            sleep 1
            fmt_gate_ir_same "$base.orig.c" "$base.fmt.c" || ir_same=0
        fi
        if [ "$ir_same" = 0 ]; then
            # Which invariant broke: compile the UNFORMATTED file a second
            # time. If two compiles of identical input already differ, it
            # is an emission-determinism failure, not a formatter one, and
            # is reported as such (first seen on Windows CI, where only
            # this attribution shows which invariant actually broke).
            cp "$f" "$sibling"
            if "$AETHERC" "$sibling" "$base.orig2.c" >/dev/null 2>&1 &&
               ! fmt_gate_ir_same "$base.orig.c" "$base.orig2.c"; then
                echo "  [FAIL] fmt_gate: two compiles of UNFORMATTED $f differ (emission nondeterminism, not a formatter fault)"
                echo "    sizes: $(wc -c < "$base.orig.c") vs $(wc -c < "$base.orig2.c") bytes"
                fmt_gate_dump_diff "$base.orig.c" "$base.orig2.c"
            elif cmp -s "$f" "$TMPDIR/idem/$f" 2>/dev/null; then
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
