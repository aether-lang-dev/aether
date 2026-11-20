#!/bin/bash
# get.sh's check of a downloaded release hands cosign and gh the arguments it
# built, unchanged, when they are native Windows programs (#3791's remedy,
# applied to get.sh).
#
# Git Bash and MSYS2 rewrite an argument they hand a native Windows program
# when it looks like a path, and that takes in every backslash of the
# certificate-identity regex: "release\.yml" arrived as "release/.yml", which
# no certificate names. The file arguments, left as /tmp/... paths, are not
# ones a native program can open either.
#
# The test builds a small native program (native_tool.c) as cosign and as gh,
# puts them first on PATH, serves a release from the download-fallback test's
# mock, and runs get.sh's gh_release_download (getsh_download.sh) on it. Each
# tool logs the arguments it got and opens the files it was given. Pinned:
#   1. the download is accepted when the tools accept their files;
#   2. cosign and gh each received exactly the identity regex, backslashes
#      and all, anchored at both ends, and gh the archive, cosign the bundle
#      and SHA256SUMS, as files they can open.
# On Linux and macOS it holds trivially; on Windows (ci.yml's Windows legs)
# it is the one that fails when get.sh stops running the tools unconverted.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"
. "$ROOT/tests/lib/server_jobs.sh"
NAME=getsh_native_args
MOCK="$ROOT/tests/integration/ae_download_api_fallback/mock_github.py"

[ -f "$ROOT/get.sh" ] && [ -f "$MOCK" ] || { echo "  [SKIP] $NAME: no get.sh or mock"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "  [SKIP] $NAME: python3 needed for the mock"; exit 0; }
command -v curl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: curl needed"; exit 0; }
CC_BIN=""
for c in "${CC:-}" cc gcc clang; do
    [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && { CC_BIN="$c"; break; }
done
[ -n "$CC_BIN" ] || { echo "  [SKIP] $NAME: no C compiler for the stand-in tools"; exit 0; }

WIN=0; EXE=""
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) WIN=1; EXE=.exe ;;
esac
native_path() { if [ "$WIN" = 1 ]; then cygpath -m "$1"; else printf '%s' "$1"; fi; }

TMP="$(mktemp -d)"
SRV_PID=""
cleanup() { stop_server "$SRV_PID"; rm -rf "$TMP"; return 0; }
trap cleanup EXIT
fail() {
    echo "  [FAIL] $NAME: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2"
    exit 1
}

mkdir -p "$TMP/rel" "$TMP/tools"
"$CC_BIN" -o "$TMP/tools/cosign$EXE" "$SCRIPT_DIR/native_tool.c" || fail "could not build the stand-in cosign"
"$CC_BIN" -o "$TMP/tools/gh$EXE" "$SCRIPT_DIR/native_tool.c" || fail "could not build the stand-in gh"

TAG=v9.9.9
ASSET="aether-9.9.9-test-x86_64.tar.gz"
head_c 3000 /dev/urandom > "$TMP/rel/$ASSET"
SUM="$(sha256_of "$TMP/rel/$ASSET")"
[ "${#SUM}" = 64 ] || { echo "  [SKIP] $NAME: no sha256 tool"; exit 0; }
printf '%s  %s\n' "$SUM" "$ASSET" > "$TMP/rel/SHA256SUMS"
echo '{"mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json"}' > "$TMP/rel/SHA256SUMS.sigstore.json"

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
BASE="http://127.0.0.1:$PORT"
: > "$TMP/rel/requests.log"
python3 "$MOCK" "$PORT" "$(native_path "$TMP/rel")" web >/dev/null 2>&1 &
SRV_PID=$!
i=0
while [ "$i" -lt 50 ]; do
    curl -s -o /dev/null "$BASE/" 2>/dev/null && break
    i=$((i + 1)); sleep 0.1
done
[ "$i" -lt 50 ] || { echo "  [SKIP] $NAME: mock server did not start"; exit 0; }

: > "$TMP/tools.log"
( env -u GITHUB_TOKEN -u GH_TOKEN PATH="$TMP/tools:$PATH" NATIVE_TOOL_LOG="$(native_path "$TMP/tools.log")" \
    AE_GITHUB_WEB_BASE="$BASE" AE_GITHUB_API_BASE="$BASE" \
    sh "$SCRIPT_DIR/getsh_download.sh" "$TAG" "$ASSET" "$TMP/out.tar.gz" ) > "$TMP/get.log" 2>&1 \
    || fail "get.sh refused a release the stand-in cosign and gh accept" "$TMP/get.log"
[ -s "$TMP/out.tar.gz" ] || fail "get.sh left no archive" "$TMP/get.log"
grep -q "verified with cosign" "$TMP/get.log" || fail "cosign was not run" "$TMP/get.log"
grep -q "provenance verified with gh" "$TMP/get.log" || fail "gh was not run" "$TMP/get.log"

# One argument per line: "<tool> <argument>".
IDENTITY='^https://github\.com/aether-lang-dev/aether/\.github/workflows/release\.yml@refs/(heads/main|tags/v9\.9\.9)$'
for t in cosign gh; do
    grep -Fxq "$t $IDENTITY" "$TMP/tools.log" \
        || fail "$t did not receive the identity regex as built, $IDENTITY" "$TMP/tools.log"
done
grep -Eq '^cosign verify-blob$' "$TMP/tools.log" || fail "cosign was not asked to verify-blob" "$TMP/tools.log"
grep -Eq '^gh attestation$' "$TMP/tools.log" || fail "gh was not asked to attest" "$TMP/tools.log"

echo "  [PASS] $NAME: cosign and gh got the identity regex unchanged and files they could open"
