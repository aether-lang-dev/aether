#!/bin/sh
# Regression: std.cryptography incremental digest context (fbs-core ask
# #4) — digest_new / digest_update / digest_final_hex / digest_final_bytes
# / digest_free. Streamed digest must equal the one-shot digest and the
# known vectors. See probe.ae for the six-case matrix.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! "$ROOT/build/ae" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] cryptography_digest_stream: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -15
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] cryptography_digest_stream: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

# No skip branch. The streaming API used to be OpenSSL-only, so a build
# without libcrypto could only report "skipped" and this test passed by not
# running. It now falls back to the pure-Aether streaming contexts, so every
# case is expected to pass on every platform -- and a skip that is still
# accepted is a green tick for a fallback nobody exercised.
if grep -q "All streaming digest tests passed" "$TMPDIR/run.log"; then
    echo "  [PASS] cryptography_digest_stream: 6 cases"
else
    echo "  [FAIL] cryptography_digest_stream: didn't reach the final PASS line"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi
