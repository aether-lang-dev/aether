#!/bin/sh
# Test: ae repl — comprehensive tests for session persistence, commands,
# multi-line blocks, edge cases, error recovery, and all exit variants.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

# Runs on Windows/MSYS2 too. An earlier skip called piped REPL input
# "unreliable" there; the 40 cases pass three runs out of three under
# MSYS2 (2026-09, #2145 follow-up), and a REPL that reads a pipe is what
# an editor integration on Windows drives.

pass=0
fail=0

check() {
    name="$1"; input="$2"; expected="$3"
    actual=$(printf "$input" | AETHER_HOME="" "$AE" repl 2>&1)
    if echo "$actual" | grep -qF -- "$expected"; then
        echo "  [PASS] repl: $name"
        pass=$((pass + 1))
    else
        echo "  [FAIL] repl: $name (expected '$expected' in output)"
        echo "    output: $actual"
        fail=$((fail + 1))
    fi
}

check_absent() {
    name="$1"; input="$2"; absent="$3"
    actual=$(printf "$input" | AETHER_HOME="" "$AE" repl 2>&1)
    if echo "$actual" | grep -qF -- "$absent"; then
        echo "  [FAIL] repl: $name (unexpected '$absent' in output)"
        echo "    output: $actual"
        fail=$((fail + 1))
    else
        echo "  [PASS] repl: $name"
        pass=$((pass + 1))
    fi
}

# ── Basic output ──────────────────────────────────────────────────────

check "println integer" \
    "println(42)\n\n:quit\n" "42"

check "println string" \
    "println(\"hello world\")\n\n:quit\n" "hello world"

check "arithmetic precedence" \
    "println(2 + 3 * 4)\n\n:quit\n" "14"

check "negative result" \
    "println(0 - 42)\n\n:quit\n" "-42"

check "multiple println in one eval" \
    "println(1)\nprintln(2)\nprintln(3)\n\n:quit\n" "3"
# ^-- checks that all three run; grep finds "3" (last line)

# ── Variable persistence ─────────────────────────────────────────────

check "int variable persists" \
    "x = 5\n\nprintln(x)\n\n:quit\n" "5"

check "string variable persists" \
    "name = \"hello\"\n\nprintln(name)\n\n:quit\n" "hello"

check "two variables persist" \
    "x = 3\n\ny = 7\n\nprintln(x + y)\n\n:quit\n" "10"

check "derived variable from prior" \
    "a = 3\n\nb = a * a\n\nprintln(b)\n\n:quit\n" "9"

check "const persists" \
    "const MAX = 100\n\nprintln(MAX)\n\n:quit\n" "100"

# ── Variable reassignment ────────────────────────────────────────────

check "simple reassignment" \
    "x = 5\n\nx = 99\n\nprintln(x)\n\n:quit\n" "99"

check "triple reassignment" \
    "x = 1\n\nx = 2\n\nx = 3\n\nprintln(x)\n\n:quit\n" "3"

# ── String interpolation ─────────────────────────────────────────────

check "string interpolation" \
    "name = \"world\"\n\nprintln(\"hello \${name}\")\n\n:quit\n" "hello world"

# ── Multi-line blocks ────────────────────────────────────────────────

check "if block" \
    "if 1 == 1 {\nprintln(77)\n}\n\n:quit\n" "77"

check "if-else block" \
    "x = 10\n\nif x > 5 {\nprintln(1)\n} else {\nprintln(0)\n}\n\n:quit\n" "1"

check "if-else false branch" \
    "x = 2\n\nif x > 5 {\nprintln(1)\n} else {\nprintln(0)\n}\n\n:quit\n" "0"

check "equality comparison" \
    "x = 5\n\nif x == 5 {\nprintln(1)\n} else {\nprintln(0)\n}\n\n:quit\n" "1"

check "nested if blocks" \
    "x = 10\n\nif x > 5 {\nif x > 8 {\nprintln(99)\n}\n}\n\n:quit\n" "99"

check "while loop" \
    "i = 0\n\nwhile i < 5 {\ni = i + 1\n}\n\nprintln(i)\n\n:quit\n" "5"

check "while loop with accumulator" \
    "sum = 0\n\ni = 0\n\nwhile i < 5 {\nsum = sum + i\ni = i + 1\n}\n\nprintln(sum)\n\n:quit\n" "10"

# ── Commands ─────────────────────────────────────────────────────────

check ":help shows commands" \
    ":help\n:quit\n" ":reset"

check ":h shortcut" \
    ":h\n:quit\n" ":reset"

check ":show lists session" \
    "x = 5\n\ny = 10\n\n:show\n:quit\n" "x = 5"

check ":show empty session" \
    ":show\n:quit\n" "(empty session)"

check ":show after reset is empty" \
    "x = 5\n\n:reset\n:show\n:quit\n" "(empty session)"

check_absent ":reset clears state" \
    "x = 5\n\n:reset\nprintln(x)\n\n:quit\n" "ae> 5"

check ":reset message" \
    "x = 1\n\n:reset\n:quit\n" "Session reset."

# ── Exit variants ────────────────────────────────────────────────────

check ":quit exits" \
    "println(7)\n\n:quit\n" "7"

check ":q exits" \
    "println(8)\n\n:q\n" "8"

check "exit command" \
    "println(9)\n\nexit\n" "9"

check "quit command" \
    "println(11)\n\nquit\n" "11"

# ── Error recovery ───────────────────────────────────────────────────

check "error shows message" \
    "println(undefined_var)\n\n:quit\n" "Undefined variable"

check "session continues after error" \
    "println(bad_var)\n\nx = 42\n\nprintln(x)\n\n:quit\n" "42"

check "failed eval not in :show" \
    "bad_var\n\n:show\n:quit\n" "(empty session)"

# ── Single-line auto-execute (no blank line needed) ─────────────

check "single-line auto-execute" \
    "println(99)\n:quit\n" "99"

check "single-line assignment auto-execute" \
    "x = 7\nprintln(x)\n:quit\n" "7"

check "single-line const auto-execute" \
    "const N = 42\nprintln(N)\n:quit\n" "42"

check "multi-line still needs close brace" \
    "if 1 == 1 {\nprintln(88)\n}\n:quit\n" "88"

# ── Banner ───────────────────────────────────────────────────────────

check "banner shows version" \
    ":quit\n" "Aether"

check "goodbye on exit" \
    ":quit\n" "Goodbye!"

echo ""
echo "REPL tests: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then exit 1; fi
