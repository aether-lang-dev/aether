#!/bin/sh
# The pure TLS client against a scripted server (peer.ae) that sends what a
# broken or hostile server would: each case is a PEER_CASE, what the client
# must answer (OK, or an error containing the text), and what the peer must
# see from it (FINISHED, or the alert it must send, by number).
#
#   #3315  extensions in ServerHello and EncryptedExtensions (RFC 8446 4.2):
#          unsupported_extension (110) for what the client never offered,
#          illegal_parameter (47) for an offered one out of place or twice,
#          decode_error (50) for a block that does not parse.
#   #3317  the record layer (RFC 8446 5): limits, types, change_cipher_spec,
#          empty records; client and server.
#   #3321  handshake messages split across records, client and server.
#   #3322  the dummy change_cipher_spec of middlebox compatibility mode.
#   #3316  CertificateEntry extensions (RFC 8446 4.4.2): the same alerts,
#          and decode_error for a status_request that is not one OCSP
#          response.
#   #3324  the right alert: illegal_parameter for a CertificateVerify
#          scheme not offered or not fitting the key, decrypt_error for a
#          bad signature, decode_error for trailing data, unexpected_message
#          for a message out of its place; and the pure server's alerts
#          about the client Finished (peerclient.ae), under its
#          application keys.
#   #3335  a server with an Ed25519 certificate.
#   #3320  KeyUpdate, client and server, both ways (kuclient.ae, kuserver.ae).
#   #3319  NewSessionTicket checks.
#   #3323  alerts: user_canceled is read past, no other alert is non-fatal.
#   #3325  HelloRetryRequest, client and server.
#   #3318  the ServerHello's legacy_version and session id echo.
#   #3357  the pure server's ClientHello checks, secp256r1, skipped 0-RTT data.
#   #3364  decode_error for a repeated EncryptedExtensions extension (found
#          before it is judged unsolicited) and a nonzero compression method.
#   #3365  a CertificateRequest's certificate_authorities and signature list.
#   #3366  a server flight in 1-byte records; the bound is on its bytes.
#   #3367  a message the server's flight cannot hold ends it at once.
#   #3369  the client names secp384r1 without a share and follows a
#          HelloRetryRequest for it; the ServerHello must be for that group.
#   #3379  one change_cipher_spec (the byte 1) in a client handshake; no
#          application data before a ClientHello.
#   #3368  the client reads what the server sent before it closes a failed
#          handshake, so a reset does not take its alert.
#
# No network beyond loopback, no openssl: the peer's certificate is built in.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
NAME=crypto_tls13_peer

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
PIDS=""
cleanup() {
    for p in $PIDS; do stop_server "$p"; done
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $NAME: $1"; exit 1; }

for p in peer client server rawclient peerclient kuclient kuserver; do
    "$AE" build "$SCRIPT_DIR/$p.ae" -o "$TMP/$p" >"$TMP/b_$p.log" 2>&1 \
        || { sed -n '1,12p' "$TMP/b_$p.log"; fail "$p did not build"; }
done

fails=0
# run_case <name> <PEER_CASE> <client expects> <peer expects>
_run_case() {
    rm -f "$TMP/port" "$TMP/peer.out"
    PEER_CASE="$2" PEER_CERT_OUT="$TMP/peer.pem" PEER_PORT_OUT="$TMP/port" "$TMP/peer" >"$TMP/peer.out" 2>&1 &
    ppid=$!
    PIDS="$PIDS $ppid"
    i=0
    while [ "$i" -lt 100 ] && [ ! -s "$TMP/port" ]; do sleep 0.1; i=$((i + 1)); done
    [ -s "$TMP/port" ] || { cat "$TMP/peer.out"; fail "$1: the peer never listened"; }
    cr=0
    case "$2" in *post=*) cr=1 ;; esac
    # CL names another client program, CENV adds its environment.
    got=$(env $CENV CLIENT_READ="$cr" PEER_PORT="$(cat "$TMP/port")" CAFILE="$TMP/peer.pem" "$TMP/${CL:-client}" 2>&1 | grep -v 'warning' || :)
    n=0
    while server_alive "$ppid" && [ "$n" -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
    stop_server "$ppid"
    seen=$(cat "$TMP/peer.out")
    ok=1
    case "$got" in *"$3"*) ;; *) ok=0 ;; esac
    case "$seen" in *"$4"*) ;; *) ok=0 ;; esac
    if [ "$ok" = 1 ]; then
        echo "    ok: $1"
    else
        echo "    FAIL: $1: client said [$got], peer saw [$seen]; want [$3] and [$4]"
        fails=$((fails + 1))
    fi
}

# The cases pass per-case settings as `VAR=value run_case ...` (CL, CENV, SENV,
# SV, POST, HRR, GROUP, EARLY). In bash an assignment before a function call
# lasts for that call; under `sh` (bash in POSIX mode, macOS's /bin/sh) it
# does not, and every later case inherited it, so `sh test_tls13_peer.sh`
# failed 26-30 cases while the sweep (which runs `bash`) passed. Clear them
# after every case so the script means the same under either shell.
_case_reset() { CL=""; CENV=""; SENV=""; SV=""; POST=""; HRR=""; GROUP=""; EARLY=""; }
run_case() { _run_case "$@"; _case_reset; }
run_server_case() { _run_server_case "$@"; _case_reset; }
run_cf_case() { _run_cf_case "$@"; _case_reset; }

run_case "a clean handshake" "" "OK" "FINISHED"

# #3315: EncryptedExtensions.
run_case "EE: an unknown extension" "ee_ext=fafa0000" "ERR" "ALERT 110"
run_case "EE: extended_master_secret (TLS 1.2)" "ee_ext=00170000" "ERR" "ALERT 110"
run_case "EE: renegotiation_info (TLS 1.2)" "ee_ext=ff01000100" "ERR" "ALERT 110"
run_case "EE: ec_point_formats (TLS 1.2)" "ee_ext=000b00020100" "ERR" "ALERT 110"
run_case "EE: early_data, not offered" "ee_ext=002a0000" "ERR" "ALERT 110"
run_case "EE: key_share, offered but out of place" "ee_ext=00330000" "ERR" "ALERT 47"
run_case "EE: supported_versions, out of place" "ee_ext=002b00020304" "ERR" "ALERT 47"
run_case "EE: the same extension twice (#3364)" "ee_ext=000a00040002001d000a00040002001d" "ERR" "ALERT 50"
run_case "EE: an unknown extension twice, refused as a repeat (#3364)" "ee_ext=fafa0000fafa0000" "ERR" "ALERT 50"
run_case "EE: ALPN naming h2, never offered" "ee_ext=001000050003026832" "ERR" "ALERT 47"
run_case "EE: ALPN naming http/1.1" "ee_ext=0010000b000908687474702f312e31" "OK" "FINISHED"
run_case "EE: an empty server_name ack" "ee_ext=00000000" "OK" "FINISHED"
run_case "EE: a server_name ack with a body" "ee_ext=0000000100" "ERR" "ALERT 50"
run_case "EE: supported_groups" "ee_ext=000a00040002001d" "OK" "FINISHED"
run_case "EE: no extensions block at all" "ee_body=-" "ERR" "ALERT 50"

# #3315: ServerHello.
run_case "SH: an unknown extension" "sh_ext=fafa0000" "ERR" "ALERT 110"
run_case "SH: server_name, offered but out of place" "sh_ext=00000000" "ERR" "ALERT 47"
run_case "SH: pre_shared_key in a full handshake" "sh_ext=002900020000" "ERR" "ALERT 110"
run_case "SH: supported_versions twice" "sh_ext=002b00020304" "ERR" "ALERT 47"
run_case "SH: an unknown extension, 32 KiB behind it (#3368)" "sh_ext=fafa0000;sh_tail=32768" "ERR" "ALERT 110"
run_case "SH: compression method 1 (#3364)" "sh_comp=1" "ERR" "ALERT 50"


# #3316: CertificateEntry extensions (RFC 8446 4.4.2).
run_case "Certificate: an unknown entry extension" "cert_ext=fafa0000" "ERR" "ALERT 110"
run_case "Certificate: an SCT, never offered" "cert_ext=00120000" "ERR" "ALERT 110"
run_case "Certificate: server_name, offered but out of place" "cert_ext=00000000" "ERR" "ALERT 47"
run_case "Certificate: status_request with no response" "cert_ext=00050000" "ERR" "ALERT 50"
run_case "Certificate: status_request twice" "cert_ext=0005000501000001000005000501000001ff" "ERR" "ALERT 47"
run_case "Certificate: one stapled response" "cert_ext=0005000501000001ff" "OK" "FINISHED"


# #3317: the record layer (RFC 8446 5).
run_case "flight: the one compatibility change_cipher_spec" "ccs=1" "OK" "FINISHED"
run_case "flight: two change_cipher_spec records" "ccs=2" "ERR" "ALERT 10"
run_case "flight: a change_cipher_spec of value 2" "ccs=bad" "ERR" "ALERT 10"
run_case "SH: the one change_cipher_spec before it" "pre_ccs=1" "OK" "FINISHED"
run_case "SH: two change_cipher_spec records before it (#3379)" "pre_ccs=2" "ERR" "ALERT 10"
run_case "SH: a change_cipher_spec of value 2 before it (#3379)" "pre_ccs=bad" "ERR" "ALERT 10"
run_case "SH: one change_cipher_spec before it and one after (#3379)" "pre_ccs=1;ccs=1" "ERR" "ALERT 10"
run_case "flight: an empty protected handshake record" "empty_hs=1" "ERR" "ALERT 10"
run_case "flight: EncryptedExtensions in the clear" "plain_ee=1" "ERR" "ALERT 10"
run_case "flight: bytes after the Finished in its record" "fin_trailing=1" "ERR" "ALERT 10"
run_case "after: a record of 2^14 + 1 bytes" "post=oversize" "RECVERR" "POST ALERT 22"
run_case "after: 8193 bytes padded past 2^14 + 1" "post=padded" "RECVERR" "POST ALERT 22"
run_case "after: a protected record over 2^14 + 256" "post=bigcipher" "RECVERR" "POST ALERT 22"
run_case "after: 40 empty records" "post=empty40" "RECVERR" "POST ALERT 10"
run_case "after: 10 empty records, then data" "post=empty10" "RECV 2" "POST"
run_case "after: a change_cipher_spec" "post=ccs" "RECVERR" "POST ALERT 10"
run_case "after: a record of unknown type" "post=badtype" "RECVERR" "POST ALERT 10"
run_case "after: a protected record of unknown inner type" "post=innertype" "RECVERR" "POST ALERT 10"
run_case "after: a record that does not authenticate" "post=badmac" "RECVERR" "POST ALERT 20"
run_case "after: an empty handshake record" "post=emptyhs" "RECVERR" "POST ALERT 10"

# run_server_case <name> <RAW hex the client sends> <answer expected>
_run_server_case() {
    rm -f "$TMP/port" "$TMP/server.out"
    PEER_PORT_OUT="$TMP/port" "$TMP/server" >"$TMP/server.out" 2>&1 &
    spid=$!
    PIDS="$PIDS $spid"
    i=0
    while [ "$i" -lt 100 ] && [ ! -s "$TMP/port" ]; do sleep 0.1; i=$((i + 1)); done
    [ -s "$TMP/port" ] || { cat "$TMP/server.out"; fail "$1: the server never listened"; }
    got=$(RAW="$2" PEER_PORT="$(cat "$TMP/port")" "$TMP/rawclient" 2>&1 | grep -v 'warning' || :)
    n=0
    while server_alive "$spid" && [ "$n" -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
    stop_server "$spid"
    case "$got" in
        *"$3"*) echo "    ok: $1" ;;
        *) echo "    FAIL: $1: the server answered [$got], server said [$(cat "$TMP/server.out")]; want [$3]"; fails=$((fails + 1)) ;;
    esac
}

# #3317, the pure server's record layer: refused from the header alone.
run_server_case "server: a record of unknown type first" "180303000100" "ALERT 10"
run_server_case "server: a plaintext record over 2^14" "1603034001" "ALERT 22"
run_server_case "server: a change_cipher_spec before the ClientHello" "140303000101" "ALERT 10"
run_server_case "server: empty application_data records before the ClientHello (#3379)" "170303000017030300001703030000" "ALERT 10"

# #3321: a handshake message split across records (RFC 8446 5.1).
run_case "SH: split into 8-byte records" "split_sh=8" "OK" "FINISHED"
run_case "SH: split into 1-byte records" "split_sh=1" "OK" "FINISHED"
run_case "flight: every message in 1-byte records (#3366)" "split_flight=1" "OK" "FINISHED"
run_case "flight: a Certificate header claiming 2^18 + 1 bytes (#3366)" "extra=0b040001" "ERR" "ALERT 10"
run_case "SH: more handshake bytes after it in its record" "pack_sh=1" "ERR" "ALERT 10"
run_server_case "server: a ClientHello split over two records" "split_ch" "SERVERHELLO"

# #3322: middlebox compatibility (RFC 8446 D.4): the client's dummy
# change_cipher_spec before its second flight, an alert included, and the
# server's right after its ServerHello.
run_case "CCS: before the client's Finished" "" "OK" "CCS+FINISHED"
run_case "CCS: before the client's alert" "ee_ext=fafa0000" "ERR" "CCS+ALERT 110"
run_server_case "server: a change_cipher_spec after the ServerHello" "split_ch" "SERVERHELLO+CCS"


# #3324 and #3335: the server's CertificateVerify (RFC 8446 4.4.3): a
# scheme the client offered and one that fits the key, else
# illegal_parameter; a signature that verifies, else decrypt_error;
# Ed25519 among them.
run_case "CV: an Ed25519 certificate" "key=ed25519" "OK" "FINISHED"
run_case "CV: rsa_pkcs1_sha256, never offered" "cv_scheme=0401" "ERR" "ALERT 47"
run_case "CV: the P-384 scheme for a P-256 key" "cv_scheme=0503" "ERR" "ALERT 47"
run_case "CV: Ed25519 for a P-256 key" "cv_scheme=0807" "ERR" "ALERT 47"
run_case "CV: the P-256 scheme for an Ed25519 key" "key=ed25519;cv_scheme=0403" "ERR" "ALERT 47"
run_case "CV: a signature that does not verify" "cv_bad=1" "ERR" "ALERT 51"
run_case "CV: an Ed25519 signature that does not verify" "key=ed25519;cv_bad=1" "ERR" "ALERT 51"

# #3324: trailing data is decode_error (RFC 8446 6.2), a message out of
# its place unexpected_message.
run_case "trailing: a byte after the CertificateVerify's signature" "cv_trailing=1" "ERR" "ALERT 50"
run_case "trailing: a Finished one byte long (4.4.4, #3357)" "fin_long=1" "ERR" "ALERT 51"
run_case "trailing: a byte after EncryptedExtensions' extensions" "ee_body=000000" "ERR" "ALERT 50"
run_case "order: no CertificateVerify" "skip_cv=1" "ERR" "ALERT 10"
run_case "order: a NewSessionTicket inside the flight" "extra=0400000d0000012c000000000000000000" "ERR" "ALERT 10"
run_case "order: an unknown message inside the flight" "extra=fe000000" "ERR" "ALERT 10"
run_case "order: the Finished sent as type 62 (#3367)" "fin_type=3e" "ERR" "ALERT 10"
run_case "order: the Finished sent as a NewSessionTicket (#3367)" "fin_type=04" "ERR" "ALERT 10"
run_case "CertificateRequest: accepted, an empty Certificate answers" "cr=000008000d000400020403" "OK" "HANDSHAKE"
run_case "CertificateRequest: a byte after its extensions" "cr=000008000d00040002040300" "ERR" "ALERT 50"
run_case "CertificateRequest: a certificate_request_context" "cr=01aa0008000d000400020403" "ERR" "ALERT 50"
run_case "CertificateRequest: no signature_algorithms" "cr=000000" "ERR" "ALERT 109"
run_case "CertificateRequest: an empty certificate_authorities (#3365)" "cr=00000e000d000400020403002f00020000" "ERR" "ALERT 50"
run_case "CertificateRequest: a byte after certificate_authorities' names (#3365)" "cr=000012000d000400020403002f000600030001aa00" "ERR" "ALERT 50"
run_case "CertificateRequest: certificate_authorities of one empty name (#3365)" "cr=000010000d000400020403002f000400020000" "ERR" "ALERT 50"
run_case "CertificateRequest: one name in certificate_authorities (#3365)" "cr=000011000d000400020403002f000500030001aa" "OK" "HANDSHAKE"
run_case "CertificateRequest: an odd signature_algorithms list (#3365)" "cr=000009000d00050003040300" "ERR" "ALERT 50"

# run_cf_case <name> <PEER_CF> <answer expected> <server says>; SV names
# another server program, SENV adds its environment, POST is PEER_POST,
# HRR is PEER_HRR, GROUP PEER_GROUP, EARLY PEER_EARLY.
_run_cf_case() {
    rm -f "$TMP/port" "$TMP/server.out"
    env $SENV PEER_PORT_OUT="$TMP/port" "$TMP/${SV:-server}" >"$TMP/server.out" 2>&1 &
    spid=$!
    PIDS="$PIDS $spid"
    i=0
    while [ "$i" -lt 100 ] && [ ! -s "$TMP/port" ]; do sleep 0.1; i=$((i + 1)); done
    [ -s "$TMP/port" ] || { cat "$TMP/server.out"; fail "$1: the server never listened"; }
    got=$(PEER_GROUP="$GROUP" PEER_EARLY="$EARLY" PEER_HRR="$HRR" PEER_POST="$POST" PEER_CF="$2" PEER_PORT="$(cat "$TMP/port")" "$TMP/peerclient" 2>&1 | grep -v 'warning' || :)
    n=0
    while server_alive "$spid" && [ "$n" -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
    stop_server "$spid"
    said=$(cat "$TMP/server.out")
    ok=1
    case "$got" in *"$3"*) ;; *) ok=0 ;; esac
    case "$said" in *"$4"*) ;; *) ok=0 ;; esac
    if [ "$ok" = 1 ]; then
        echo "    ok: $1"
    else
        echo "    FAIL: $1: the server answered [$got] and said [$said]; want [$3] and [$4]"
        fails=$((fails + 1))
    fi
}

# #3324, the pure server: the client Finished, its alerts under the
# server's application keys.
run_cf_case "server: the right Finished" "ok" "CLOSED" "ACCEPTED"
run_cf_case "server: the Finished over two records" "split" "CLOSED" "ACCEPTED"
run_cf_case "server: a Finished MAC that does not match" "badmac" "ALERT 51 (app)" "REFUSED"
run_cf_case "server: verify_data one byte short (4.4.4, #3357)" "short" "ALERT 51 (app)" "REFUSED"
run_cf_case "server: verify_data one byte long (4.4.4, #3357)" "long" "ALERT 51 (app)" "REFUSED"
run_cf_case "server: a byte after the Finished in its record" "trailing" "ALERT 10 (app)" "REFUSED"
run_cf_case "server: a Certificate instead of the Finished" "wrongtype" "ALERT 10 (app)" "REFUSED"
run_cf_case "server: a record that does not authenticate" "badrecord" "ALERT 20 (app)" "REFUSED"
run_cf_case "server: the Finished in the clear" "plain" "ALERT 10 (app)" "REFUSED"
run_cf_case "server: application data before the Finished" "appdata" "ALERT 10 (app)" "REFUSED"


# #3320: KeyUpdate (RFC 8446 4.6.3). The client and the server move their
# receiving keys on at each KeyUpdate, answer update_requested once with
# their own before their next data however many asked, update their own
# keys on request (conn_key_update), and refuse a KeyUpdate that is not
# one byte of 0 or 1 or does not end its record, and more than 32 without
# data between them.
run_case "KeyUpdate: one, then data under the new keys" "post=ku1" "RECV 2" "POST ALERT 0"
run_case "KeyUpdate: 32 in a row" "post=ku32" "RECV 2" "POST ALERT 0"
run_case "KeyUpdate: 33 in a row" "post=kux33" "RECVERR" "POST ALERT 10"
run_case "KeyUpdate: split over two records" "post=kusplit" "RECV 2" "POST ALERT 0"
CENV="CLIENT_ECHO=1" run_case "KeyUpdate: update_requested is answered" "post=kureq1" "RECV 2" "POST KU DATA 2"
CENV="CLIENT_ECHO=1" run_case "KeyUpdate: five requests, one answer" "post=kureq5" "RECV 2" "POST KU DATA 2"
run_case "KeyUpdate: request_update 2" "post=kubad" "RECVERR" "POST ALERT 47"
run_case "KeyUpdate: a two-byte body" "post=kulong" "RECVERR" "POST ALERT 50"
run_case "KeyUpdate: another message after it in its record" "post=kutrail" "RECVERR" "POST ALERT 10"
CL=kuclient CENV="CLIENT_KU=0" run_case "KeyUpdate: the client updates its keys" "post=listen" "OK" "POST KU DATA 2"
CL=kuclient CENV="CLIENT_KU=1" run_case "KeyUpdate: the client asks for the server's" "post=listen" "OK" "POST KU DATA 2"
CL=kuclient CENV="CLIENT_KU=7" run_case "KeyUpdate: conn_key_update refuses request 7" "post=listen" "KUERR" "POST ALERT 0"
SENV="SERVER_READ=1" POST=ku1 run_cf_case "server KeyUpdate: one, then data" "ok" "POST DATA 2" "RECV 2"
SENV="SERVER_READ=1" POST=kux33 run_cf_case "server KeyUpdate: 33 in a row" "ok" "POST ALERT 10" "RECVERR"
SENV="SERVER_READ=1" POST=kureq5 run_cf_case "server KeyUpdate: five requests, one answer" "ok" "POST KU DATA 2" "RECV 2"
SENV="SERVER_READ=1" POST=kubad run_cf_case "server KeyUpdate: request_update 2" "ok" "POST ALERT 47" "RECVERR"
SENV="SERVER_READ=1" POST=kulong run_cf_case "server KeyUpdate: a two-byte body" "ok" "POST ALERT 50" "RECVERR"
SENV="SERVER_READ=1" POST=kutrail run_cf_case "server KeyUpdate: another message after it" "ok" "POST ALERT 10" "RECVERR"
SENV="SERVER_READ=1" POST=nstx run_cf_case "server: a NewSessionTicket from the client" "ok" "POST ALERT 10" "RECVERR"
SV=kuserver SENV="SERVER_KU=0" POST=listen run_cf_case "server KeyUpdate: the server updates its keys" "ok" "POST KU DATA 2" "ACCEPTED"
SV=kuserver SENV="SERVER_KU=1" POST=listen run_cf_case "server KeyUpdate: the server asks for the client's" "ok" "POST KU DATA 2" "ACCEPTED"

# #3319: NewSessionTicket (RFC 8446 4.6.1): an empty ticket, bytes after
# the extensions, an early_data that is not four bytes and TLS flags that
# are empty are decode_error; a duplicate extension, one this client knows
# that a ticket may not carry, and flags ending in a zero byte are
# illegal_parameter; an extension it does not know is ignored.
NST=0000012c0102030401aa0004deadbeef
CENV="CLIENT_TICKET=1" run_case "ticket: one with early_data" "post=nst:${NST}0008002a000400004000" "TICKET early=16384 len=4" "POST ALERT 0"
CENV="CLIENT_TICKET=1" run_case "ticket: over two records" "post=nstsplit:${NST}0008002a000400004000" "TICKET early=16384 len=4" "POST ALERT 0"
CENV="CLIENT_TICKET=1" run_case "ticket: none behind 512 KiB of data, the wait bounded (#3356)" "post=stream:524288" "TICKETERR: no NewSessionTicket within the first 262144 bytes" "POST STREAMED"
run_case "ticket: an unknown extension" "post=nst:${NST}0004fafa0000" "RECV 2" "POST ALERT 0"
run_case "ticket: TLS flags" "post=nst:${NST}0006003e00020101" "RECV 2" "POST ALERT 0"
run_case "ticket: an empty ticket" "post=nstx:0000012c0102030401aa00000000" "RECVERR" "POST ALERT 50"
run_case "ticket: a byte after the extensions" "post=nstx:${NST}000000" "RECVERR" "POST ALERT 50"
run_case "ticket: early_data twice" "post=nstx:${NST}0010002a000400004000002a000400004000" "RECVERR" "POST ALERT 47"
run_case "ticket: early_data of three bytes" "post=nstx:${NST}0007002a0003000040" "RECVERR" "POST ALERT 50"
run_case "ticket: empty TLS flags" "post=nstx:${NST}0005003e000100" "RECVERR" "POST ALERT 50"
run_case "ticket: TLS flags ending in a zero byte" "post=nstx:${NST}0006003e00020100" "RECVERR" "POST ALERT 47"
run_case "ticket: a key_share" "post=nstx:${NST}000400330000" "RECVERR" "POST ALERT 47"
run_case "after: a CertificateRequest (no post-handshake auth offered)" "post=postcr" "RECVERR" "POST ALERT 10"

# #3323: alerts (RFC 8446 6). user_canceled is read past (at most four in
# a row) and the close_notify after it ends the stream; TLS 1.3 has no
# other non-fatal alert, so one is answered with decode_error; a level
# that is neither 1 nor 2 is illegal_parameter; a fatal alert ends the
# connection unanswered. The same before the server's Finished, and on
# the pure server.
run_case "alert: four user_canceled, then data" "post=alerts:015a015a015a015a" "RECV 2" "POST ALERT 0"
run_case "alert: five user_canceled" "post=alertsx:015a015a015a015a015a" "RECVERR" "POST ALERT 10"
run_case "alert: user_canceled, then close_notify" "post=alertsx:015a0100" "RECV-EOF" "POST ALERT 0"
run_case "alert: unexpected_message at level 1" "post=alertsx:010a" "RECVERR" "POST ALERT 50"
run_case "alert: level 0x42" "post=alertsx:420a" "RECVERR" "POST ALERT 47"
run_case "alert: a fatal user_canceled" "post=alertsx:025a" "RECVERR" "POST CLOSED"
run_case "flight: user_canceled before EncryptedExtensions" "flight_alerts=015a" "OK" "FINISHED"
run_case "flight: unexpected_message at level 1" "flight_alerts=010a" "ERR" "ALERT 50"
run_case "flight: close_notify" "flight_alerts=0100" "ERR: the server closed the connection during the handshake" "CLOSED"
SENV="SERVER_READ=1" POST=alerts:015a015a015a015a run_cf_case "server alert: four user_canceled, then data" "ok" "POST DATA 2" "RECV 2"
SENV="SERVER_READ=1" POST=alertsx:015a015a015a015a015a run_cf_case "server alert: five user_canceled" "ok" "POST ALERT 10" "RECVERR"
SENV="SERVER_READ=1" POST=alertsx:010a run_cf_case "server alert: unexpected_message at level 1" "ok" "POST ALERT 50" "RECVERR"
SENV="SERVER_READ=1" POST=alertsx:420a run_cf_case "server alert: level 0x42" "ok" "POST ALERT 47" "RECVERR"
SENV="SERVER_READ=1" POST=alertsx:0100 run_cf_case "server alert: close_notify" "ok" "POST CLOSED" "EOF"


# #3325: HelloRetryRequest (RFC 8446 4.1.4, 4.2.8). The client follows one
# that asks for a cookie: the change_cipher_spec, then the same ClientHello
# with the cookie, in a record of version 0x0303, under the message_hash
# transcript. It refuses one naming a group it has a share for or did not
# offer, one asking for no change (illegal_parameter), an empty cookie
# (decode_error), a second one (unexpected_message), and a ServerHello
# after it with another suite (illegal_parameter). The pure server asks
# for a group it takes when the shares hold none, sends its
# change_cipher_spec after the HelloRetryRequest only, and refuses a second
# ClientHello without that share, with another session id or another
# suite (illegal_parameter) or after two change_cipher_spec records.
run_case "HRR: a cookie, followed" "hrr=cookie" "OK" "FINISHED"
run_case "HRR: x25519, which has a share" "hrr=group:001d" "ERR" "ALERT 47"
run_case "HRR: secp256r1, which has a share" "hrr=group:0017" "ERR" "ALERT 47"
run_case "HRR: secp521r1, never offered" "hrr=group:0019" "ERR" "ALERT 47"
run_case "HRR: secp384r1, named without a share, followed (#3369)" "hrr=group:0018" "OK" "FINISHED"
run_case "HRR: secp384r1, then a ServerHello for x25519 (#3369)" "hrr=group:0018;sh_group=x25519" "ERR" "ALERT 47"
run_case "HRR: asking for no change" "hrr=none" "ERR" "ALERT 47"
run_case "HRR: an empty cookie" "hrr=emptycookie" "ERR" "ALERT 50"
run_case "HRR: a second one" "hrr=cookie,twice" "ERR" "ALERT 10"
run_case "HRR: then a ServerHello with another suite" "hrr=cookie,suite" "ERR" "ALERT 47"
run_case "HRR: compression method 1 (#3364)" "hrr=cookie,comp" "ERR" "ALERT 50"
HRR=ok run_cf_case "server HRR: asks for x25519, then the handshake" "ok" "CLOSED" "ACCEPTED"
HRR=noshare run_cf_case "server HRR: the second ClientHello without the share" "ok" "HRR ALERT 47" "REFUSED"
HRR=sid run_cf_case "server HRR: the second ClientHello with another session id" "ok" "HRR ALERT 47" "REFUSED"
HRR=suite run_cf_case "server HRR: the second ClientHello with another suite" "ok" "HRR ALERT 47" "REFUSED"
HRR=ccs2 run_cf_case "server HRR: two change_cipher_spec records" "ok" "HRR ALERT 10" "REFUSED"

# #3318: the ServerHello's fixed fields (RFC 8446 4.1.3): legacy_version
# 0x0303 and the session id echo, else illegal_parameter. (The PSK
# identity checks need a resumption: crypto_tls13_resume.)
run_case "SH: legacy_version 0x0302" "sh_version=0302" "ERR" "ALERT 47"
run_case "SH: a session id echo that is not the client's" "sh_sid=bad" "ERR" "ALERT 47"


# #3357: the pure server's ClientHello checks (RFC 8446 4.1.2, 4.2, 9.2),
# BoGo's server tests among them: compression other than null alone and
# a pre_shared_key not last are illegal_parameter, so are two key shares
# for one group; a duplicate extension, a server_name, key_share or
# certificate_authorities that does not parse, and bytes after the
# extensions are decode_error; no supported_groups or no key_share is
# missing_extension; another handshake type is unexpected_message. It
# takes a secp256r1 share (9.1 says it MUST), skips 0-RTT data it does not
# take (4.2.10), and sends its change_cipher_spec whatever the session id.
run_server_case "server CH: compression { 1, 0 }" "ch:comp_extra" "ALERT 47"
run_server_case "server CH: compression { 1 }" "ch:comp_nonull" "ALERT 47"
run_server_case "server CH: ALPN twice" "ch:dup_ext" "ALERT 50"
run_server_case "server CH: a byte after the server_name list" "ch:sni_trailing" "ALERT 50"
run_server_case "server CH: certificate_authorities empty" "ch:ca_empty" "ALERT 50"
run_server_case "server CH: two x25519 shares" "ch:ks_dup" "ALERT 47"
run_server_case "server CH: a byte after the key shares" "ch:ks_trailing" "ALERT 50"
run_server_case "server CH: no supported_groups" "ch:no_groups" "ALERT 109"
run_server_case "server CH: no key_share" "ch:no_keyshare" "ALERT 109"
run_server_case "server CH: pre_shared_key not last" "ch:psk_not_last" "ALERT 47"
run_server_case "server CH: handshake type 3" "ch:wrong_type" "ALERT 10"
run_server_case "server CH: a byte after the extensions" "ch:msg_trailing" "ALERT 50"
run_server_case "server CH: no session id, still a change_cipher_spec" "ch:empty_sid" "SERVERHELLO+CCS"
GROUP=p256 run_cf_case "server: a secp256r1 share alone" "ok" "CLOSED" "ACCEPTED"
EARLY=1 run_cf_case "server: 0-RTT data it does not take is skipped" "ok" "CLOSED" "ACCEPTED"

[ "$fails" = 0 ] || fail "$fails case(s)"
echo "  [PASS] $NAME"
