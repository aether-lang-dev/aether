#!/bin/sh
# Integration tests for contrib.host.ruby, CO-LOCATED with the bridge they
# test rather than in tests/integration/host_ruby/ (the convention the other
# bridges follow). Co-location is the direction argued in issue #1584: the
# test sits next to the code it describes, and a change to the bridge and its
# test is one diff in one directory.
#
# WHY THIS EXISTS: the bridge's OWN embedding lifecycle had no direct test.
# Ruby is not uncovered — tests/integration/namespace_ruby/ exercises the
# generated FFI SDK over Fiddle, and tests/sandbox/test_shared_map_all.sh
# covers ruby_run_sandboxed_with_map — but both drive Ruby from the OUTSIDE.
# Nothing exercised ruby_init_host / ruby_run / ruby_finalize_host directly,
# which is why the post-finalize re-init crash below went unnoticed.
#
# It is also the piece the NIGHTLY misses: contrib_check.sh has no host/*
# entries at all, and contrib_host_demos.sh reports `ruby SKIP (no demo)`,
# so on that box the bridge got a 7ms `ae check` of module.ae and a
# `-fsyntax-only` of the C shim, and nothing executed.
#
# LOADING MODEL: the bridge dlopens libruby rather than linking it, so the
# soname must be resolved at runtime. Debian ships an unversioned
# libruby.so symlink; Fedora/Bazzite do not, which is why the bridge
# documents `ruby -rrbconfig -e 'print RbConfig::CONFIG["LIBRUBY_SO"]'` as
# the orchestrator's probe. This script performs exactly that probe and
# exports AETHER_RUBY_SONAME, so it works on both packagings.
#
# Driven from C rather than through `ae build`: it exercises the same surface
# module.ae declares, without depending on contrib-link plumbing — matching
# tests/integration/host_lua_bidirectional/.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) echo "  [SKIP] host_ruby on Windows"; exit 0 ;;
esac

fail() {
    echo "  [FAIL] host_ruby: $1"
    [ -n "${2:-}" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -12
    exit 1
}

# ---- dependency probes: SKIP (not FAIL) when ruby isn't provisioned -------
command -v ruby >/dev/null 2>&1 || { echo "  [SKIP] host_ruby: no ruby runtime"; exit 0; }

RUBY_CFLAGS=""
if pkg-config --exists ruby 2>/dev/null; then
    RUBY_CFLAGS="$(pkg-config --cflags ruby)"
else
    # Debian/Ubuntu without the .pc: headers live in a versioned dir.
    for d in /usr/include/ruby-*; do
        [ -f "$d/ruby.h" ] && { RUBY_CFLAGS="-I$d"; break; }
    done
fi
[ -n "$RUBY_CFLAGS" ] || { echo "  [SKIP] host_ruby: no ruby dev headers"; exit 0; }

# The bridge's own documented soname probe.
RUBY_SONAME="$(ruby -rrbconfig -e 'print RbConfig::CONFIG["LIBRUBY_SO"]' 2>/dev/null)"
[ -n "$RUBY_SONAME" ] || { echo "  [SKIP] host_ruby: could not probe LIBRUBY_SO"; exit 0; }
export AETHER_RUBY_SONAME="$RUBY_SONAME"

[ -f "$ROOT/build/libaether.a" ] || { echo "  [SKIP] host_ruby: libaether.a not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# ---- build the harness ----------------------------------------------------
# ruby.h's own headers are noisy under -Wall -Wextra; this test is about the
# bridge's behaviour, not Ruby's header hygiene.
if ! gcc -o "$TMP/t" \
    "$SCRIPT_DIR/aether_host_ruby.c" \
    "$ROOT/runtime/aether_sandbox.c" \
    "$ROOT/runtime/aether_shared_map.c" \
    $RUBY_CFLAGS -I"$ROOT" -I"$ROOT/runtime" \
    -DAETHER_HAS_RUBY -DAETHER_HAS_SANDBOX \
    -L"$ROOT/build" -laether -ldl -lm -lrt -lpthread \
    -Wno-discarded-qualifiers \
    -xc - 2>"$TMP/cc.log" << 'CEOF'
#include <stdio.h>
#include <string.h>
#include "contrib/host/ruby/aether_host_ruby.h"

/* The bridge links against libaether; these are the symbols an `ae`-built
   program would normally supply. */
void aether_args_init(int a, char** v){ (void)a; (void)v; }
void* _aether_ctx_stack[64];
int _aether_ctx_depth = 0;

int main(void) {
    if (ruby_init_host() != 0) { printf("FAIL init_host\n"); return 1; }

    /* 1. Plain evaluation. */
    if (ruby_run("puts 'RB-HELLO'") != 0) { printf("FAIL run\n"); return 1; }

    /* 2. Ruby-side computation reaching stdout — proves the interpreter is
          really executing, not just loading. */
    if (ruby_run("puts (2 + 3) * 7") != 0) { printf("FAIL run-arith\n"); return 1; }

    /* 3. A raised exception must be reported as failure, not silently
          swallowed. A bridge that returns 0 for broken script code is worse
          than one that cannot run at all: the caller believes it worked. */
    int bad = ruby_run("raise 'deliberate'");
    printf("EXC-RC=%d\n", bad);

    /* 4. Repeated init is a no-op while the VM is live — a host that calls
          init defensively before each eval must not pay for it or crash. */
    if (ruby_init_host() != 0) { printf("FAIL reinit-live\n"); return 1; }
    if (ruby_run("puts 'RB-STILL-LIVE'") != 0) { printf("FAIL run-after-reinit\n"); return 1; }

    /* 5. Finalize once, at the end. NOT followed by another init: CRuby's
          ruby_finalize() tears the VM down permanently and ruby_init()
          cannot be called again in the same process. The bridge currently
          returns 0 from a post-finalize ruby_init_host() and then segfaults
          on the next eval — see the note in README.md. This test
          deliberately does NOT exercise that path; it pins the supported
          lifecycle so a future fix has a baseline to change. */
    ruby_finalize_host();

    printf("DONE\n");
    return 0;
}
CEOF
then
    fail "harness did not compile" "$TMP/cc.log"
fi

# ---- run ------------------------------------------------------------------
if ! "$TMP/t" > "$TMP/out.log" 2>&1; then
    fail "harness exited non-zero" "$TMP/out.log"
fi

grep -q "RB-HELLO" "$TMP/out.log" \
    || fail "ruby_run did not execute the script" "$TMP/out.log"
grep -q "^35$" "$TMP/out.log" \
    || fail "ruby-side arithmetic did not reach stdout (interpreter not really running?)" "$TMP/out.log"
grep -q "RB-STILL-LIVE" "$TMP/out.log" \
    || fail "a repeated init while the VM is live broke the embedding" "$TMP/out.log"
grep -q "DONE" "$TMP/out.log" \
    || fail "harness did not reach the end" "$TMP/out.log"

# A raised Ruby exception must NOT be reported as success.
if grep -q "^EXC-RC=0$" "$TMP/out.log"; then
    fail "a raising script returned rc=0 — failures are being swallowed" "$TMP/out.log"
fi

echo "  [PASS] host_ruby: eval, compute, exception-propagates, idempotent init"
exit 0
