#!/bin/sh
set -e

# Prove that the fuzz harness can find bugs by planting two mutants in std/cbor/module.ae
# and running the fuzzer against them until crash (or timeout FUZZ_SECONDS=300).
#
# M1: remove the depth check in parse_item (expect stack overflow or ASan report on deep nesting).
# M2: remove the check that a declared length fits in the remaining bytes (expect OOB or oversized allocation).
# M3: a definite array one element over max_array_elements accepted.
# M4: the definite map's max_map_pairs check removed.
# M5: indefinite-length strings accepted though allow_indefinite is 0.
# M1 and M3..M5 are found by the oracle's limit check, not by a crash.
#
# Does NOT use git stash and does NOT edit the real std/cbor/module.ae.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
CLANG="${CLANG:-clang}"
FUZZ_SECONDS="${FUZZ_SECONDS:-300}"

TMPDIR_BASE="$(mktemp -d)"
cleanup() {
    rm -rf "$TMPDIR_BASE"
}
trap cleanup EXIT INT TERM

# ASan keeps a stack trace for every allocation and never frees one. The
# decoder recurses once per level, so with the default 30 frames nearly
# every nesting shape is a new trace, and RSS climbed past -rss_limit_mb
# (2 GB) in 30 minutes with no leak in the program. 8 frames still name the
# allocating function and stay flat. A caller's ASAN_OPTIONS wins.
export ASAN_OPTIONS="${ASAN_OPTIONS:-malloc_context_size=8}"

DICT_FILE="$SCRIPT_DIR/cbor.dict"
ORIGINAL_CBOR="$ROOT/std/cbor/module.ae"

run_mutant() {
    name="$1"
    mutant_ae="$2"
    echo "=========================================================="
    echo "Running Mutant $name..."
    echo "=========================================================="

    M_DIR="$TMPDIR_BASE/$name"
    mkdir -p "$M_DIR/std/cbor"
    mkdir -p "$M_DIR/corpus"
    cp -r "$SCRIPT_DIR/corpus"/* "$M_DIR/corpus/" 2>/dev/null || true

    # Copy the mutant module into place in a local module tree
    cp "$mutant_ae" "$M_DIR/std/cbor/module.ae"

    # Also copy harness and compile against this mutant module using --lib
    cp "$SCRIPT_DIR/fuzz_cbor.ae" "$M_DIR/fuzz_cbor.ae"
    cp "$SCRIPT_DIR/fuzz_cbor.c" "$M_DIR/fuzz_cbor.c"

    echo "==> Building $name harness..."
    (cd "$M_DIR" && AE_CC="$CLANG -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -g -O1" \
        "$AE" build "fuzz_cbor.ae" --with=extern --emit=obj -o "fuzz_cbor_ae.o")

    AE_CFLAGS="$("$AE" cflags)"
    # shellcheck disable=SC2086
    (cd "$M_DIR" && "$CLANG" -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1 \
        "fuzz_cbor.c" "fuzz_cbor_ae.o" \
        $AE_CFLAGS \
        -o "fuzz_cbor")

    echo "==> Fuzzing $name (up to ${FUZZ_SECONDS}s)..."
    cd "$M_DIR"
    set +e
    start_time=$(date +%s)
    ./fuzz_cbor \
        -dict="$DICT_FILE" \
        -max_len=65536 \
        -rss_limit_mb=2048 \
        -max_total_time="$FUZZ_SECONDS" \
        "$M_DIR/corpus" > "$M_DIR/fuzz.log" 2>&1
    rc=$?
    end_time=$(date +%s)
    duration=$((end_time - start_time))
    set -e

    if [ $rc -eq 0 ]; then
        echo "FAIL: $name was NOT found within ${FUZZ_SECONDS}s!" >&2
        cat "$M_DIR/fuzz.log" | tail -n 30 >&2
        return 1
    fi

    echo "SUCCESS: $name found in ${duration}s (exit code $rc)!"
    echo "--- Crash report summary for $name ---"
    # Print crash type / sanitizer report lines
    grep -E "ERROR: (AddressSanitizer|libFuzzer|UndefinedBehaviorSanitizer)|SCARINESS|SUMMARY:|SEGV|stack-overflow|out-of-memory|deadly signal|Test unit written" "$M_DIR/fuzz.log" | head -n 10 || head -n 25 "$M_DIR/fuzz.log"
    echo "--------------------------------------"
}

# ----------------- Create Mutant M1 -----------------
# Remove the depth check in parse_item:
# if depth > reader.max_depth { ... }
M1_FILE="$TMPDIR_BASE/m1.ae"
python3 - << EOF
with open("$ORIGINAL_CBOR") as f:
    content = f.read()

target = """    if depth > reader.max_depth {
        return null, string.concat("cbor: nesting deeper than ", string.from_int(reader.max_depth))
    }"""
assert target in content, "M1 target pattern not found in module.ae"
mutated = content.replace(target, "    // M1: depth check removed")
with open("$M1_FILE", "w") as f:
    f.write(mutated)
EOF

# ----------------- Create Mutant M2 -----------------
# Remove the check that a declared length fits in the remaining bytes:
# rem = (reader.len - reader.offset) as long
# if len_val < 0 || len_val > rem { return null, "cbor: unexpected EOF" }
M2_FILE="$TMPDIR_BASE/m2.ae"
python3 - << EOF
with open("$ORIGINAL_CBOR") as f:
    content = f.read()

target = """        rem = (reader.len - reader.offset) as long
        if len_val < 0 || len_val > rem { return null, "cbor: unexpected EOF" }"""
assert target in content, "M2 target pattern not found in module.ae"
mutated = content.replace(target, """        rem = (reader.len - reader.offset) as long
        // M2: remaining bytes check removed for byte/text strings
        if len_val < 0 { return null, "cbor: unexpected EOF" }""")
with open("$M2_FILE", "w") as f:
    f.write(mutated)
EOF

# ----------------- M3..M5: the limits the oracle measures -----------------
# Each is invisible to "doesn't crash": the decoder accepts what the options
# refuse, and only the oracle's limit check (within_limits) can see it.
make_mutant() {
    out="$1"; target="$2"; replacement="$3"
    TARGET="$target" REPLACEMENT="$replacement" python3 - "$ORIGINAL_CBOR" "$out" << 'PYEOF'
import os, sys
src = open(sys.argv[1]).read()
t, r = os.environ["TARGET"], os.environ["REPLACEMENT"]
assert src.count(t) == 1, "mutant target not found exactly once: " + t
open(sys.argv[2], "w").write(src.replace(t, r))
PYEOF
}
M3_FILE="$TMPDIR_BASE/m3.ae"
make_mutant "$M3_FILE" \
    '        if len_val > (reader.max_array_elements as long) {' \
    '        if len_val > (reader.max_array_elements as long) + 1 {'
M4_FILE="$TMPDIR_BASE/m4.ae"
make_mutant "$M4_FILE" \
    '        if len_val > (reader.max_map_pairs as long) {' \
    '        if false {'
M5_FILE="$TMPDIR_BASE/m5.ae"
make_mutant "$M5_FILE" \
    'fn parse_indefinite_string(r: ptr, mt: int, depth: int) -> (ptr, string) {
    reader = r as *Reader
    if reader.allow_indefinite == 0 {' \
    'fn parse_indefinite_string(r: ptr, mt: int, depth: int) -> (ptr, string) {
    reader = r as *Reader
    if false {'

echo "Testing mutants with prove_mutants.sh..."
# `prove_mutants.sh M3 M5` runs only those.
for m in ${*:-M1 M2 M3 M4 M5}; do
    eval "run_mutant $m \"\$${m}_FILE\""
done

echo "All mutants proven successfully!"
