#!/usr/bin/env sh
# Aether remote installer — install a prebuilt release binary, or build from source.
#
# The one-command, no-clone path (mirrors aeb's install.sh). For the full
# clone-time installer (editor extension, `ae version` management, shell-rc
# setup, `~/.aether` layout) use ./install.sh after `git clone`; for the
# from-HEAD developer flow see docs/bootstrap-from-source.md.
#
# Usage:
#   curl -sSL https://raw.githubusercontent.com/aether-lang-dev/aether/main/get.sh | sh
#   curl -sSL .../get.sh | sh -s -- v0.184.0            # a specific release (piped)
#   sh get.sh v0.184.0                                 # a specific release (downloaded)
#   AETHER_REF=v0.184.0 sh get.sh                       # same, via env var
#   PREFIX=/usr/local sh get.sh v0.184.0               # system-wide (needs sudo)
#   AETHER_FROM_SOURCE=1 sh get.sh                     # force a source build
#
# The version to install may be given as the FIRST POSITIONAL ARGUMENT
# (`sh get.sh v0.184.0`, or piped as `| sh -s -- v0.184.0`) or via the
# AETHER_REF env var. The argument wins if both are set. With neither, the
# latest release is used.
#
# Env knobs:
#   AETHER_REF          release tag (vX.Y.Z) to install (same as the positional
#                       argument; the argument takes precedence). Default: the
#                       latest release. A branch or commit SHA is also accepted,
#                       but forces a source build (no prebuilt exists for those).
#   AETHER_FROM_SOURCE  set to 1 to skip the prebuilt binary and build from the
#                       source tarball even when a prebuilt exists.
#   PREFIX              install prefix. Default: $HOME/.local  (no sudo).
#   CC                  C compiler for a source build. Default: cc (gcc/clang).
#
# How this resolves the latest version WITHOUT tripping GitHub's anti-bot /
# rate limits: it reads the `Location:` header of the plain web redirect at
# /releases/latest (a normal 302, not the 60-req/hr api.github.com JSON API).
# Release ASSET downloads at /releases/download/<tag>/<file> are likewise
# unauthenticated and un-throttled (they 302 to a signed CDN URL curl follows).
# When that URL fails (it did, alone, on 2026-11-09), the asset is fetched
# through the REST API instead; see "release assets" below.
#
# Prebuilt binaries need only a libc at runtime — no C compiler, no make. A
# source build (fallback / forced / non-release ref) needs a C compiler and
# GNU make; Aether compiles to C, so there is no toolchain chicken-and-egg.
# Tests are NOT run either way.
set -eu

REPO="aether-lang-dev/aether"
PREFIX="${PREFIX:-$HOME/.local}"
CC="${CC:-cc}"
FROM_SOURCE="${AETHER_FROM_SOURCE:-0}"

say()  { printf 'aether-install: %s\n' "$*"; }
die()  { printf 'aether-install: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

have curl || die "curl is required."
have tar  || die "tar is required."

# --- resolve the release tag to install ------------------------------------
# Precedence: first positional argument, then AETHER_REF, then latest. The ref
# may be a tag (vX.Y.Z), a branch, or a SHA. Only a vX.Y.Z tag has a prebuilt;
# branches/SHAs fall through to a source build.
REF="${1:-${AETHER_REF:-}}"
if [ -z "$REF" ]; then
    # The redirect at /releases/latest names the newest release tag in its
    # Location header. No JSON API, no auth, no rate limit.
    loc=$(curl -fsSI "https://github.com/$REPO/releases/latest" 2>/dev/null \
        | tr -d '\r' \
        | sed -n 's#^[Ll]ocation:[[:space:]]*.*/releases/tag/\(.*\)$#\1#p' \
        | tail -1)
    if [ -n "$loc" ]; then
        REF="$loc"
        say "latest release is $REF"
    else
        # No release yet (or the redirect was unreachable): fall back to the
        # highest vX.Y.Z tag via the tags API, then to 'main'.
        say "could not read /releases/latest; trying the tags API"
        REF=$(curl -fsSL "https://api.github.com/repos/$REPO/tags?per_page=100" 2>/dev/null \
            | grep -o '"name"[[:space:]]*:[[:space:]]*"v[0-9][0-9.]*"' \
            | sed -n 's/.*"\(v[0-9][0-9.]*\)".*/\1/p' \
            | sort -t. -k1.2,1n -k2,2n -k3,3n | tail -1)
        if [ -z "$REF" ]; then
            REF="main"
            say "no vX.Y.Z tag found; falling back to 'main' (source build, not pinned)."
        fi
    fi
fi

# Is this ref a release tag (vX.Y.Z, prebuilt candidate) or not?
is_release_tag=0
case "$REF" in
    v[0-9]*.[0-9]*.[0-9]*) is_release_tag=1 ;;
esac
# Version string without the leading 'v' (asset filenames use it).
VER="${REF#v}"

# --- detect the platform slug used in asset names --------------------------
# Assets are named: aether-<ver>-<os>-<arch>{.tar.gz|.zip}
# e.g. aether-0.645.0-linux-x86_64.tar.gz, aether-0.645.0-macos-arm64.tar.gz
detect_slug() {
    os_raw=$(uname -s 2>/dev/null || echo unknown)
    arch_raw=$(uname -m 2>/dev/null || echo unknown)
    case "$os_raw" in
        Linux)   os=linux ;;
        Darwin)  os=macos ;;
        FreeBSD) os=freebsd ;;
        *)       os="" ;;   # Windows here means MSYS/Cygwin — the .zip path;
    esac                    # unsupported for the shell installer's binary path.
    case "$arch_raw" in
        x86_64|amd64)  arch=x86_64 ;;
        arm64|aarch64) arch=arm64 ;;
        *)             arch="" ;;
    esac
    [ -n "$os" ] && [ -n "$arch" ] && printf '%s-%s' "$os" "$arch"
}

# --- release assets: web URL first, REST API second -------------------------
# https://github.com/<repo>/releases/download/<tag>/<asset> is served by a
# different backend from api.github.com, and it can fail alone (a repo-specific
# HTTP 504 on it was seen on 2026-11-09). The API serves the same bytes: find
# the asset's id in /releases/tags/<tag>, then GET /releases/assets/<id> with
# Accept: application/octet-stream (a redirect to the CDN). Unauthenticated use
# is allowed; GITHUB_TOKEN or GH_TOKEN, when set, is sent as a Bearer token.
# The API lists a sha256 `digest` per asset; the fallback verifies against it.
# AE_GITHUB_WEB_BASE / AE_GITHUB_API_BASE move the two endpoints (for tests).
GH_WEB="${AE_GITHUB_WEB_BASE:-https://github.com}"
GH_API="${AE_GITHUB_API_BASE:-https://api.github.com}"

# gh_http URL DEST [octet] [auth] : one GET with curl, else wget; non-zero on failure.
# The token goes in a mode-600 file, never on the command line.
gh_http() {
    _ghh_u="$1"; _ghh_d="$2"; _ghh_o="${3:-}"; _ghh_tok=""
    # The token is for the API only; the plain release URL is tried without it.
    [ -z "${4:-}" ] || _ghh_tok="${GITHUB_TOKEN:-${GH_TOKEN:-}}"
    case "$_ghh_tok" in *[!A-Za-z0-9_.-]*) _ghh_tok="" ;; esac
    _ghh_cfg=""
    if [ -n "$_ghh_tok" ]; then
        _ghh_cfg="$(umask 077; mktemp)" || return 1
        if have curl; then printf 'header = "Authorization: Bearer %s"\n' "$_ghh_tok" > "$_ghh_cfg"
        else printf 'header = Authorization: Bearer %s\n' "$_ghh_tok" > "$_ghh_cfg"; fi
    fi
    if have curl; then
        set -- -fsSL --connect-timeout 10
        [ -z "$_ghh_o" ] || set -- "$@" -H "Accept: application/octet-stream"
        [ -z "$_ghh_cfg" ] || set -- "$@" -K "$_ghh_cfg"
        _ghh_rc=0; curl "$@" -o "$_ghh_d" "$_ghh_u" 2>/dev/null || _ghh_rc=$?
    elif have wget; then
        set -- -q --timeout=10 --tries=2
        [ -z "$_ghh_o" ] || set -- "$@" --header="Accept: application/octet-stream"
        [ -z "$_ghh_cfg" ] || set -- "$@" --config="$_ghh_cfg"
        _ghh_rc=0; wget "$@" -O "$_ghh_d" "$_ghh_u" 2>/dev/null || _ghh_rc=$?
    else
        _ghh_rc=1
    fi
    [ -z "$_ghh_cfg" ] || rm -f "$_ghh_cfg"
    return $_ghh_rc
}

gh_sha256() {
    if have sha256sum; then sha256sum "$1" | awk '{print $1}'
    elif have shasum; then shasum -a 256 "$1" | awk '{print $1}'
    fi
}

# gh_release_download REPO TAG ASSET DEST : fetch a release asset, trying the
# normal URL and then the API. 0 on success; on failure says that both failed.
gh_release_download() {
    _ghd_r="$1"; _ghd_t="$2"; _ghd_a="$3"; _ghd_d="$4"
    gh_http "$GH_WEB/$_ghd_r/releases/download/$_ghd_t/$_ghd_a" "$_ghd_d" && return 0
    rm -f "$_ghd_d"
    say "  $GH_WEB/$_ghd_r/releases/download/$_ghd_t/$_ghd_a failed; trying the GitHub API"
    _ghd_j="$_ghd_d.release.json"
    if ! gh_http "$GH_API/repos/$_ghd_r/releases/tags/$_ghd_t" "$_ghd_j" "" auth; then
        rm -f "$_ghd_j"
        say "  both the release URL and the GitHub API failed for $_ghd_a ($_ghd_t): the API could not be read (no network, no such release, or its rate limit)"
        return 1
    fi
    # No jq: split the JSON at commas, one field per line, and take the id of
    # the asset whose "name" is exactly $_ghd_a, plus its sha256 digest if listed.
    _ghd_l="$(tr ',' '\n' < "$_ghd_j" | awk -v f="$_ghd_a" '
        /"id"[[:space:]]*:/ && !hit { s=$0; sub(/.*"id"[[:space:]]*:[[:space:]]*/,"",s); sub(/[^0-9].*/,"",s); lastid=s }
        /"name"[[:space:]]*:/ { if (hit) exit
            n=$0; sub(/.*"name"[[:space:]]*:[[:space:]]*"/,"",n); sub(/".*/,"",n)
            if (n==f) { hit=1; id=lastid } }
        hit && /"digest"[[:space:]]*:/ { d=$0; sub(/.*sha256:/,"",d); sub(/[^0-9a-f].*/,"",d); dig=d }
        END { if (hit) print id, dig }')"
    rm -f "$_ghd_j"
    _ghd_id="${_ghd_l%% *}"; _ghd_dig=""
    case "$_ghd_l" in *" "*) _ghd_dig="${_ghd_l#* }" ;; esac
    if [ -z "$_ghd_id" ]; then
        say "  both the release URL and the GitHub API failed for $_ghd_a ($_ghd_t): the release lists no asset by that name"
        return 1
    fi
    if ! gh_http "$GH_API/repos/$_ghd_r/releases/assets/$_ghd_id" "$_ghd_d" octet auth; then
        rm -f "$_ghd_d"
        say "  both the release URL and the GitHub API failed for $_ghd_a ($_ghd_t): the API asset download failed"
        return 1
    fi
    if [ "${#_ghd_dig}" = 64 ]; then
        _ghd_got="$(gh_sha256 "$_ghd_d")"
        if [ -n "$_ghd_got" ] && [ "$_ghd_got" != "$_ghd_dig" ]; then
            rm -f "$_ghd_d"
            say "  the API download of $_ghd_a does not match the sha256 digest GitHub lists (expected $_ghd_dig, got $_ghd_got)"
            return 1
        fi
    fi
    say "  fetched $_ghd_a through the GitHub API"
    return 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

install_prebuilt=0
if [ "$FROM_SOURCE" != "1" ] && [ "$is_release_tag" = "1" ]; then
    slug=$(detect_slug || true)
    if [ -n "${slug:-}" ]; then
        asset="aether-$VER-$slug.tar.gz"
        say "trying prebuilt binary: $asset"
        if gh_release_download "$REPO" "$REF" "$asset" "$tmp/aether-bin.tar.gz"; then
            install_prebuilt=1
        else
            say "no prebuilt for $slug at $REF — will build from source."
        fi
    else
        say "no prebuilt for this platform ($(uname -s)/$(uname -m)) — building from source."
    fi
fi

# --- prebuilt path: extract the prefix-shaped tree straight into PREFIX -----
if [ "$install_prebuilt" = "1" ]; then
    tar -xzf "$tmp/aether-bin.tar.gz" -C "$tmp" || die "extract failed (prebuilt)."
    # The tarball is flat: bin/ lib/ include/ share/ VERSION LICENSE at top.
    [ -x "$tmp/bin/ae" ] || die "prebuilt archive missing bin/ae — is the asset correct?"
    mkdir -p "$PREFIX" || die "cannot create PREFIX ($PREFIX)."
    say "installing prebuilt @ $REF  ->  PREFIX=$PREFIX"
    # Copy the four prefix trees; -R preserves the layout `make install` uses.
    for d in bin lib include share; do
        [ -d "$tmp/$d" ] && cp -R "$tmp/$d" "$PREFIX/"
    done
    chmod 755 "$PREFIX/bin/ae" "$PREFIX/bin/aetherc" 2>/dev/null || true
else
    # --- source path: fetch the source tarball and `make install` ----------
    have make || die "GNU make is required for a source build."
    have "$CC" || die "a C compiler ('$CC') is required — install gcc or clang, or set CC."
    say "installing aether @ $REF from source  ->  PREFIX=$PREFIX  (CC=$CC)"

    # GitHub serves a source tarball for any ref (tag/branch/sha) at this URL.
    surl="https://github.com/$REPO/archive/$REF.tar.gz"
    say "fetching $surl"
    curl -fSL "$surl" -o "$tmp/aether-src.tar.gz" || die "download failed for ref '$REF'."
    tar -xzf "$tmp/aether-src.tar.gz" -C "$tmp" || die "extract failed (source)."

    # GitHub names the top dir <repo>-<ref> (leading 'v' stripped for tags).
    src=$(find "$tmp" -mindepth 1 -maxdepth 1 -type d -name 'aether-*' | head -1)
    [ -n "$src" ] && [ -d "$src" ] || die "could not locate extracted source dir."

    say "building + installing (no tests run) — this compiles the toolchain, ~1-2 min"
    make -C "$src" install PREFIX="$PREFIX" CC="$CC"
fi

bin="$PREFIX/bin/ae"
[ -x "$bin" ] || die "install finished but $bin is missing."
say "installed: $bin"
"$bin" --version || true

case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) say "note: $PREFIX/bin is not on your PATH — add it to use 'ae' directly." ;;
esac

# Which ae does the shell reach? A second install (install.sh defaults to
# ~/.aether/bin, this one to ~/.local/bin) earlier on PATH keeps winning, and
# nothing used to say so: that is the second half of issue #1602.
other=$(command -v ae 2>/dev/null || true)
if [ -n "$other" ] && [ "$other" != "$bin" ]; then
    say "note: PATH finds a different ae first: $other"
    say "      put $PREFIX/bin ahead of it, or remove the other install."
fi

say "optional: native contrib modules (sqlite, avcodec, …) — from a"
say "          checkout run 'make contrib && make install-contrib PREFIX=$PREFIX'"
say "          (built only where the dev libraries are present)."
say "done. Pin this in CI with: AETHER_REF=$REF"
