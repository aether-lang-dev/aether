#!/bin/sh
# #2683: `helper = helper() + 1`, a local named after the function its own
# initializer calls, got three different answers: `ae check` of the module on
# its own passed, the same code in a main file passed the checker and failed
# in gcc ("called object 'helper' is not a function", since a C declaration
# is in scope in its own initializer), and an importing program failed with
# E0301 "Undefined function 'helper'" (the merge left the call unrenamed).
#
# A local's scope starts after its initializer: the call is the function's,
# every later `helper` the local's. Each path now builds and runs that way,
# with the shapes around it: a function value passed before the binding, a
# local bound in both arms of an if (declared ahead of them), a closure local
# whose body calls the function, a compound assignment to the local, and C
# names that differ from the Aether one (`list.size` is `list_size`, an
# `@extern`, a selective import). A call through the local after its binding
# is an error that says the local hides the function.
#
# #3872: the local lasts to the end of its function, not of its block, as
# codegen has it: after the loop or branch body that first bound it, the
# call is still refused (it was accepted, and failed in the C compiler on
# an undeclared `ae_local_helper`). A closure's own local is the closure's:
# after the closure the function is called as before (codegen renamed the
# call past the closure, the same C failure).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

fail() { echo "  [FAIL] local_named_after_called_function: $1"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export AETHER_CACHE_DIR="$TMP/cache"
cd "$SCRIPT_DIR" || exit 1

out="$("$AE" check lib/shadowlib/module.ae 2>&1)" || { echo "$out"; fail "the module checked on its own was rejected"; }

out="$("$AE" run mainfile.ae 2>&1)" || { echo "$out"; fail "the main file did not build"; }
want="42 43 42 L! none 7
1 4 5 10"
[ "$out" = "$want" ] || { echo "$out"; fail "the main file printed the wrong values"; }

out="$("$AE" run imported.ae 2>&1)" || { echo "$out"; fail "the importing program did not build"; }
[ "$out" = "42 42 42 42" ] || { echo "$out"; fail "the importing program printed the wrong values"; }

out="$("$AE" check hidden.ae 2>&1)" && { echo "$out"; fail "a call through the local after its binding was accepted"; }
echo "$out" | grep -q "error\[E0301\]: 'helper' is a local here, which hides the function 'helper'" ||
    { echo "$out"; fail "the call through the local is not reported as hiding the function"; }

out="$("$AE" check hidden_past_body.ae 2>&1)" && { echo "$out"; fail "a call after the loop or branch that bound the local was accepted"; }
[ "$(echo "$out" | grep -c "error\[E0301\]: '\(helper\|other\)' is a local here, which hides the function")" = 2 ] ||
    { echo "$out"; fail "the calls after the loop and the branch are not both reported as hidden"; }

out="$("$AE" run closure_local.ae 2>&1)" || { echo "$out"; fail "a call after a closure with a local of the name did not build"; }
[ "$out" = "6 5" ] || { echo "$out"; fail "the closure's local or the function after it gave the wrong value"; }

echo "  [PASS] local_named_after_called_function: the call is the function's, wherever it sits"
