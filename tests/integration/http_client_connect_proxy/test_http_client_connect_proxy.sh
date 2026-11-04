#!/bin/sh
# An https request through a forward proxy, end to end (#3557): CONNECT to
# a relaying proxy, the proxy's reply read to its head and no further, and
# TLS through the tunnel to the origin, whose certificate the client pins.
# Also a 407 refusal and a reply whose status has four digits, which the
# client refuses as malformed. See connect_proxy.ae.
#
# Once with each TLS backend where the build has both: the default one, and
# the pure-Aether client and server (AETHER_PURE_TLS=1). Skips when there is
# no openssl to make the certificate.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] http_client_connect_proxy: ae not built"; exit 0; }
if ! command -v openssl >/dev/null 2>&1; then
    echo "  [SKIP] http_client_connect_proxy: no openssl to make a certificate"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  [FAIL] http_client_connect_proxy: $1"; exit 1; }

. "$ROOT/tests/lib/test_openssl_conf.sh"
test_openssl_conf "$TMP"
# MSYS2_ARG_CONV_EXCL keeps the subject from being rewritten as a path.
MSYS2_ARG_CONV_EXCL='/CN=' \
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "$TMP/key.pem" -out "$TMP/cert.pem" -days 2 -nodes \
    -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" >"$TMP/ssl.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/ssl.log"; fail "could not make a test certificate"; }

AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/connect_proxy.ae" -o "$TMP/connect_proxy" >"$TMP/build.log" 2>&1 \
    || { sed -n '1,20p' "$TMP/build.log"; fail "connect_proxy.ae did not build"; }

# run LABEL [env...]: one pass; its last line is PASS: or SKIP:.
run() {
    _label="$1"; shift
    env CERT_PATH="$TMP/cert.pem" KEY_PATH="$TMP/key.pem" "$@" "$TMP/connect_proxy" >"$TMP/run.log" 2>&1
    _rc=$?
    _last="$(tail -n 1 "$TMP/run.log" | tr -d '\r')"
    case "$_last" in
        PASS:*) [ "$_rc" -eq 0 ] || { sed 's/^/    /' "$TMP/run.log"; fail "$_label: PASS with exit $_rc"; }
                NOTES="$NOTES $_label" ;;
        SKIP:*) NOTES="$NOTES ($_label skipped: ${_last#SKIP: })" ;;
        *)      sed 's/^/    /' "$TMP/run.log"; fail "$_label" ;;
    esac
}

NOTES=""
run "default TLS"
run "pure TLS 1.3" AETHER_PURE_TLS=1
echo "  [PASS] http_client_connect_proxy:$NOTES"
