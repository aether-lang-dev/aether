#!/bin/bash
# A release archive is checked against the release's SHA256SUMS, and, when
# cosign and an authenticated gh are installed, the signature of SHA256SUMS
# and the archive's build provenance, before it is unpacked (#3752).
#
# A release made since #3752 lists SHA256SUMS (sha256sum's lines for every
# asset), SHA256SUMS.sigstore.json (keyless cosign) and an attestation per
# asset. The mock (mock_github.py) serves the files this test puts beside the
# archive; fake `cosign` and `gh` programs stand in for the real ones and log
# how they were called, so nothing touches the public internet or the
# machine's own tools: AE_GITHUB_WEB_BASE and AE_GITHUB_API_BASE point at the
# mock, and PATH holds the fakes or no tool of either name.
#
# Pinned, for ae (`ae version install`, so `ae install` and `ae upgrade`) and
# (on POSIX) for get.sh:
#   1. a matching SHA256SUMS line installs; without cosign or gh it says
#      that the signature and the provenance were not checked;
#   2. a SHA256SUMS line that does not match refuses, nothing installed;
#   3. a SHA256SUMS that does not list the archive refuses;
#   4. with the API down, SHA256SUMS from the release URL still verifies it;
#   5. cosign that verifies, and gh that accepts the attestation: installs,
#      says so, and cosign was asked about this repository's release
#      workflow under GitHub's token issuer;
#   6. cosign that refuses the signature refuses the install;
#   7. gh that refuses the attestation refuses the install;
#   8. gh that is not logged in is not asked, and that is said.
# A release with no SHA256SUMS keeps the digest check of #3748
# (test_release_digest_verified.sh).
# HOME and USERPROFILE are redirected so the real ~/.aether is never touched.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
NAME=release_sums_verified

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "  [SKIP] $NAME: python3 needed for the mock"; exit 0; }
command -v curl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: curl needed"; exit 0; }

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
    echo "  [FAIL] $NAME: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2"
    exit 1
}

PLAT="$("$AE" version 2>&1 | tr -d '\r' | sed -n 's/^Platform:[[:space:]]*//p' | head -1)"
[ -n "$PLAT" ] || { echo "  [SKIP] $NAME: cannot read the platform from 'ae version'"; exit 0; }
[ "$PLAT" = unsupported ] && { echo "  [SKIP] $NAME: no release build for this host ($(uname -s))"; exit 0; }

# A release archive that passes ae's post-extract checks, as in
# test_release_digest_verified.sh.
mkdir -p "$TMP/pkg/bin" "$TMP/pkg/lib/aether" "$TMP/pkg/share/aether" "$TMP/rel"
printf '#!/bin/sh\necho aetherc 9.9.9\n' > "$TMP/pkg/bin/aetherc"
chmod 755 "$TMP/pkg/bin/aetherc"; cp "$TMP/pkg/bin/aetherc" "$TMP/pkg/bin/ae"
{ printf '!<arch>\n'; head_c 70000 /dev/urandom; } > "$TMP/pkg/lib/aether/libaether.a"
head_c 20000 /dev/urandom > "$TMP/pkg/share/aether/pad.bin"
echo 9.9.9 > "$TMP/pkg/VERSION"
case "$PLAT" in
    windows*)
        ASSET="aether-9.9.9-$PLAT.zip"
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
        ASSET="aether-9.9.9-$PLAT.tar.gz"
        ( cd "$TMP/pkg" && COPYFILE_DISABLE=1 tar -czf "$TMP/rel/$ASSET" . ) || fail "could not make the archive"
        ;;
esac
SUM="$(sha256sum "$TMP/rel/$ASSET" 2>/dev/null | awk '{print $1}')"
[ -n "$SUM" ] || SUM="$(shasum -a 256 "$TMP/rel/$ASSET" | awk '{print $1}')"
[ "${#SUM}" = 64 ] || { echo "  [SKIP] $NAME: no sha256 tool"; exit 0; }
OTHER="aether-9.9.9-other-platform.tar.gz"

# write_sums good|bad|missing: SHA256SUMS as the release workflow writes it,
# with this archive's line right, wrong, or absent; and a bundle beside it.
write_sums() {
    case "$1" in
        good)    printf '%s  %s\n%s  %s\n' "$SUM" "$ASSET" "$(printf '%064d' 7)" "$OTHER" ;;
        bad)     printf '%s  %s\n%s  %s\n' "$(printf '%064d' 1)" "$ASSET" "$(printf '%064d' 7)" "$OTHER" ;;
        missing) printf '%s  %s\n' "$(printf '%064d' 7)" "$OTHER" ;;
    esac > "$TMP/rel/SHA256SUMS"
    echo '{"mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json"}' > "$TMP/rel/SHA256SUMS.sigstore.json"
}

# Fake tools. Each logs its arguments to $TMP/tools.log and exits with the
# status in $TMP/<tool>.rc (gh: <tool>_auth.rc for `auth status`).
TOOLS="$TMP/tools"
mkdir -p "$TOOLS"
if [ "$WIN" = 1 ]; then
    printf '@echo cosign %%*>> "%s"\r\n@set /p rc=<"%s"\r\n@exit /b %%rc%%\r\n' \
        "$(cygpath -w "$TMP/tools.log")" "$(cygpath -w "$TMP/cosign.rc")" > "$TOOLS/cosign.cmd"
    printf '@echo gh %%*>> "%s"\r\n@if "%%~1"=="auth" (set /p rc=<"%s") else (set /p rc=<"%s")\r\n@exit /b %%rc%%\r\n' \
        "$(cygpath -w "$TMP/tools.log")" "$(cygpath -w "$TMP/gh_auth.rc")" "$(cygpath -w "$TMP/gh.rc")" > "$TOOLS/gh.cmd"
else
    printf '#!/bin/sh\necho "cosign $*" >> "%s"\nexit "$(cat "%s")"\n' "$TMP/tools.log" "$TMP/cosign.rc" > "$TOOLS/cosign"
    printf '#!/bin/sh\necho "gh $*" >> "%s"\nif [ "$1" = auth ]; then exit "$(cat "%s")"; fi\nexit "$(cat "%s")"\n' \
        "$TMP/tools.log" "$TMP/gh_auth.rc" "$TMP/gh.rc" > "$TOOLS/gh"
    chmod +x "$TOOLS/cosign" "$TOOLS/gh"
fi

# PATH without any cosign or gh of the machine's own.
BARE_PATH=""
old_ifs="$IFS"; IFS=:
for d in $PATH; do
    [ -n "$d" ] || continue
    skip=0
    for t in cosign gh cosign.exe gh.exe; do [ -e "$d/$t" ] && skip=1; done
    [ "$skip" = 1 ] || BARE_PATH="${BARE_PATH:+$BARE_PATH:}$d"
done
IFS="$old_ifs"

# tools none | tools COSIGN_RC GH_AUTH_RC GH_RC
tools() {
    : > "$TMP/tools.log"
    if [ "$1" = none ]; then RUN_PATH="$BARE_PATH"; return; fi
    RUN_PATH="$TOOLS:$BARE_PATH"
    echo "$1" > "$TMP/cosign.rc"; echo "$2" > "$TMP/gh_auth.rc"; echo "$3" > "$TMP/gh.rc"
}

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
    echo "  [SKIP] $NAME: mock server did not start"; exit 0
}

run_install() {   # $1 = case name
    H="$TMP/home_$1"; mkdir -p "$H"
    ( env -u GITHUB_TOKEN -u GH_TOKEN PATH="$RUN_PATH" HOME="$H" USERPROFILE="$(native_path "$H")" \
        AE_GITHUB_WEB_BASE="$BASE" AE_GITHUB_API_BASE="$BASE" \
        "$AE" version install v9.9.9 ) >"$TMP/$1.log" 2>&1
}
installed() { [ -d "$TMP/home_$1/.aether/versions/v9.9.9/bin" ]; }

expect_ok() {   # $1 = case, $2 = what
    run_install "$1" || fail "ae refused $2" "$TMP/$1.log"
    installed "$1" || fail "ae installed nothing ($2)" "$TMP/$1.log"
}
expect_refused() {   # $1 = case, $2 = what, $3 = the message
    run_install "$1" && fail "ae installed $2" "$TMP/$1.log"
    installed "$1" && fail "ae left an install behind after refusing $2" "$TMP/$1.log"
    grep -q "$3" "$TMP/$1.log" || fail "ae did not say '$3' ($2)" "$TMP/$1.log"
}

# ae ---------------------------------------------------------------------------
write_sums good
start_mock web
tools none
expect_ok sums "an archive whose SHA256SUMS line matches"
grep -q "Verified $ASSET against SHA256SUMS and the sha256 digest GitHub lists" "$TMP/sums.log" \
    || fail "ae did not say it verified against SHA256SUMS" "$TMP/sums.log"
grep -q "signature of SHA256SUMS was not checked: cosign is not installed" "$TMP/sums.log" \
    || fail "ae did not say the signature was not checked" "$TMP/sums.log"
grep -q "provenance was not checked: gh is not installed" "$TMP/sums.log" \
    || fail "ae did not say the provenance was not checked" "$TMP/sums.log"
grep -q "GET /.*/releases/download/v9.9.9/SHA256SUMS" "$TMP/rel/requests.log" \
    || fail "SHA256SUMS did not come from the release URL" "$TMP/rel/requests.log"

write_sums bad
expect_refused badsums "an archive whose SHA256SUMS line does not match" "does not match its sha256 in SHA256SUMS"

write_sums missing
expect_refused nosumsline "an archive SHA256SUMS does not list" "SHA256SUMS does not list it"

write_sums good
start_mock webapidown
expect_ok apidown "an archive SHA256SUMS verifies while the API is down"
grep -q "Verified $ASSET against SHA256SUMS" "$TMP/apidown.log" \
    || fail "ae did not verify against SHA256SUMS with the API down" "$TMP/apidown.log"

start_mock web
tools 0 0 0
expect_ok signed "an archive cosign and gh accept"
grep -q "signature of SHA256SUMS verified with cosign" "$TMP/signed.log" \
    || fail "ae did not say the signature verified" "$TMP/signed.log"
grep -q "provenance verified with gh attestation verify" "$TMP/signed.log" \
    || fail "ae did not say the provenance verified" "$TMP/signed.log"
# cosign was asked about this repository's release workflow, under GitHub's
# token issuer, with the bundle, and gh about the archive.
grep -q "^cosign verify-blob .*--bundle .*SHA256SUMS.sigstore.json.*--certificate-identity https://github.com/aether-lang-dev/aether/.github/workflows/release.yml@refs/.*--certificate-oidc-issuer https://token.actions.githubusercontent.com" \
    "$TMP/tools.log" || fail "cosign was not asked the right question" "$TMP/tools.log"
grep -q "^gh attestation verify .*$ASSET.* --repo aether-lang-dev/aether --signer-workflow aether-lang-dev/aether/.github/workflows/release.yml" \
    "$TMP/tools.log" || fail "gh was not asked the right question" "$TMP/tools.log"

tools 1 0 0
expect_refused badsig "an archive whose SHA256SUMS signature cosign refuses" "does not verify with cosign"

tools 0 0 1
expect_refused badattest "an archive gh attestation verify refuses" "gh attestation verify. accepts"

tools 0 1 0
expect_ok nologin "an archive with gh not logged in"
grep -q "provenance was not checked: gh is not logged in" "$TMP/nologin.log" \
    || fail "ae did not say gh is not logged in" "$TMP/nologin.log"
grep -q "^gh attestation" "$TMP/tools.log" && fail "ae asked a gh that is not logged in" "$TMP/tools.log"

# get.sh -----------------------------------------------------------------------
# get.sh installs prebuilt .tar.gz archives on Linux, macOS and FreeBSD only.
run_get() {   # $1 = case name
    PFX="$TMP/prefix_$1"
    ( env -u GITHUB_TOKEN -u GH_TOKEN PATH="$RUN_PATH" PREFIX="$PFX" AE_GITHUB_WEB_BASE="$BASE" \
        AE_GITHUB_API_BASE="$BASE" sh "$ROOT/get.sh" v9.9.9 ) >"$TMP/get_$1.log" 2>&1
}
get_ok() {   # $1 = case, $2 = what
    run_get "$1" || fail "get.sh refused $2" "$TMP/get_$1.log"
    [ -x "$TMP/prefix_$1/bin/ae" ] || fail "get.sh installed no bin/ae ($2)" "$TMP/get_$1.log"
}
get_refused() {   # $1 = case, $2 = what, $3 = the message
    run_get "$1" && fail "get.sh installed $2" "$TMP/get_$1.log"
    [ -e "$TMP/prefix_$1/bin/ae" ] && fail "get.sh installed after refusing $2" "$TMP/get_$1.log"
    grep -q "from source" "$TMP/get_$1.log" && fail "get.sh fell back to a source build after refusing $2" "$TMP/get_$1.log"
    grep -q "$3" "$TMP/get_$1.log" || fail "get.sh did not say '$3' ($2)" "$TMP/get_$1.log"
}
if [ "$WIN" = 0 ] && [ -f "$ROOT/get.sh" ]; then
    write_sums good
    start_mock web
    tools none
    get_ok sums "an archive whose SHA256SUMS line matches"
    grep -q "verified $ASSET against SHA256SUMS and the sha256 digest GitHub lists" "$TMP/get_sums.log" \
        || fail "get.sh did not say it verified against SHA256SUMS" "$TMP/get_sums.log"
    grep -q "signature of SHA256SUMS was not checked: cosign is not installed" "$TMP/get_sums.log" \
        || fail "get.sh did not say the signature was not checked" "$TMP/get_sums.log"
    grep -q "provenance was not checked: gh is not installed" "$TMP/get_sums.log" \
        || fail "get.sh did not say the provenance was not checked" "$TMP/get_sums.log"

    write_sums bad
    get_refused badsums "an archive whose SHA256SUMS line does not match" "does not match its sha256 in SHA256SUMS"
    write_sums missing
    get_refused nosumsline "an archive SHA256SUMS does not list" "SHA256SUMS does not list it"

    write_sums good
    start_mock webapidown
    get_ok apidown "an archive SHA256SUMS verifies while the API is down"

    start_mock web
    tools 0 0 0
    get_ok signed "an archive cosign and gh accept"
    grep -q "signature of SHA256SUMS verified with cosign" "$TMP/get_signed.log" \
        || fail "get.sh did not say the signature verified" "$TMP/get_signed.log"
    grep -q "provenance verified with gh attestation verify" "$TMP/get_signed.log" \
        || fail "get.sh did not say the provenance verified" "$TMP/get_signed.log"
    grep -q "^cosign verify-blob .*--bundle .*--certificate-identity-regexp .*aether-lang-dev/.*release.*--certificate-oidc-issuer https://token.actions.githubusercontent.com" \
        "$TMP/tools.log" || fail "get.sh did not ask cosign the right question" "$TMP/tools.log"

    tools 1 0 0
    get_refused badsig "an archive whose SHA256SUMS signature cosign refuses" "does not verify with cosign"
    tools 0 0 1
    get_refused badattest "an archive gh attestation verify refuses" "gh attestation verify' accepts"
fi

echo "  [PASS] $NAME"
