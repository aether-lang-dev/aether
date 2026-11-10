#!/bin/sh
# #2675: on Windows the two HTTPS clients trust the same anchors, in the same
# order, and the Windows certificate store is one of them.
#
# Neither client read the store, so a program copied to a machine without
# MSYS2 or Git for Windows could make no HTTPS request, and they looked for
# different bundle files: std.http.client's OpenSSL path found the bundle the
# release ships beside a program (<root>\bin\prog.exe ->
# <root>\share\ssl\ca-bundle.crt) and MSYS2's UCRT64 one, the pure-Aether
# client neither; the pure client failed closed on an SSL_CERT_FILE it could
# not load, OpenSSL fell through to the bundle files. Both now take
# SSL_CERT_FILE / SSL_CERT_DIR alone when either is set, else the ROOT system
# store together with the first bundle file of one shared list.
#
# Everything is local: a throwaway CA and a leaf it signs, our own std.http
# server, and a client program built into <tmp>/app/bin/ with the CA as the
# bundle beside it, under <tmp>/app/share/ssl/. Each case runs on both stacks
# (OpenSSL where the build has it, and AETHER_PURE_TLS=1). The machine's
# certificate store is only read, never changed: a server whose CA is only in
# the store would need a private key for one of its roots, so the store is
# checked by reading it, against what PowerShell lists for Cert:\CurrentUser\Root.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
NAME=https_windows_trust

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no openssl to make certificates"; exit 0; }
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) windows=1 ;;
    *) windows= ;;
esac

unset SSL_CERT_FILE SSL_CERT_DIR AETHER_PURE_TLS || :

TMP="$(mktemp -d)"
# Certificates carry the extensions asked for, under any openssl (#2776).
. "$ROOT/tests/lib/test_openssl_conf.sh"
test_openssl_conf "$TMP"
PIDS=""
cleanup() {
    for p in $PIDS; do stop_server "$p"; done
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $NAME: $1"; exit 1; }

# ---- certificates (as https_client_pure_tls makes them) ---------------------
gen() {
    MSYS2_ARG_CONV_EXCL='/CN=' openssl "$@" >>"$TMP/ssl.log" 2>&1 \
        || { sed -n '1,12p' "$TMP/ssl.log"; fail "openssl $1 failed while making test certificates"; }
}
EC="-newkey ec -pkeyopt ec_paramgen_curve:prime256v1"
gen req -x509 $EC -keyout "$TMP/ca.key" -out "$TMP/ca.pem" -days 2 -nodes \
    -subj "/CN=Aether Windows Trust Test CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"
gen req $EC -keyout "$TMP/leaf.key" -out "$TMP/leaf.csr" -nodes -subj "/CN=localhost"
printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n' > "$TMP/leaf.ext"
gen x509 -req -in "$TMP/leaf.csr" -CA "$TMP/ca.pem" -CAkey "$TMP/ca.key" -CAcreateserial \
    -out "$TMP/leaf.pem" -days 2 -sha256 -extfile "$TMP/leaf.ext"
# A CA that signed nothing here.
gen req -x509 $EC -keyout "$TMP/other.key" -out "$TMP/other.pem" -days 2 -nodes \
    -subj "/CN=Aether Unrelated Test CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"

# ---- builds ----------------------------------------------------------------
mkdir -p "$TMP/app/bin" "$TMP/app/share/ssl"
cp "$TMP/ca.pem" "$TMP/app/share/ssl/ca-bundle.crt"
"$AE" build "$SCRIPT_DIR/server.ae" -o "$TMP/srv" >"$TMP/b1.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b1.log"; fail "server did not build"; }
"$AE" build "$SCRIPT_DIR/client.ae" -o "$TMP/app/bin/cli" >"$TMP/b2.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b2.log"; fail "client did not build"; }
"$AE" build "$SCRIPT_DIR/store_probe.ae" -o "$TMP/probe" >"$TMP/b3.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b3.log"; fail "store_probe did not build"; }

CERT_PATH="$TMP/leaf.pem" KEY_PATH="$TMP/leaf.key" "$TMP/srv" >"$TMP/srv.log" 2>&1 &
PIDS="$PIDS $!"
i=0
while [ "$i" -lt 100 ]; do
    grep -q READY "$TMP/srv.log" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
PORT=$(sed -n 's/^READY \([0-9][0-9]*\).*/\1/p' "$TMP/srv.log" | head -1)
[ -n "$PORT" ] || { sed -n '1,10p' "$TMP/srv.log"; fail "the server never became READY"; }
URL="https://localhost:$PORT/"

# One client run, with a watchdog: a handshake that hangs fails in its own
# right. Output in $OUT.
run() {
    "$@" >"$TMP/out" 2>&1 &
    cpid=$!
    n=0
    while kill -0 "$cpid" 2>/dev/null; do
        n=$((n + 1))
        if [ "$n" -gt 300 ]; then
            kill_server "$cpid"
            OUT="HUNG (no result after 30 s)"
            return 0
        fi
        sleep 0.1
    done
    wait "$cpid" 2>/dev/null || :
    OUT=$(grep -vE 'warning: unresolved|-->' "$TMP/out" || :)
}

# both CASE WANT [VAR=value...]: the client on each stack; WANT is "ok" for a
# 200, "fail" for a refused handshake.
both() {
    case_name="$1"; want="$2"; shift 2
    for stack in openssl pure; do
        if [ "$stack" = pure ]; then sel="AETHER_PURE_TLS=1"; else sel="AETHER_PURE_TLS=0"; fi
        run env "$sel" "$@" URL="$URL" "$TMP/app/bin/cli"
        case "$want:$OUT" in
            "ok:STATUS 200 BODY trust-ok"*) echo "    ok: $case_name ($stack)" ;;
            fail:ERR:*) echo "    ok: $case_name ($stack refused)" ;;
            *) echo "    got: $OUT"
               # What the client was given, so a failure says what the CA held.
               echo "    openssl: $(openssl version 2>&1)"
               openssl x509 -in "$TMP/ca.pem" -noout -text 2>/dev/null \
                   | grep -E "X509v3 |CA:" | sed "s/^ */    ca: /"
               fail "$case_name on the $stack stack: expected $want" ;;
        esac
    done
}

# 1. SSL_CERT_FILE names the store: both trust the CA in it.
both "SSL_CERT_FILE" ok SSL_CERT_FILE="$TMP/ca.pem"

# 2. ...and it alone, even with the right CA in the bundle beside the program.
both "SSL_CERT_FILE wins over the bundle" fail SSL_CERT_FILE="$TMP/other.pem"

# 3. An SSL_CERT_FILE that cannot be read fails closed on both: OpenSSL used to
#    fall through to the bundle files, so on Windows it trusted the bundle
#    beside the program while the pure client refused.
both "unreadable SSL_CERT_FILE" fail SSL_CERT_FILE="$TMP/no-such-bundle.pem"

run "$TMP/probe"
blocks=$(printf '%s\n' "$OUT" | sed -n 's/^STORE \([0-9]*\) \([0-9]*\)$/\1/p')
parsed=$(printf '%s\n' "$OUT" | sed -n 's/^STORE \([0-9]*\) \([0-9]*\)$/\2/p')
[ -n "$blocks" ] || fail "store_probe printed no STORE line: $OUT"

if [ -n "$windows" ]; then
    # 4. Nothing set: the bundle beside the program is the first of the list,
    #    and both clients find it next to the store.
    both "bundle beside the program, with the store" ok

    # 5. The store as both clients get it: every certificate PowerShell lists
    #    for the current user's ROOT store (which includes the machine's).
    [ "$blocks" -gt 0 ] || fail "the ROOT store gave no certificate"
    [ "$parsed" -gt 0 ] || fail "none of the $blocks ROOT certificates parsed as a trust anchor"
    if command -v powershell.exe >/dev/null 2>&1; then
        want=$(powershell.exe -NoProfile -NonInteractive -Command \
            "(Get-ChildItem Cert:\\CurrentUser\\Root).Count" 2>/dev/null | tr -d '\r ')
        case "$want" in
            ''|*[!0-9]*) echo "    skip: PowerShell did not count the store ($want)" ;;
            *) [ "$blocks" = "$want" ] \
                   || fail "the store gave $blocks certificates, PowerShell lists $want"
               echo "    ok: the ROOT store's $blocks certificates ($parsed parse as anchors)" ;;
        esac
    fi
else
    # Off Windows there is no such store, and nothing may stand in for one.
    [ "$blocks" = 0 ] || fail "the Windows store reader returned $blocks certificates off Windows"
    echo "    ok: no Windows store off Windows"
fi

echo "  [PASS] $NAME"
