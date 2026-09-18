#!/bin/bash
# Regression test: multi-value return that mixes a literal "" with a
# destructured local, inside a nested if-body, with a fall-through
# return that calls a tuple-typed function.
#
# ============================================================
# Why this exists — and the litmus test for the bug cluster
# ============================================================
#
# The Go-style (value, err) tuple-return migration in stdlib
# (std.map, std.list, std.fs, std.zlib, std.cryptography, ...) put
# weight on cross-function tuple-return inference. Several latent
# bugs in compiler/analysis/type_inference.c surfaced together only
# when *all four* of these shapes coincide in one function:
#
#   1. The body destructures a multi-value call into local idents.
#   2. One branch returns a tuple that mixes a literal ("") with
#      an ident that came from the destructure.
#   3. That branch lives inside an if/while/for body — not directly
#      inside the function's outermost block.
#   4. The fall-through return is a single-value call to a
#      tuple-typed function.
#
# Pre-fix, codegen emitted the offending function as
# `_tuple_string_int` (second slot decayed to int) and every
# downstream caller produced -Wint-conversion errors; runtime then
# segfaulted on the string-shaped slot. The shipped fixes carry
# the inline rationale at each fix site in
# compiler/analysis/type_inference.c — search for the
# "Bug #2 / #3 / #4" comments. Bug #1 (cross-module local-var
# pollution) shipped earlier in commit bc42939; its companion test
# is tests/integration/tuple_destructure_cross_module/.
#
# Litmus test for "is one of these regressing":
#   if codegen prints `unresolved type in codegen, defaulting to
#   int` on a function whose return mixes a destructured local
#   with a literal inside a nested if-body, one of the three fixes
#   in this commit is misbehaving.
#
# ============================================================
# Repro the test drives
# ============================================================
#
# hash_file(p) walks the four-part shape:
#   bytes, length, rerr = fs.read_binary(p)        // (1) destructure
#   if string.length(rerr) > 0 {
#       return "", rerr                             // (2) literal +
#                                                   //     destructured local
#                                                   // (3) inside if-body
#   }
#   return cryptography.sha256_hex(bytes, length)   // (4) tuple-typed call
#
# Both branches return (string, string). The test's main() drives
# both — happy path against a real file, error path against a
# missing file — and asserts the second slot survives as a string
# all the way to the caller.
#
# ============================================================
# Bash invocation note
# ============================================================
#
# Test harness now invokes via `bash` (PR #245); `set -eu` alone is
# enough — no pipefail dependency, no pipelines used.
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
AETHER_ROOT="$(cd "$DIR/../../.." && pwd)"
AE="${AETHER_ROOT}/build/ae"

if [ ! -x "$AE" ]; then
    echo "SKIP: $AE not built"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK" || true' EXIT

# Repro shape from docs/type-inference-multi-value-returns.md §"The
# shape that triggers the cluster". hash_file:
#   - destructures a 3-tuple from fs.read_binary (string, int, string)
#   - early-returns ("", rerr) when the third slot signals an error
#   - falls through to cryptography.sha256_hex (string, string)
# Both branches return (string, string).
cat > "$WORK/main.ae" <<'AE'
import std.fs
import std.cryptography
import std.string
import std.io

extern exit(code: int)

hash_file(p: string) -> {
    bytes, length, rerr = fs.read_binary(p)
    if string.length(rerr) > 0 {
        return "", rerr
    }
    return cryptography.sha256_hex(bytes, length)
}

main() {
    tmp = io.getenv("TEMP")
    if tmp == 0 { tmp = io.getenv("TMPDIR") }
    if tmp == 0 { tmp = "/tmp" }

    // Write a known-content file, hash it, verify the digest.
    werr = fs.write("${tmp}/hash_repro.bin", "hello, aether\n")
    if werr != "" { println("write failed: ${werr}"); exit(1) }

    digest, derr = hash_file("${tmp}/hash_repro.bin")
    if derr != "" { println("hash failed: ${derr}"); exit(1) }
    if string.length(digest) != 64 {
        println("digest wrong length: ${string.length(digest)}")
        exit(1)
    }

    // Negative path: missing file → second slot must come back as
    // a string, not whatever the int-typed-empty-string-bug left
    // behind. This is the assertion that fails before the fix.
    _, merr = hash_file("${tmp}/this/path/does/not/exist")
    if string.length(merr) == 0 {
        println("expected non-empty err on missing file")
        exit(1)
    }

    println("PASS")
}
AE

cd "$WORK"
"$AE" build main.ae -o main >build.out 2>&1 || {
    cat build.out
    echo "FAIL: build rejected the multi-return destructure chain"
    exit 1
}

# Catch the codegen-warning-fallback signal too — pre-fix, the build
# succeeded but emitted "unresolved type in codegen, defaulting to
# int" on stderr. Treat that as a failure.
if grep -qi "unresolved type" build.out; then
    cat build.out
    echo "FAIL: codegen emitted 'unresolved type' fallback"
    exit 1
fi

out="$(./main 2>&1)" || {
    echo "FAIL: runtime error"
    echo "stdout: $out"
    exit 1
}
if [ "$out" != "PASS" ]; then
    echo "FAIL: expected 'PASS', got '$out'"
    exit 1
fi
echo "PASS"
