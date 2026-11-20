#!/bin/bash
# A release archive is checked against the sha256 digest GitHub lists for it
# before it is unpacked, on every download path (#3748).
#
# `ae version install` (and so `ae install` and `ae upgrade`) and get.sh used
# to check the digest only when the archive came through the REST API, the
# fallback. Fetched from the release URL, the normal way, it was checked for an
# archive's magic and size and unpacked. The mock here (mock_github.py, the
# web* modes) serves the archive from the release URL and lists a digest in
# the API that is right, wrong, missing, or unreachable. Nothing touches the
# public internet: AE_GITHUB_WEB_BASE and AE_GITHUB_API_BASE point at the mock.
#
# Pinned properties, for ae and (on POSIX) for get.sh:
#   1. a matching digest installs, and says it verified,
#   2. a digest that does not match refuses, and nothing is installed,
#   3. a digest that cannot be had (the API down) refuses, and nothing is
#      installed,
#   4. a listing with no digest refuses (ae; get.sh has the same branch),
#   5. get.sh does not fall back to a source build after a refusal.
# The release is v0.9.9, below the signing floor (SIGNED_SINCE in get.sh):
# it has no SHA256SUMS, as a release from before #3752, and is checked by
# its digest. A release at or above the floor without SHA256SUMS is refused
# (test_release_sums_verified.sh).
# HOME and USERPROFILE are redirected so the real ~/.aether is never touched.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] release_digest_verified: ae not built"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "  [SKIP] release_digest_verified: python3 needed"; exit 0; }
command -v curl >/dev/null 2>&1 || { echo "  [SKIP] release_digest_verified: curl needed"; exit 0; }

WIN=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) WIN=1 ;;
esac
native_path() {
    if [ "$WIN" = 1 ]; then cygpath -m "$1"; else printf '%s' "$1"; fi
}

TMP="$(mktemp -d)"
SRV_PID=""
cleanup() { stop_server "$SRV_PID"; rm -rf "$TMP"; return 0; }
trap cleanup EXIT
fail() {
    echo "  [FAIL] release_digest_verified: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2"
    exit 1
}

PLAT="$("$AE" version 2>&1 | tr -d '\r' | sed -n 's/^Platform:[[:space:]]*//p' | head -1)"
[ -n "$PLAT" ] || { echo "  [SKIP] release_digest_verified: cannot read the platform from 'ae version'"; exit 0; }
[ "$PLAT" = unsupported ] && { echo "  [SKIP] release_digest_verified: no release build for this host ($(uname -s))"; exit 0; }

# A release archive that passes ae's post-extract checks: flat layout, a
# bin/aetherc, lib/aether/libaether.a (an `ar` archive) and enough bulk to be
# over the 1 KB floor. A .zip on Windows, as the releases are there.
mkdir -p "$TMP/pkg/bin" "$TMP/pkg/lib/aether" "$TMP/pkg/share/aether" "$TMP/rel"
printf '#!/bin/sh\necho aetherc 0.9.9\n' > "$TMP/pkg/bin/aetherc"
chmod 755 "$TMP/pkg/bin/aetherc"; cp "$TMP/pkg/bin/aetherc" "$TMP/pkg/bin/ae"
{ printf '!<arch>\n'; head_c 70000 /dev/urandom; } > "$TMP/pkg/lib/aether/libaether.a"
head_c 20000 /dev/urandom > "$TMP/pkg/share/aether/pad.bin"
echo 0.9.9 > "$TMP/pkg/VERSION"
case "$PLAT" in
    windows*)
        ASSET="aether-0.9.9-$PLAT.zip"
        python3 - "$(native_path "$TMP/pkg")" "$(native_path "$TMP/rel/$ASSET")" <<'PY' || fail "could not make the zip"
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _dirs, names in os.walk(src):
        for n in names:
            full = os.path.join(root, n)
            z.write(full, os.path.relpath(full, src))
PY
        ;;
    *)
        ASSET="aether-0.9.9-$PLAT.tar.gz"
        ( cd "$TMP/pkg" && COPYFILE_DISABLE=1 tar -czf "$TMP/rel/$ASSET" . ) || fail "could not make the archive"
        ;;
esac

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
BASE="http://127.0.0.1:$PORT"

start_mock() {   # $1 = mode
    stop_server "$SRV_PID"; SRV_PID=""
    : > "$TMP/rel/requests.log"
    python3 "$SCRIPT_DIR/mock_github.py" "$PORT" "$(native_path "$TMP/rel")" "$1" >/dev/null 2>&1 &
    SRV_PID=$!
    i=0
    while [ "$i" -lt 50 ]; do
        curl -s -o /dev/null "$BASE/" 2>/dev/null && return 0
        i=$((i + 1)); sleep 0.1
    done
    echo "  [SKIP] release_digest_verified: mock server did not start"; exit 0
}

run_install() {   # $1 = case name
    H="$TMP/home_$1"; mkdir -p "$H"
    ( env -u GITHUB_TOKEN -u GH_TOKEN HOME="$H" USERPROFILE="$(native_path "$H")" \
        AE_GITHUB_WEB_BASE="$BASE" AE_GITHUB_API_BASE="$BASE" \
        "$AE" version install v0.9.9 ) >"$TMP/$1.log" 2>&1
}
installed() { [ -d "$TMP/home_$1/.aether/versions/v0.9.9/bin" ]; }

# ae ---------------------------------------------------------------------------
start_mock web
run_install ok || fail "ae did not install an archive whose digest matches" "$TMP/ok.log"
installed ok || fail "ae installed nothing" "$TMP/ok.log"
grep -q "Verified $ASSET" "$TMP/ok.log" || fail "ae did not say it verified the archive" "$TMP/ok.log"
grep -q "GET /.*/releases/download/v0.9.9/$ASSET" "$TMP/rel/requests.log" \
    || fail "the archive did not come from the release URL" "$TMP/rel/requests.log"

start_mock webbaddigest
run_install bad && fail "ae installed an archive whose digest does not match" "$TMP/bad.log"
grep -q "does not match the sha256 digest" "$TMP/bad.log" || fail "ae did not report the mismatch" "$TMP/bad.log"
installed bad && fail "a digest mismatch left an install behind" "$TMP/bad.log"

start_mock webapidown
run_install down && fail "ae installed an archive it could not verify (API down)" "$TMP/down.log"
grep -q "cannot be verified" "$TMP/down.log" || fail "ae did not say the archive cannot be verified" "$TMP/down.log"
installed down && fail "an unverified archive left an install behind" "$TMP/down.log"

start_mock webnodigest
run_install nodig && fail "ae installed an archive with no listed digest" "$TMP/nodig.log"
grep -q "lists no sha256 digest" "$TMP/nodig.log" || fail "ae did not say no digest is listed" "$TMP/nodig.log"
installed nodig && fail "an archive with no digest left an install behind" "$TMP/nodig.log"

# get.sh -----------------------------------------------------------------------
# get.sh installs prebuilt .tar.gz archives on Linux, macOS and FreeBSD only.
run_get() {   # $1 = case name
    PFX="$TMP/prefix_$1"
    ( env -u GITHUB_TOKEN -u GH_TOKEN PREFIX="$PFX" AE_GITHUB_WEB_BASE="$BASE" \
        AE_GITHUB_API_BASE="$BASE" sh "$ROOT/get.sh" v0.9.9 ) >"$TMP/get_$1.log" 2>&1
}
if [ "$WIN" = 0 ] && [ -f "$ROOT/get.sh" ]; then
    start_mock web
    run_get ok || fail "get.sh did not install an archive whose digest matches" "$TMP/get_ok.log"
    [ -x "$TMP/prefix_ok/bin/ae" ] || fail "get.sh installed no bin/ae" "$TMP/get_ok.log"
    grep -q "verified $ASSET" "$TMP/get_ok.log" || fail "get.sh did not say it verified the archive" "$TMP/get_ok.log"

    start_mock webbaddigest
    run_get bad && fail "get.sh installed an archive whose digest does not match" "$TMP/get_bad.log"
    grep -q "does not match the sha256 digest" "$TMP/get_bad.log" || fail "get.sh did not report the mismatch" "$TMP/get_bad.log"
    [ -e "$TMP/prefix_bad/bin/ae" ] && fail "get.sh installed after a mismatch" "$TMP/get_bad.log"
    grep -q "from source" "$TMP/get_bad.log" && fail "get.sh fell back to a source build after a mismatch" "$TMP/get_bad.log"

    start_mock webapidown
    run_get down && fail "get.sh installed an archive it could not verify" "$TMP/get_down.log"
    grep -q "cannot be verified" "$TMP/get_down.log" || fail "get.sh did not say the archive cannot be verified" "$TMP/get_down.log"
    [ -e "$TMP/prefix_down/bin/ae" ] && fail "get.sh installed an unverified archive" "$TMP/get_down.log"
fi

echo "  [PASS] release_digest_verified"
