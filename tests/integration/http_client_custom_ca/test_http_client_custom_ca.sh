#!/bin/sh
# Regression: #1107 (set_cafile) + #1110 (the pinned CA must be the TRUST
# anchor, not merely loaded). Starts an Aether TLS server whose leaf cert is
# signed by a private root CA (SAN IP:127.0.0.1) — the Proxmox-VE topology —
# then runs an Aether client that hits it three ways: pin the correct ROOT CA
# (must verify -> 200), pin a WRONG CA (must fail closed), and no cafile (system
# store must reject the private chain). This is the "verify, but against THIS
# cert" path — strictly stronger than set_insecure. See client.ae.
#
# Skips cleanly when openssl is missing or the build has no OpenSSL.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_client_custom_ca — TLS code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

if ! command -v openssl >/dev/null 2>&1; then
    echo "  [SKIP] openssl not on PATH"
    exit 0
fi

TMPDIR="$(mktemp -d)"
cleanup() {
    stop_server "${SRV_PID:-}"
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# The server binds port 0, so there is no port to keep unique any more. That
# also defuses the reason this test used to need one: `ae run` forks the server
# child and the trap cannot fully reap it, and a lingering server used to starve
# the next test of its fixed port. A leftover now holds a port nobody wants.

# Real CA-signs-a-separate-leaf topology, mirroring a Proxmox VE endpoint
# (private root CA `pve-root-ca.pem` that signs a distinct server cert). This
# is the case that exposed #1110: the server presents LEAF, the client pins the
# ROOT CA — so verification must build LEAF -> CA and trust CA. A self-signed
# cert (which is its own anchor) would NOT have caught the bug; a real chain
# does, because the pinned CA has to be the trust anchor, not just present.
CA="$TMPDIR/ca.pem"          # root CA — this is what the client pins (GOOD_CA)
CAKEY="$TMPDIR/ca.key"
CERT="$TMPDIR/leaf.pem"      # server's leaf cert, SIGNED BY the CA
KEY="$TMPDIR/leaf.key"
BADCA="$TMPDIR/badca.pem"    # an unrelated CA (WRONG CA the client also tries)

# 1. root CA
if ! openssl req -x509 -newkey rsa:2048 -keyout "$CAKEY" -out "$CA" -days 1 \
        -nodes -subj "/CN=Aether Test Root CA" 2>"$TMPDIR/o1.err"; then
    echo "  [SKIP] openssl (root CA) failed:"; head -5 "$TMPDIR/o1.err"; exit 0
fi
# 2. leaf key + CSR, then sign with the CA, SAN IP:127.0.0.1 (server binds IP).
if ! openssl req -newkey rsa:2048 -keyout "$KEY" -out "$TMPDIR/leaf.csr" \
        -nodes -subj "/CN=127.0.0.1" 2>"$TMPDIR/o2.err" \
   || ! printf 'subjectAltName=IP:127.0.0.1\n' > "$TMPDIR/ext.cnf" \
   || ! openssl x509 -req -in "$TMPDIR/leaf.csr" -CA "$CA" -CAkey "$CAKEY" \
        -CAcreateserial -out "$CERT" -days 1 -sha256 -extfile "$TMPDIR/ext.cnf" \
        2>"$TMPDIR/o3.err"; then
    echo "  [SKIP] openssl (sign leaf) failed:"; head -5 "$TMPDIR/o3.err"; exit 0
fi
# 3. an unrelated root CA — the "wrong CA" the client pins to prove fail-closed.
if ! openssl req -x509 -newkey rsa:2048 -keyout "$TMPDIR/badkey.pem" \
        -out "$BADCA" -days 1 -nodes -subj "/CN=Wrong CA" 2>"$TMPDIR/o4.err"; then
    echo "  [SKIP] openssl (bad CA) failed:"; head -5 "$TMPDIR/o4.err"; exit 0
fi

# Sanity: the couriered CA verifies the leaf via the openssl CLI (ground truth,
# exactly what the #1110 report checked). If this fails, skip — env problem.
if ! openssl verify -CAfile "$CA" "$CERT" >/dev/null 2>&1; then
    echo "  [SKIP] openssl verify -CAfile did not accept the leaf (env issue)"
    exit 0
fi

AETHER_HOME="$ROOT" CERT_PATH="$CERT" KEY_PATH="$KEY" \
    "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

deadline=$(($(date +%s) + 30))
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -q READY "$TMPDIR/srv.log" 2>/dev/null && break
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        if grep -q "TLS unavailable" "$TMPDIR/srv.log" 2>/dev/null; then
            echo "  [SKIP] build has no OpenSSL"
            exit 0
        fi
        echo "  [FAIL] server died before READY:"
        head -20 "$TMPDIR/srv.log"
        exit 1
    fi
    sleep 0.1
done
if ! grep -q READY "$TMPDIR/srv.log" 2>/dev/null; then
    if grep -q "TLS unavailable" "$TMPDIR/srv.log" 2>/dev/null; then
        echo "  [SKIP] build has no OpenSSL"
        exit 0
    fi
    echo "  [FAIL] server never reported READY:"
    head -20 "$TMPDIR/srv.log"
    exit 1
fi

PORT=$(read_ready_port "$TMPDIR/srv.log") || exit 1
wait_port "$PORT" || exit 1

OUT="$TMPDIR/client.out"
# GOOD_CA is the ROOT CA (not the leaf): the client pins the CA and the server
# presents the leaf, so verification must build the chain and trust the CA —
# the #1110 case.
if ! AETHER_HOME="$ROOT" GOOD_CA="$CA" BAD_CA="$BADCA" \
        AE_TEST_PORT="$PORT" "$AE" run "$SCRIPT_DIR/client.ae" >"$OUT" 2>&1; then
    echo "  [FAIL] client exited non-zero:"
    head -10 "$OUT"
    exit 1
fi

if grep -q '^PASS ' "$OUT"; then
    echo "  [PASS] http_client_custom_ca: correct CA verifies, wrong CA fails closed, default rejects"
    exit 0
fi

echo "  [FAIL] client did not report PASS:"
head -10 "$OUT"
exit 1
