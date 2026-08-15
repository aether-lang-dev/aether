#!/bin/sh
# #1360: `ae add` installs from a published release artifact when one
# matches the host, instead of always git-cloning.
#
# Hermetic by construction: a python http.server on a loopback port
# serves a fake forge whose layout mirrors a real one
# (<pkg>/releases/download/<tag>/<repo>-<tag>-<os>-<arch>.tar.gz), and
# AE_RELEASE_BASE_URL points `ae add` at it. Nothing here touches the
# public internet or a real package host.
#
# Pinned properties:
#   1. a matching artifact is downloaded, checksum-verified, unpacked
#      into ~/.aether/packages/<pkg>/, and recorded in aether.toml,
#   2. a MISMATCHED checksum is fatal — nothing is installed, exit 1
#      (the security-critical case; a released binary must be verified
#      in a way a git tag need not be),
#   3. an artifact with no published .sha256 installs but WARNS,
#   4. --source skips the artifact path entirely (falls to git),
#   5. no matching artifact falls back to git rather than failing.
#
# HOME is redirected to a scratch dir so the real package cache is never
# touched.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"

[ -x "$AE" ] || { echo "  [SKIP] ae_add_release_artifact: ae not built"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "  [SKIP] ae_add_release_artifact: python3 needed to serve fixtures"; exit 0; }
command -v tar >/dev/null 2>&1 || { echo "  [SKIP] ae_add_release_artifact: tar needed"; exit 0; }
if command -v sha256sum >/dev/null 2>&1; then SHA="sha256sum"
elif command -v shasum >/dev/null 2>&1; then SHA="shasum -a 256"
else echo "  [SKIP] ae_add_release_artifact: no sha256 tool"; exit 0; fi

# The artifact path is per-host-triple; on a host we publish no assets
# for, ae_host_triple() returns NULL and everything correctly falls back
# to git — nothing to assert here.
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64|Linux-aarch64|Linux-arm64|Darwin-arm64|Darwin-x86_64) ;;
    *) echo "  [SKIP] ae_add_release_artifact: no release triple for this host"; exit 0 ;;
esac

TMP="$(mktemp -d)"
PORT=""
SRV_PID=""
cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    rm -rf "$TMP"
    return 0
}
trap cleanup EXIT

fail() {
    echo "  [FAIL] ae_add_release_artifact: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2"
    exit 1
}

# ---- host triple, spelled as the release convention does ------------------
case "$(uname -s)" in
    Linux)  OS_PART="linux" ;;
    Darwin) OS_PART="macos" ;;
esac
case "$(uname -m)" in
    x86_64)          ARCH_PART="x86_64" ;;
    aarch64|arm64)   ARCH_PART="arm64" ;;
esac
TRIPLE="$OS_PART-$ARCH_PART"

# ---- fake forge ----------------------------------------------------------
FORGE="$TMP/forge"
PKG="fake.host/user/repo"
mk_release() {   # $1 = tag, $2 = "sum" | "nosum"
    d="$FORGE/$PKG/releases/download/$1"
    mkdir -p "$d/payload"
    printf 'greet() -> string { return "hi" }\n' > "$d/payload/module.ae"
    ( cd "$d" && tar -czf "repo-$1-$TRIPLE.tar.gz" -C payload . )
    rm -rf "$d/payload"
    if [ "$2" = "sum" ]; then
        ( cd "$d" && $SHA "repo-$1-$TRIPLE.tar.gz" | awk -v n="repo-$1-$TRIPLE.tar.gz" '{print $1"  "n}' \
            > "repo-$1-$TRIPLE.tar.gz.sha256" )
    elif [ "$2" = "badsum" ]; then
        ( cd "$d" && echo "0000000000000000000000000000000000000000000000000000000000000000  repo-$1-$TRIPLE.tar.gz" \
            > "repo-$1-$TRIPLE.tar.gz.sha256" )
    fi
}
mk_release v1.0.0 sum
mk_release v2.0.0 badsum
mk_release v3.0.0 nosum

# ---- serve it on a free loopback port ------------------------------------
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
( cd "$FORGE" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1 ) &
SRV_PID=$!

# Wait for readiness rather than sleeping a guessed interval.
ready=0
i=0
while [ "$i" -lt 50 ]; do
    if curl -fsS -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null; then ready=1; break; fi
    i=$((i + 1))
    sleep 0.1
done
[ "$ready" = "1" ] || { echo "  [SKIP] ae_add_release_artifact: fixture server did not start"; exit 0; }

BASE="http://127.0.0.1:$PORT"

# ---- each case runs in its own project + its own fake HOME ---------------
new_proj() {
    p="$TMP/proj_$1"
    mkdir -p "$p/home"
    printf '[package]\nname = "t"\nversion = "0.1.0"\n\n[dependencies]\n' > "$p/aether.toml"
    echo "$p"
}

# ---- Property 1: artifact installed + verified + recorded ----------------
P="$(new_proj ok)"
( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" add "$PKG@v1.0.0" ) \
    >"$TMP/ok.log" 2>&1 || fail "install from artifact exited non-zero" "$TMP/ok.log"

grep -q "Found release artifact repo-v1.0.0-$TRIPLE.tar.gz" "$TMP/ok.log" \
    || fail "did not report finding the release artifact" "$TMP/ok.log"
grep -q "Checksum verified" "$TMP/ok.log" \
    || fail "did not verify the published checksum" "$TMP/ok.log"
[ -f "$P/home/.aether/packages/$PKG/module.ae" ] \
    || fail "artifact contents not unpacked into the package dir" "$TMP/ok.log"
grep -q "$PKG" "$P/aether.toml" \
    || fail "dependency not recorded in aether.toml"
# It must NOT have fallen through to git.
grep -qi "Downloading\.\.\." "$TMP/ok.log" \
    && fail "took the git path despite a matching artifact" "$TMP/ok.log"

# ---- Property 2: checksum mismatch is fatal ------------------------------
P="$(new_proj bad)"
( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" add "$PKG@v2.0.0" ) \
    >"$TMP/bad.log" 2>&1 && bad_rc=0 || bad_rc=$?

[ "$bad_rc" -ne 0 ] || fail "a MISMATCHED checksum exited 0 — must be fatal" "$TMP/bad.log"
grep -qi "checksum MISMATCH" "$TMP/bad.log" \
    || fail "no checksum-mismatch diagnostic" "$TMP/bad.log"
[ ! -e "$P/home/.aether/packages/$PKG/module.ae" ] \
    || fail "installed a tampered artifact" "$TMP/bad.log"
# A mismatch must NOT silently fall back to cloning — that would defeat
# the verification entirely.
grep -qi "Downloading\.\.\." "$TMP/bad.log" \
    && fail "fell back to git after a checksum mismatch" "$TMP/bad.log"

# ---- Property 3: no published checksum installs, but warns ---------------
P="$(new_proj nosum)"
( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" add "$PKG@v3.0.0" ) \
    >"$TMP/nosum.log" 2>&1 || fail "unsigned artifact install exited non-zero" "$TMP/nosum.log"
grep -qi "publishes no .sha256" "$TMP/nosum.log" \
    || fail "no warning for an artifact without a published checksum" "$TMP/nosum.log"
[ -f "$P/home/.aether/packages/$PKG/module.ae" ] \
    || fail "unsigned artifact was not installed" "$TMP/nosum.log"

# ---- Property 4: --source skips the artifact path ------------------------
# fake.host does not resolve, so git fails — that failure IS the proof the
# artifact path was skipped.
P="$(new_proj src)"
( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" add "$PKG@v1.0.0" --source ) \
    >"$TMP/src.log" 2>&1
grep -q "Found release artifact" "$TMP/src.log" \
    && fail "--source still used the release artifact" "$TMP/src.log"
grep -qi "Downloading\.\.\." "$TMP/src.log" \
    || fail "--source did not take the git path" "$TMP/src.log"

# ---- Property 5: no matching artifact falls back to git ------------------
P="$(new_proj none)"
( cd "$P" && HOME="$P/home" AE_RELEASE_BASE_URL="$BASE" "$AE" add "$PKG@v9.9.9" ) \
    >"$TMP/none.log" 2>&1
grep -q "Found release artifact" "$TMP/none.log" \
    && fail "claimed to find an artifact for an unpublished tag" "$TMP/none.log"
grep -qi "Downloading\.\.\." "$TMP/none.log" \
    || fail "did not fall back to git when no artifact matched" "$TMP/none.log"

echo "  [PASS] ae_add_release_artifact: artifact install + checksum gate + --source/no-asset fallbacks"
exit 0
