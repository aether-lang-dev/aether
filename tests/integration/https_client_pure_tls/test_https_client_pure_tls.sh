#!/bin/sh
# std.http.client doing HTTPS over the pure-Aether TLS 1.3 client, hermetically.
#
# Why this exists. A build without OpenSSL -- every `ae build --target=`
# cross-build, and the Android one that sae ships -- has only the pure client
# for https. That path verified certificates, but:
#   - it could only be exercised by actually cross-building, so no CI leg with
#     OpenSSL ever ran it through std.http.client (AETHER_PURE_TLS=1 now
#     selects it there too, which is what this test leans on);
#   - its trust store had to be one PEM bundle file, and Android has none, only
#     directories of one-certificate files (SSL_CERT_DIR is the portable form);
#   - every failure reached the caller as the same guessed sentence, so an
#     untrusted chain, a wrong host and a TLS 1.2-only server looked alike.
#
# Everything is local: a throwaway CA and leaf made here, our own std.http
# server (OpenSSL where the build has it, the pure server where it does not),
# and `openssl s_server` capped at TLS 1.2 for the version case. No network.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
AE="$ROOT/build/ae"
NAME=https_client_pure_tls

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: no openssl to make certificates"; exit 0; }

# Nothing from the caller's environment may decide the trust store or the
# backend; each case below sets exactly what it tests.
unset SSL_CERT_FILE SSL_CERT_DIR AETHER_PURE_TLS || :

TMP="$(mktemp -d)"
PIDS=""
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null || :; wait "$p" 2>/dev/null || :; done
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $NAME: $1"; exit 1; }

# ---- certificates -----------------------------------------------------------
# MSYS2_ARG_CONV_EXCL='/CN=': on MSYS2 the shell rewrites an argument that looks
# like a POSIX path, so `-subj "/CN=..."` would reach openssl as a Windows path.
# Scoped to the subject prefix so the -out/-keyout paths are still converted
# (see https_pure_tls_vertical for the measurement).
gen() {
    MSYS2_ARG_CONV_EXCL='/CN=' openssl "$@" >>"$TMP/ssl.log" 2>&1 \
        || { sed -n '1,12p' "$TMP/ssl.log"; fail "openssl $1 failed while making test certificates"; }
}
EC="-newkey ec -pkeyopt ec_paramgen_curve:prime256v1"

# A CA, and a leaf it signs for DNS:localhost only (no IP SAN, so dialling
# 127.0.0.1 is the wrong-host case).
gen req -x509 $EC -keyout "$TMP/ca.key" -out "$TMP/ca.pem" -days 2 -nodes \
    -subj "/CN=Aether Pure TLS Test CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"
gen req $EC -keyout "$TMP/leaf.key" -out "$TMP/leaf.csr" -nodes -subj "/CN=localhost"
printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n' > "$TMP/leaf.ext"
gen x509 -req -in "$TMP/leaf.csr" -CA "$TMP/ca.pem" -CAkey "$TMP/ca.key" -CAcreateserial \
    -out "$TMP/leaf.pem" -days 2 -extfile "$TMP/leaf.ext"
# A self-signed certificate for the same name, which nothing trusts.
gen req -x509 $EC -keyout "$TMP/self.key" -out "$TMP/self.pem" -days 2 -nodes \
    -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost"

# A CA directory laid out the way Android's /system/etc/security/cacerts is:
# one file per CA named <subject-hash>.0, the PEM followed by the text dump
# Android appends. Plus things a real directory holds that are not
# certificates: a README, a subdirectory, a dotfile.
mkdir -p "$TMP/cadir/sub" "$TMP/emptydir"
HASH=$(openssl x509 -hash -noout -in "$TMP/ca.pem" 2>/dev/null) || HASH=ca
{ cat "$TMP/ca.pem"; openssl x509 -text -noout -in "$TMP/ca.pem"; } > "$TMP/cadir/$HASH.0"
echo "not a certificate" > "$TMP/cadir/README"
echo "-----BEGIN CERTIFICATE-----garbage-----END CERTIFICATE-----" > "$TMP/cadir/.hidden"
cp "$TMP/self.pem" "$TMP/cadir/sub/self.pem"   # must NOT be trusted: no descent

# ---- builds -------------------------------------------------------------------
"$AE" build "$SCRIPT_DIR/server.ae" -o "$TMP/srv" >"$TMP/b1.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b1.log"; fail "server did not build"; }
"$AE" build "$SCRIPT_DIR/client.ae" -o "$TMP/cli" >"$TMP/b2.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b2.log"; fail "client did not build"; }
"$AE" build "$SCRIPT_DIR/client_nomod.ae" -o "$TMP/cli_nomod" >"$TMP/b3.log" 2>&1 \
    || { sed -n '1,12p' "$TMP/b3.log"; fail "client_nomod did not build"; }

start_server() { # name cert key -> sets PORT
    CERT_PATH="$2" KEY_PATH="$3" "$TMP/srv" >"$TMP/$1.log" 2>&1 &
    PIDS="$PIDS $!"
    i=0
    while [ "$i" -lt 100 ]; do
        grep -q READY "$TMP/$1.log" 2>/dev/null && break
        sleep 0.1
        i=$((i + 1))
    done
    grep -q READY "$TMP/$1.log" 2>/dev/null || { sed -n '1,10p' "$TMP/$1.log"; fail "server $1 never became READY"; }
    PORT=$(read_ready_port "$TMP/$1.log") || exit 1
}

# Run one client with a watchdog: a handshake that never finishes is a failure
# in its own right (the TLS 1.2 case must ERROR, not hang). Output in $OUT.
run() {
    "$@" >"$TMP/out" 2>&1 &
    cpid=$!
    n=0
    while kill -0 "$cpid" 2>/dev/null; do
        n=$((n + 1))
        if [ "$n" -gt 300 ]; then
            kill "$cpid" 2>/dev/null || :
            OUT="HUNG (no result after 30 s)"
            return 0
        fi
        sleep 0.1
    done
    wait "$cpid" 2>/dev/null || :
    OUT=$(grep -vE 'warning: unresolved|-->' "$TMP/out" || :)
}

expect() { # case-name pattern
    case "$OUT" in
        *"$2"*) echo "    ok: $1" ;;
        *) echo "    got: $OUT"; fail "$1: expected output containing '$2'" ;;
    esac
}

start_server good "$TMP/leaf.pem" "$TMP/leaf.key"
GOOD=$PORT
start_server self "$TMP/self.pem" "$TMP/self.key"
SELF=$PORT
PURE="AETHER_PURE_TLS=1"

# 1. A pinned CA (set_cafile) on the pure path: the whole GET arrives.
run env $PURE URL="https://localhost:$GOOD/" CAFILE="$TMP/ca.pem" "$TMP/cli"
expect "set_cafile pin" "STATUS 200 BODY pure-client-ok"

# 2. SSL_CERT_FILE names the store.
run env $PURE SSL_CERT_FILE="$TMP/ca.pem" URL="https://localhost:$GOOD/" "$TMP/cli"
expect "SSL_CERT_FILE" "STATUS 200 BODY pure-client-ok"

# 3. SSL_CERT_DIR names an Android-style directory.
run env $PURE SSL_CERT_DIR="$TMP/cadir" URL="https://localhost:$GOOD/" "$TMP/cli"
expect "SSL_CERT_DIR (Android-style directory)" "STATUS 200 BODY pure-client-ok"

# 4. SSL_CERT_DIR is a list; an entry that does not exist does not stop the rest.
# On Windows the separator is ';' and the entries must be native paths: MSYS2
# rewrites a POSIX path (or a ':'-list of them) in the environment it hands a
# native program, but leaves a ';'-list alone, so translate each entry here.
DIRLIST="$TMP/no-such-dir:$TMP/cadir"
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        command -v cygpath >/dev/null 2>&1 \
            && DIRLIST="$(cygpath -m "$TMP/no-such-dir");$(cygpath -m "$TMP/cadir")" ;;
esac
run env $PURE SSL_CERT_DIR="$DIRLIST" URL="https://localhost:$GOOD/" "$TMP/cli"
expect "SSL_CERT_DIR list" "STATUS 200 BODY pure-client-ok"

# 5. Fail closed when the named store holds nothing, and say so.
run env $PURE SSL_CERT_DIR="$TMP/emptydir" URL="https://localhost:$GOOD/" "$TMP/cli"
expect "empty store fails closed" "cannot load trust store"

# 6. Wrong host: the leaf is for localhost, and 127.0.0.1 is not on it.
run env $PURE URL="https://127.0.0.1:$GOOD/" CAFILE="$TMP/ca.pem" "$TMP/cli"
expect "wrong host rejected" "ERR: TLS handshake failed (pure-Aether TLS 1.3)"
case "$OUT" in
    *"not valid for the requested"*|*"iPAddress SAN"*|*subjectAltName*) ;;
    *) echo "    got: $OUT"; fail "wrong host: the error does not name the hostname mismatch" ;;
esac

# 7. A self-signed server is not trusted by our CA -- neither through the
#    directory (whose subdirectory holds that very certificate) nor directly.
run env $PURE SSL_CERT_DIR="$TMP/cadir" URL="https://localhost:$SELF/" "$TMP/cli"
expect "self-signed rejected" "does not chain to a trusted anchor"

# 8. ...unless verification is switched off, which must keep working here.
run env $PURE INSECURE=1 URL="https://localhost:$SELF/" "$TMP/cli"
expect "set_insecure" "STATUS 200 BODY pure-client-ok"

# 9. Selecting the pure client without linking it names the fix. (In a build
#    without OpenSSL the variable is redundant and the message differs, but
#    both name the import.)
run env $PURE URL="https://localhost:$GOOD/" "$TMP/cli_nomod"
expect "pure client not linked" "import std.cryptography.tls13_client"
# ...and says so before it dials: with nothing listening (port 1), the
# answer is the same. It was found only after the TCP connect, so a connect
# that failed first (a loaded machine) read "connection failed".
run env $PURE URL="https://localhost:1/" "$TMP/cli_nomod"
expect "pure client not linked, nothing listening" "import std.cryptography.tls13_client"

# 10. A TLS 1.2-only server: a clear error, promptly -- not a hang, not a
#     crash, and not a "truncated ServerHello". This is also the case that
#     proves AETHER_PURE_TLS really moved the request off OpenSSL, which would
#     have negotiated TLS 1.2 and succeeded.
if openssl s_server -help 2>&1 | grep -q -- '-tls1_2'; then
    S12=""
    for try in 1 2 3 4 5; do
        P=$(( 20000 + ($$ * 7 + try * 1009) % 40000 ))
        # No MSYS2_ARG_CONV_EXCL here: the -cert/-key paths must be translated
        # for a native openssl.exe, and there is no /CN= argument to protect.
        openssl s_server -accept "$P" -cert "$TMP/leaf.pem" -key "$TMP/leaf.key" \
            -tls1_2 -www >"$TMP/s12.log" 2>&1 &
        spid=$!
        PIDS="$PIDS $spid"
        if wait_port "$P" 127.0.0.1 100 >/dev/null 2>&1 && kill -0 "$spid" 2>/dev/null; then
            S12=$P
            break
        fi
        kill "$spid" 2>/dev/null || :
    done
    [ -n "$S12" ] || fail "could not start openssl s_server -tls1_2"
    run env $PURE URL="https://localhost:$S12/" CAFILE="$TMP/ca.pem" "$TMP/cli"
    expect "TLS 1.2-only server" "does not support TLS 1.3"
else
    echo "    skip: TLS 1.2-only server (this openssl s_server has no -tls1_2)"
fi

echo "  [PASS] $NAME"
