#!/bin/sh
# #3138: finite bridge probe, suitable for checking refused-handshake leaks.
set -eu
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] tls_ownership: POSIX descriptor reuse probe"
        exit 0 ;;
esac
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export AETHER_CACHE_DIR="$TMP/cache"
export AETHER_HOME="$ROOT"
. "$ROOT/tests/lib/test_openssl_conf.sh"
test_openssl_conf "$TMP"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "$TMP/key.pem" -out "$TMP/cert.pem" -days 2 -nodes \
    -subj /CN=localhost >"$TMP/cert.log" 2>&1
cat >"$TMP/probe.ae" <<'AE'
import std.cryptography.tls13_server
import std.os
extern test_tls_ownership(cert: string, key: string) -> int
extern exit(code: int)
main() {
    exit(test_tls_ownership(os_getenv("CERT_PATH"), os_getenv("KEY_PATH")))
}
AE
"$ROOT/build/ae" build "$TMP/probe.ae" \
    --extra "$ROOT/tests/integration/https_pure_tls_vertical/ownership.c" \
    -o "$TMP/probe" >"$TMP/build.log" 2>&1 || { cat "$TMP/build.log"; exit 1; }
CERT_PATH="$TMP/cert.pem" KEY_PATH="$TMP/key.pem" "$TMP/probe"
# Not under ASan: its shadow memory cannot be set up under valgrind, and
# LeakSanitizer checks the same run for leaks (#3539).
if command -v valgrind >/dev/null 2>&1 && [ "${AETHER_SANITIZER:-}" != asan ]; then
    CERT_PATH="$TMP/cert.pem" KEY_PATH="$TMP/key.pem" \
    valgrind --leak-check=full --show-leak-kinds=definite \
        --errors-for-leak-kinds=definite --error-exitcode=1 \
        --log-file="$TMP/valgrind.log" "$TMP/probe" \
        || { cat "$TMP/valgrind.log"; exit 1; }
fi
