#!/bin/sh
# #1598: when codegen renames a top-level function, every reference has to
# move with the definition — calls AND bare value references.
#
# Two passes rename functions, and both shared the call-only rewrite:
#   * rename_leading_underscore_functions (#279)  `_h`   -> `ae_h`
#   * rename_extern_colliding_functions   (#1366) `puts` -> `ae_puts`
#
# The underscore half is pinned by tests/regression/ (it fails to compile,
# which the bulk .ae sweep catches). This directory pins the EXTERN half,
# which needs a C sidecar to exercise — and which failed far more quietly:
# the un-renamed reference resolved to the real libc symbol the extern
# declared, so the program linked cleanly and SEGFAULTED at runtime,
# handing libc's puts(const char*) an int. Verified on the pre-fix
# compiler: "Program crashed (signal 11: segmentation fault)".
#
# So this is a runtime test on purpose: a compile-only check would have
# passed on the broken compiler.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"

[ -x "$AE" ] || { echo "  [SKIP] fn_value_rename: ae not built"; exit 0; }

cd "$ROOT" || exit 1
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() {
    echo "  [FAIL] fn_value_rename: $1"
    [ -f "$TMP/out.log" ] && sed 's/^/        /' "$TMP/out.log"
    exit 1
}

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" \
    --extra "$SCRIPT_DIR/support.c" >"$TMP/out.log" 2>&1 || fail "probe did not run"

# The probe prints its own verdict; a segfault or a wrong value would show
# up as a missing PASS line rather than a diff.
grep -q "PASS: extern-colliding fn correct as value and as call" "$TMP/out.log" \
    || fail "extern-colliding function was wrong as a value or as a call"

echo "  [PASS] fn_value_rename: renamed fn correct as value and as call (#1598)"
exit 0
