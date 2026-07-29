#!/bin/sh
# #1299: emitted-C determinism gate.
#
# The compiler guarantees byte-identical C for the same source and the
# same compiler build (docs/architecture.md, "Deterministic output").
# That property holds today because codegen order tracks AST/source
# order and no hash-table iteration or volatile metadata (timestamps,
# build paths) feeds the emitted text. Nothing enforced it, so a future
# change could silently regress it; this gate compiles a corpus twice
# and byte-compares the output.
#
# The corpus spans the surfaces where nondeterminism typically creeps
# in: actors + messages (registry walks), std imports (module registry),
# string interpolation (literal pools), and collection-heavy code
# (helper dedup tables).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

CORPUS="examples/basics/hello.ae
examples/basics/string-processing.ae
examples/actors/ask-pattern.ae
examples/applications/chat-room.ae
examples/applications/task-queue.ae
tests/regression/test_std_set.ae
tests/regression/test_std_pqueue.ae"

i=0
for src in $CORPUS; do
    i=$((i + 1))
    if ! "$AETHERC" "$ROOT/$src" "$TMPDIR/a$i.c" >"$TMPDIR/log$i.a" 2>&1; then
        echo "  [FAIL] emit_c_determinism: first compile of $src failed"
        tail -5 "$TMPDIR/log$i.a"
        exit 1
    fi
    if ! "$AETHERC" "$ROOT/$src" "$TMPDIR/b$i.c" >"$TMPDIR/log$i.b" 2>&1; then
        echo "  [FAIL] emit_c_determinism: second compile of $src failed"
        tail -5 "$TMPDIR/log$i.b"
        exit 1
    fi
    if ! cmp -s "$TMPDIR/a$i.c" "$TMPDIR/b$i.c"; then
        echo "  [FAIL] emit_c_determinism: $src emitted different C across two runs"
        diff "$TMPDIR/a$i.c" "$TMPDIR/b$i.c" | head -10
        exit 1
    fi
    # Volatile metadata guard: a same-second recompile cannot catch baked
    # dates, so reject the mechanisms rather than their output.
    if grep -qE '__DATE__|__TIME__|__TIMESTAMP__' "$TMPDIR/a$i.c"; then
        echo "  [FAIL] emit_c_determinism: $src bakes a build timestamp macro"
        grep -nE '__DATE__|__TIME__|__TIMESTAMP__' "$TMPDIR/a$i.c" | head -3
        exit 1
    fi
done

echo "  [PASS] emit_c_determinism: $i programs byte-identical across two compiles"
