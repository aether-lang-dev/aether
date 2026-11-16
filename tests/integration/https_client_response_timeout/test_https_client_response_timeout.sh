#!/bin/sh
# An https request whose server completes the handshake, reads the request
# and answers nothing fails at the request's timeout (#3703), through each
# client TLS backend the build has. slow.ae's handler sleeps 4 s; the
# request's timeout is 500 ms; each round must fail within ROUND_LIMIT_MS.
# The pure-Aether client read a timed-out response 50 times over, 25 s for a
# 500 ms timeout, where the OpenSSL client fails at the first timeout.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=https_client_response_timeout
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
AE="$ROOT/build/ae$EXE"
[ -x "$AE" ] || { echo "  [SKIP] $NAME: build/ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no openssl to make a certificate"; exit 0; }
ROUND_LIMIT_MS=2500
RUN_LIMIT=60
. "$ROOT/tests/lib/server_jobs.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

# A path as the native programs here read it: openssl(1) and the program are
# Windows programs under MSYS2, where cygpath gives the form they open (and
# MSYS2_ARG_CONV_EXCL keeps "/CN=..." from being taken for a path).
native() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi
}
if ! MSYS2_ARG_CONV_EXCL="*" openssl req -x509 -newkey rsa:2048 \
        -keyout "$(native "$TMP/key.pem")" -out "$(native "$TMP/cert.pem")" \
        -days 1 -nodes -subj "/CN=localhost" >"$TMP/openssl.log" 2>&1; then
    echo "  [SKIP] $NAME: openssl req failed"
    exit 0
fi

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/slow.ae" -o "$TMP/slow$EXE" \
        >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] $NAME: build failed"
    sed 's/^/    /' "$TMP/build.log" | head -20
    exit 1
fi

CERT_PATH="$(native "$TMP/cert.pem")" KEY_PATH="$(native "$TMP/key.pem")" \
    "$TMP/slow$EXE" >"$TMP/run.log" 2>&1 &
pid=$!
waited=0
while server_alive "$pid"; do
    if [ "$waited" -ge "$RUN_LIMIT" ]; then
        kill_server "$pid"
        echo "  [FAIL] $NAME: the program did not finish in ${RUN_LIMIT}s"
        sed 's/^/    /' "$TMP/run.log" | head -10
        exit 1
    fi
    sleep 1
    waited=$((waited + 1))
done
wait "$pid"

if ! grep -q "^DONE" "$TMP/run.log"; then
    echo "  [FAIL] $NAME: the program did not finish"
    sed 's/^/    /' "$TMP/run.log" | head -10
    exit 1
fi

ran=0
while read -r tag backend what ms rest; do
    [ "$tag" = ROUND ] || continue
    [ "$what" = ELAPSED ] || continue
    ran=$((ran + 1))
    case "$rest" in
        "ERR none"*)
            echo "  [FAIL] $NAME: [$backend] a response, after the request's timeout"
            sed 's/^/    /' "$TMP/run.log" | head -10
            exit 1 ;;
    esac
    if [ "$ms" -ge "$ROUND_LIMIT_MS" ]; then
        echo "  [FAIL] $NAME: [$backend] gave up after ${ms} ms, past ${ROUND_LIMIT_MS} ms for a 500 ms timeout"
        sed 's/^/    /' "$TMP/run.log" | head -10
        exit 1
    fi
done < "$TMP/run.log"

if [ "$ran" -eq 0 ]; then
    echo "  [FAIL] $NAME: no TLS backend ran"
    sed 's/^/    /' "$TMP/run.log" | head -10
    exit 1
fi
echo "  [PASS] $NAME: $ran backend(s) failed a stalled response at the request's timeout"
