#!/bin/sh
# Integration test for message tracing (#1333).
#
# Asserts the three properties the feature is only useful if it has:
#   1. a default build contains no tracing at all, and setting AETHER_TRACE
#      does nothing to it;
#   2. `ae build --trace` produces a binary that writes the trace, and the
#      events match the program's known message sequence IN ORDER;
#   3. the events carry message NAMES, not the bare integer ids the runtime
#      sees, which is what makes a trace readable at all.
#
# 1 is the one worth the most: the whole design rests on the shipped build
# carrying no tracing, so a test that only checked tracing works would pass
# just as happily if it were compiled into everything.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] message_trace: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export AETHER_HOME="$ROOT"

# (1) default build: tracing is not compiled in.
if ! "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMP/plain" >"$TMP/plain.log" 2>&1; then
    echo "  [FAIL] message_trace: default build failed"
    sed 's/^/        /' "$TMP/plain.log" | head -10
    exit 1
fi
AETHER_TRACE="$TMP/plain.jsonl" "$TMP/plain" >/dev/null 2>&1
if [ -f "$TMP/plain.jsonl" ]; then
    echo "  [FAIL] message_trace: a default build wrote a trace; tracing is compiled in"
    exit 1
fi

# (2) traced build: the trace exists and the program still behaves.
if ! "$AE" build --trace "$SCRIPT_DIR/probe.ae" -o "$TMP/traced" >"$TMP/traced.log" 2>&1; then
    echo "  [FAIL] message_trace: --trace build failed"
    sed 's/^/        /' "$TMP/traced.log" | head -20
    exit 1
fi
out=$(AETHER_TRACE="$TMP/t.jsonl" "$TMP/traced" 2>&1)
expected_out="ping 1
pong 2
ping 3"
if [ "$out" != "$expected_out" ]; then
    echo "  [FAIL] message_trace: traced build changed program output"
    echo "$out" | sed 's/^/        /'
    exit 1
fi
if [ ! -s "$TMP/t.jsonl" ]; then
    echo "  [FAIL] message_trace: --trace build wrote no trace"
    exit 1
fi

# (3) the message sequence, in order, by NAME.
seq=$(grep -o '"msg_name":"[A-Za-z]*"' "$TMP/t.jsonl" | sed 's/.*:"//;s/"//' | tr '\n' ',')
if [ "$seq" != "Ping,Pong,Ping," ]; then
    echo "  [FAIL] message_trace: send sequence is '$seq', expected 'Ping,Pong,Ping,'"
    sed 's/^/        /' "$TMP/t.jsonl" | head -12
    exit 1
fi

# Delivery and processing both have to appear, or the trace is only recording
# intent and not what the runtime actually did.
for ev in step_begin step_end; do
    if ! grep -q "\"event\":\"$ev\"" "$TMP/t.jsonl"; then
        echo "  [FAIL] message_trace: no '$ev' events in the trace"
        exit 1
    fi
done

# The summary line reports completeness; a wrapped ring must say so.
if ! grep -q '"summary":true' "$TMP/t.jsonl"; then
    echo "  [FAIL] message_trace: trace has no summary line"
    exit 1
fi

echo "  [PASS] message_trace: absent by default, and records the real sequence under --trace"
