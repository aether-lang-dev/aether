#!/bin/sh
# Issue #1025: the build cache must invalidate when an imported lib MODULE is
# edited, in both the default-lib and explicit --lib layouts.
#
#   Bug A: with no --lib flag, the compiler still searches the default `lib/`
#          dir, but compute_cache_key never walked it, so editing a module
#          there served a stale binary until `ae cache clear`.
#   Bug B: the --lib walk keyed on mtime(seconds)+size, so a same-second,
#          same-size edit (a one-char constant flip in an editor-save loop)
#          was missed and the stale binary was served.
#
# Both are exercised end-to-end here: build, edit the module, rebuild, and
# assert the output reflects the edit. The workspace and the build cache are
# isolated in temp dirs (HOME override) so the test neither pollutes nor
# depends on the developer's ~/.aether/cache, and run 1 is always a fresh miss.
#
# Bug B is made deterministic (would fail on the pre-fix code) by restoring the
# module's pre-edit mtime with `touch -r` after a same-length edit: identical
# mtime AND size, so only content-hashing catches the change.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

WORK="$(mktemp -d)"
HOME_ISO="$(mktemp -d)"
trap 'rm -rf "$WORK" "$HOME_ISO"' EXIT INT TERM

mkdir -p "$WORK/lib/greet" "$WORK/src"
cat > "$WORK/src/main.ae" <<'AEEOF'
import greet
main() { println(greet.msg()) }
AEEOF

mod="$WORK/lib/greet/module.ae"
# AETHER_CACHE_DIR as well as HOME: on Windows the cache follows USERPROFILE,
# not HOME, and the lib-dir walk runs there too since #1235.
run() { ( cd "$WORK" && HOME="$HOME_ISO" AETHER_CACHE_DIR="$HOME_ISO/.aether/cache" \
          "$AE" run "$@" 2>&1 | grep -v 'command not found' ) }

fail() { echo "  [FAIL] cache_lib_invalidation: $1"; exit 1; }

# ---- Bug A: default lib/ (no --lib) ----
printf 'exports (msg)\nmsg() -> string { return "OLD" }\n' > "$mod"
a1="$(run src/main.ae | tail -1)"
[ "$a1" = "OLD" ] || fail "Bug A run 1: expected OLD, got '$a1'"
printf 'exports (msg)\nmsg() -> string { return "NEW" }\n' > "$mod"
a2="$(run src/main.ae | tail -1)"
[ "$a2" = "NEW" ] || fail "Bug A run 2 (default lib/ edit not seen): expected NEW, got '$a2'"

# ---- Bug B: explicit --lib, identical mtime AND size, changed content ----
printf 'exports (msg)\nmsg() -> string { return "V1" }\n' > "$mod"
b1="$(run --lib lib src/main.ae | tail -1)"
[ "$b1" = "V1" ] || fail "Bug B run 1: expected V1, got '$b1'"
touch -r "$mod" "$WORK/.mtimeref"                 # snapshot the pre-edit mtime
printf 'exports (msg)\nmsg() -> string { return "V2" }\n' > "$mod"   # same length as V1
touch -r "$WORK/.mtimeref" "$mod"                 # restore mtime: now identical mtime+size
b2="$(run --lib lib src/main.ae | tail -1)"
[ "$b2" = "V2" ] || fail "Bug B run 2 (same-mtime same-size edit not seen): expected V2, got '$b2'"

# ---- sanity: an unchanged rebuild still returns the right output ----
b3="$(run --lib lib src/main.ae | tail -1)"
[ "$b3" = "V2" ] || fail "sanity: unchanged rebuild returned '$b3', expected V2"

echo "  [PASS] cache_lib_invalidation: default-lib and same-mtime/size --lib module edits invalidate the cache"
exit 0
