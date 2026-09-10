#!/bin/sh
# Integration test for std.http.script_gateway — issue #384.
#
# Builds script.ae as a shared library via `aetherc --emit=lib`,
# builds host.ae as a normal executable that mounts the .so via
# `script_gateway.mount`, then drives requests with curl to verify:
#
#   1. GET /gateway/anything            — runs the .so handler
#                                         (X-Handler: script_gateway,
#                                          body "hello from script.so")
#   2. GET /static                      — runs the static fallback
#                                         (X-Handler: static)
#   3. Mount of a missing .so           — fails with KIND_NOT_FOUND
#                                         (host exits non-zero, message
#                                          contains "so_path not readable")
#   4. Mount of a .so missing the
#      aether_script_handle entrypoint  — fails with KIND_INVALID
#
# DESIGN — same defensive shape the proxy_pool tests landed in:
#   - No `set -e` (transient curl flakes shouldn't kill the script
#     silently; explicit `fail` calls do the right thing).
#   - SIGKILL teardown via `kill -9` (TerminateProcess on MSYS2).
#   - Polling-based wait_for_port (not sleep N).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"
    exit 0
fi

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  [SKIP] script_gateway: Windows DLL hosting is a follow-up (KIND_UNAVAILABLE)"
        exit 0
        ;;
esac

TMPDIR="$(mktemp -d)"
HOST_PID=""

cleanup() {
    if [ -n "$HOST_PID" ]; then
        kill -9 "$HOST_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

PORT=18454
BAD_PORT=18455
URL="http://127.0.0.1:$PORT"
BAD_URL="http://127.0.0.1:$BAD_PORT"

fail() { echo "  [FAIL] $1"; exit 1; }

# Pick the right shared-library extension per platform; aetherc
# --emit=lib -fPIC -shared produces .dylib on macOS, .so elsewhere.
case "$(uname -s)" in
    Darwin) SO_EXT=".dylib" ;;
    *)      SO_EXT=".so"    ;;
esac

# --- Build script.so via aetherc --emit=lib ---
SO_PATH="$TMPDIR/script$SO_EXT"
if ! AETHER_HOME="$ROOT" "$AETHERC" --emit=lib --with=net \
        "$SCRIPT_DIR/script.ae" "$TMPDIR/script.c" \
        2>"$TMPDIR/aetherc.err"; then
    echo "  [FAIL] aetherc --emit=lib script.ae:"
    head -30 "$TMPDIR/aetherc.err"
    exit 1
fi
# Compile the emitted C to a shared library, linking against the
# stdlib archive we already built. -fPIC + -shared produces a
# dlopen()-able artifact; -ldl is harmless on macOS (no-op libdl
# stub) and required on older Linux glibc (< 2.34).
# Leave runtime symbols (http_response_set_*, string_concat, etc.)
# unresolved in the .so — they're satisfied at dlopen time from
# the host process's libaether.a. macOS ld defaults to
# -undefined,error; we need dynamic_lookup. On Linux/glibc
# unresolved-by-default-on-shared is fine, but be defensive.
case "$(uname -s)" in
    Darwin) SHARED_LDFLAGS="-Wl,-undefined,dynamic_lookup" ;;
    *)      SHARED_LDFLAGS="" ;;
esac
if ! gcc -fPIC -shared -O2 \
        -I"$ROOT/runtime" -I"$ROOT/runtime/actors" \
        -I"$ROOT/runtime/scheduler" -I"$ROOT/runtime/utils" \
        -I"$ROOT/runtime/memory" -I"$ROOT/runtime/config" \
        -I"$ROOT/std" -I"$ROOT/std/string" -I"$ROOT/std/io" \
        -I"$ROOT/std/math" -I"$ROOT/std/net" -I"$ROOT/std/collections" \
        -I"$ROOT/std/json" \
        "$TMPDIR/script.c" \
        $SHARED_LDFLAGS \
        -o "$SO_PATH" 2>"$TMPDIR/gcc.err"; then
    echo "  [FAIL] gcc -shared:"
    head -30 "$TMPDIR/gcc.err"
    exit 1
fi
if ! [ -f "$SO_PATH" ]; then
    fail "$SO_PATH not produced"
fi

# --- Build host.ae as a normal executable ---
if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/host.ae" \
        -o "$TMPDIR/host" >"$TMPDIR/host.build.log" 2>&1; then
    echo "  [FAIL] ae build host.ae:"
    head -30 "$TMPDIR/host.build.log"
    exit 1
fi

# --- Test 1: happy path — script.so dispatches /gateway/* ---
"$TMPDIR/host" "$PORT" "$SO_PATH" >"$TMPDIR/host.log" 2>&1 &
HOST_PID=$!

# Poll the listener — expect READY in the host's log. 15s, the deadline the
# rest of the integration suite uses: this ran with 5s and timed out whenever
# the parallel sweep loaded the box, reporting a startup that had in fact
# succeeded. A deadline costs nothing when the host starts promptly.
deadline=$(($(date +%s) + 15))
ready=0
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -q '^READY$' "$TMPDIR/host.log" 2>/dev/null; then
        ready=1
        break
    fi
    if ! kill -0 "$HOST_PID" 2>/dev/null; then
        echo "  [FAIL] host died during startup:"
        head -30 "$TMPDIR/host.log"
        exit 1
    fi
    sleep 0.1
done
if [ "$ready" != "1" ]; then
    echo "  [FAIL] host did not reach READY:"
    head -30 "$TMPDIR/host.log"
    exit 1
fi

# Need a few more ms for the listen socket to actually accept.
deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if curl -s -o /dev/null --max-time 1 --connect-timeout 0.3 "$URL/static" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

# 1. /gateway/anything → script.so handler.
HDRS1=$(curl -s -D - -o "$TMPDIR/body1" --max-time 3 "$URL/gateway/foo" 2>/dev/null)
echo "$HDRS1" | grep -qiE '^X-Handler: ?script_gateway' || \
    fail "gateway: missing X-Handler: script_gateway header"
grep -q '^hello from script.so$' "$TMPDIR/body1" || \
    fail "gateway: body did not match expected greeting"

# 2. /static → static fallback (gateway must NOT short-circuit it).
HDRS2=$(curl -s -D - -o "$TMPDIR/body2" --max-time 3 "$URL/static" 2>/dev/null)
echo "$HDRS2" | grep -qiE '^X-Handler: ?static' || \
    fail "static: missing X-Handler: static header (gateway over-matched?)"
grep -q '^static fallback$' "$TMPDIR/body2" || \
    fail "static: body mismatch"

# Tear down the happy-path host before the negative cases.
kill -9 "$HOST_PID" 2>/dev/null || true
wait "$HOST_PID" 2>/dev/null || true
HOST_PID=""

# --- Test 3: missing .so path — host exits non-zero with KIND_NOT_FOUND ---
MISSING_SO="$TMPDIR/does_not_exist$SO_EXT"
"$TMPDIR/host" "$BAD_PORT" "$MISSING_SO" >"$TMPDIR/host_missing.log" 2>&1
RC=$?
if [ "$RC" = "0" ]; then
    fail "missing .so: host exited 0 (expected non-zero)"
fi
if ! grep -q "kind=1" "$TMPDIR/host_missing.log"; then
    echo "  [FAIL] missing .so: expected kind=1 (KIND_NOT_FOUND) in log:"
    head -10 "$TMPDIR/host_missing.log"
    exit 1
fi

# --- Test 4: .so without aether_script_handle — KIND_INVALID ---
# Build a .so that has NO aether_script_handle export. We compile a
# trivial C file directly via gcc — no aetherc involvement needed.
cat > "$TMPDIR/empty.c" <<'EOF'
/* Intentionally empty — exports no symbols beyond the implicit ones
   the linker generates. The script_gateway dlsym() must fail with
   "missing aether_script_handle entrypoint" → KIND_INVALID. */
int aether_unrelated_symbol(void) { return 0; }
EOF
EMPTY_SO="$TMPDIR/empty$SO_EXT"
gcc -fPIC -shared -O0 "$TMPDIR/empty.c" -o "$EMPTY_SO" 2>"$TMPDIR/gcc_empty.err" \
    || fail "gcc -shared empty.c"

"$TMPDIR/host" "$BAD_PORT" "$EMPTY_SO" >"$TMPDIR/host_empty.log" 2>&1
RC=$?
if [ "$RC" = "0" ]; then
    fail "empty .so: host exited 0 (expected non-zero)"
fi
if ! grep -q "kind=6" "$TMPDIR/host_empty.log"; then
    echo "  [FAIL] empty .so: expected kind=6 (KIND_INVALID) in log:"
    head -10 "$TMPDIR/host_empty.log"
    exit 1
fi

echo "  [PASS] http_script_gateway: 4/4 — happy path, static fallback, missing .so, no entrypoint"
