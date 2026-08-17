#!/bin/sh
# `ae --version` must describe THIS binary, not a pin, and must name the aetherc
# it would actually run. Both halves were wrong (#1602): a stale binary reported
# whatever ~/.aether/active_version claimed, and an `ae` beside an `aetherc`
# from another install said nothing about the mismatch that decides codegen.
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
AE="$ROOT/build/ae${EXE_EXT:-}"
[ -x "$AE" ] || { echo "  [SKIP] version_identity: $AE not built"; exit 0; }
[ -f "$ROOT/VERSION" ] || { echo "  [SKIP] version_identity: no VERSION file"; exit 0; }

want="$(tr -d '[:space:]' < "$ROOT/VERSION")"
pass=0; fail=0
check() { if [ "$2" = "$3" ]; then echo "  [PASS] $1"; pass=$((pass+1)); else
    echo "  [FAIL] $1: got '$2', want '$3'"; fail=$((fail+1)); fi; }

out="$("$AE" --version 2>&1)"
got="$(printf '%s\n' "$out" | sed -n 's/^ae \([0-9][0-9.]*\).*/\1/p' | head -1)"
check "reports the version it was built from" "$got" "$want"

# The binary's own path, so an operator can tell two installs apart.
case "$out" in *"Binary:"*) echo "  [PASS] names its own path"; pass=$((pass+1));;
    *) echo "  [FAIL] no Binary: line"; fail=$((fail+1));; esac

# The compiler that does the codegen, with its version.
case "$out" in *"aetherc:"*) echo "  [PASS] names the aetherc it would run"; pass=$((pass+1));;
    *) echo "  [FAIL] no aetherc: line"; fail=$((fail+1));; esac

# A pin that disagrees is reported as a pin, not as the version.
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
# Both: the home directory is USERPROFILE on Windows and HOME elsewhere.
HOME="$tmp"; USERPROFILE="$tmp"; export HOME USERPROFILE
mkdir -p "$tmp/.aether"
echo "0.0.1" > "$tmp/.aether/active_version"
pinned_out="$("$AE" --version 2>&1)"
pinned_got="$(printf '%s\n' "$pinned_out" | sed -n 's/^ae \([0-9][0-9.]*\).*/\1/p' | head -1)"
check "a pin does not change the reported version" "$pinned_got" "$want"
case "$pinned_out" in *"pins 0.0.1"*) echo "  [PASS] the disagreeing pin is called out"; pass=$((pass+1));;
    *) echo "  [FAIL] the pin was not reported"; fail=$((fail+1));; esac

echo ""
echo "version_identity: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
