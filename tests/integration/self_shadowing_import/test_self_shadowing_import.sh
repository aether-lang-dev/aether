#!/bin/sh
# Regression for #1780: a module named X that does `import std.X` (no alias)
# self-shadows — a qualified `X.name(...)` resolves to the CURRENT module first,
# so a wrapper forwarding to the stdlib function of the same name calls itself
# and segfaults on infinite recursion with NO diagnostic.
#
# Fix has two parts, both checked here:
#   (1) a warning at the import site naming the trap + the alias fix;
#   (2) that warning must survive `ae run` (aetherc warnings were being dropped
#       by the run/build path's quiet compile — now shown).
# Plus a control: a module NOT named `audio` importing std.audio is silent.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

fail=0

# (1)+(2): the self-shadowing module warns, and the warning is visible via `ae run`.
log="$(cd "$SCRIPT_DIR" && "$AE" run main.ae 2>&1)"
if ! printf '%s' "$log" | grep -qi "resolves to THIS module"; then
    echo "  [FAIL] self-shadowing import produced no visible warning via 'ae run'"
    printf '%s\n' "$log" | sed 's/^/        /' | head -4
    fail=1
elif ! printf '%s' "$log" | grep -qi "import .* as"; then
    echo "  [FAIL] warning does not point at the alias fix"
    fail=1
else
    echo "  [PASS] self-shadowing import warns (visible in 'ae run') and names the alias fix"
fi

# control: a module named 'sound' importing std.audio must NOT warn.
log2="$(cd "$SCRIPT_DIR" && "$AE" run main_ok.ae 2>&1)"
if printf '%s' "$log2" | grep -qi "resolves to THIS module"; then
    echo "  [FAIL] false-positive self-shadow warning for a non-matching module name"
    fail=1
else
    echo "  [PASS] no false warning for a module whose name differs from the std import"
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS: #1780 self-shadowing import diagnostic"
    exit 0
fi
echo "FAIL: #1780"
exit 1
