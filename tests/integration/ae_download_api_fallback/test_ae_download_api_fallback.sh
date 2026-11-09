#!/bin/bash
# `ae version install` (and every other `ae` download of a GitHub release
# asset) falls back to the REST API when the release-download URL fails.
#
# On 2026-11-09 every https://github.com/aether-lang-dev/aether/releases/download/...
# URL answered HTTP 504 while the same asset downloaded through
# api.github.com/repos/.../releases/assets/<id>. The mock here (mock_github.py)
# answers 504 on the download URL and serves the API, so nothing touches the
# public internet. AE_GITHUB_WEB_BASE and AE_GITHUB_API_BASE point `ae` at it.
#
# Pinned properties:
#   1. 504 on the download URL, API fine: the version installs, and the API
#      was asked for the octet-stream, with no Authorization header by default,
#   2. GITHUB_TOKEN is sent as a Bearer token on the API requests,
#   3. a digest in the API listing that does not match the bytes is fatal,
#   4. both ways failing says so, naming both,
#   5. an asset missing from the listing says so.
# HOME is redirected so the real ~/.aether is never touched.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"

[ -x "$AE" ] || { echo "  [SKIP] ae_download_api_fallback: ae not built"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "  [SKIP] ae_download_api_fallback: python3 needed"; exit 0; }
command -v tar >/dev/null 2>&1 || { echo "  [SKIP] ae_download_api_fallback: tar needed"; exit 0; }
command -v curl >/dev/null 2>&1 || { echo "  [SKIP] ae_download_api_fallback: curl needed"; exit 0; }

TMP="$(mktemp -d)"
SRV_PID=""
cleanup() { stop_server "$SRV_PID"; rm -rf "$TMP"; return 0; }
trap cleanup EXIT
fail() {
    echo "  [FAIL] ae_download_api_fallback: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2"
    exit 1
}

PLAT="$("$AE" version 2>&1 | sed -n 's/^Platform:[[:space:]]*//p' | head -1)"
[ -n "$PLAT" ] || { echo "  [SKIP] ae_download_api_fallback: cannot read the platform from 'ae version'"; exit 0; }
case "$PLAT" in windows*) echo "  [SKIP] ae_download_api_fallback: zip archives, not covered here"; exit 0 ;; esac

# A release archive that passes ae's post-extract checks: flat layout, a
# bin/aetherc, lib/aether/libaether.a (an `ar` archive) and enough bulk to be
# over the 1 KB floor.
ASSET="aether-9.9.9-$PLAT.tar.gz"
mkdir -p "$TMP/pkg/bin" "$TMP/pkg/lib/aether" "$TMP/pkg/share/aether" "$TMP/rel"
printf '#!/bin/sh\necho aetherc 9.9.9\n' > "$TMP/pkg/bin/aetherc"
chmod 755 "$TMP/pkg/bin/aetherc"; cp "$TMP/pkg/bin/aetherc" "$TMP/pkg/bin/ae"
{ printf '!<arch>\n'; head -c 70000 /dev/urandom; } > "$TMP/pkg/lib/aether/libaether.a"
head -c 20000 /dev/urandom > "$TMP/pkg/share/aether/pad.bin"
echo 9.9.9 > "$TMP/pkg/VERSION"
( cd "$TMP/pkg" && COPYFILE_DISABLE=1 tar -czf "$TMP/rel/$ASSET" . )

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
BASE="http://127.0.0.1:$PORT"

start_mock() {   # $1 = mode
    stop_server "$SRV_PID"; SRV_PID=""
    : > "$TMP/rel/requests.log"
    python3 "$SCRIPT_DIR/mock_github.py" "$PORT" "$TMP/rel" "$1" >/dev/null 2>&1 &
    SRV_PID=$!
    i=0
    while [ "$i" -lt 50 ]; do
        curl -s -o /dev/null "$BASE/" 2>/dev/null && return 0
        i=$((i + 1)); sleep 0.1
    done
    echo "  [SKIP] ae_download_api_fallback: mock server did not start"; exit 0
}

run_install() {   # $1 = case name; extra env via the caller's environment
    H="$TMP/home_$1"; mkdir -p "$H"
    ( env -u GITHUB_TOKEN -u GH_TOKEN $2 HOME="$H" AE_GITHUB_WEB_BASE="$BASE" AE_GITHUB_API_BASE="$BASE" \
        "$AE" version install v9.9.9 ) >"$TMP/$1.log" 2>&1
}

# 1 + 2 ----------------------------------------------------------------------
start_mock ok
run_install ok "" || fail "install did not succeed with the download URL 504ing" "$TMP/ok.log"
[ -x "$TMP/home_ok/.aether/versions/v9.9.9/bin/aetherc" ] || fail "version not installed" "$TMP/ok.log"
grep -q "GET /.*/releases/download/v9.9.9/$ASSET" "$TMP/rel/requests.log" || fail "primary URL was not tried first"
grep -q "GET /repos/.*/releases/assets/1000 auth=- accept=application/octet-stream" "$TMP/rel/requests.log" \
    || fail "API asset was not fetched as octet-stream without credentials" "$TMP/rel/requests.log"

start_mock ok
run_install tok "GITHUB_TOKEN=ghp_testtoken123" || fail "install with a token failed" "$TMP/tok.log"
grep -q "releases/assets/1000 auth=Bearer ghp_testtoken123" "$TMP/rel/requests.log" \
    || fail "GITHUB_TOKEN not sent as a Bearer token" "$TMP/rel/requests.log"
grep -q "releases/download/.*auth=-" "$TMP/rel/requests.log" \
    || fail "token leaked to the web download URL" "$TMP/rel/requests.log"

# 3 ----------------------------------------------------------------------------
start_mock baddigest
run_install bad "" && fail "a wrong digest was accepted"
grep -q "digest" "$TMP/bad.log" || fail "wrong digest not reported" "$TMP/bad.log"
[ -e "$TMP/home_bad/.aether/versions/v9.9.9/bin" ] && fail "a digest mismatch left an install behind"

# 4 ----------------------------------------------------------------------------
start_mock apidown
run_install down "" && fail "install succeeded with both endpoints down"
grep -q "both ways" "$TMP/down.log" || fail "failure does not say both ways were tried" "$TMP/down.log"

# 5 ----------------------------------------------------------------------------
start_mock noasset
run_install none "" && fail "install succeeded though the release lists no asset"
grep -q "lists no asset called $ASSET" "$TMP/none.log" || fail "missing asset not reported" "$TMP/none.log"

# 6: the same for get.sh ------------------------------------------------------
# (the digest-mismatch case is left out: get.sh then builds from source, which
# would reach the real github.com)
if [ -f "$ROOT/get.sh" ]; then
    start_mock ok
    PFX="$TMP/prefix_get"
    ( env -u GITHUB_TOKEN -u GH_TOKEN GH_TOKEN=ghp_gettoken456 PREFIX="$PFX" AE_GITHUB_WEB_BASE="$BASE" \
        AE_GITHUB_API_BASE="$BASE" sh "$ROOT/get.sh" v9.9.9 ) >"$TMP/get.log" 2>&1 \
        || fail "get.sh did not succeed with the download URL 504ing" "$TMP/get.log"
    [ -x "$PFX/bin/ae" ] || fail "get.sh installed no bin/ae" "$TMP/get.log"
    grep -q "through the GitHub API" "$TMP/get.log" || fail "get.sh did not report the API path" "$TMP/get.log"
    grep -q "releases/assets/1000 auth=Bearer ghp_gettoken456" "$TMP/rel/requests.log" \
        || fail "get.sh did not send GH_TOKEN as a Bearer token" "$TMP/rel/requests.log"
fi

echo "  [PASS] ae_download_api_fallback"
