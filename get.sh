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
# through the REST API instead; see "release assets" below. Either way the
# archive is verified before it is unpacked: against the release's SHA256SUMS
# and its keyless cosign signature and build provenance (#3752), when cosign
# and an authenticated gh are installed, and against the sha256 digest the API
# lists for it, which costs one API request per install (#3748). A release
# from before #3752 has no SHA256SUMS and is checked by the digest alone.
#
# Prebuilt binaries need only a libc at runtime — no C compiler, no make. A
# source build (fallback / forced / non-release ref) needs a C compiler and
# GNU make; Aether compiles to C, so there is no toolchain chicken-and-egg.
# Tests are NOT run either way.
set -eu

REPO="aether-lang-dev/aether"
# The signing floor (#3752): the first release signed, with SHA256SUMS, its
# cosign bundle and build provenance. A release at or above it must have
# SHA256SUMS, and its bundle when cosign is here; one without them is
# refused, so someone who can edit a release cannot delete them and have its
# archive taken on GitHub's digest alone. It must be the first release
# published by a release.yml that has the sign job: release.yml's publish
# job fails a release that disagrees with it. Equal to AE_SIGNED_SINCE in
# tools/ae_version.c and signed_since in scripts/verify-release-asset.sh
# (tests/scripts/check_signing_floor.sh, in make check-tests).
SIGNED_SINCE="0.926.0"
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
# The API lists a sha256 `digest` per asset, and the release a SHA256SUMS
# (#3752); the archive is verified against them on every path, before
# anything in it is unpacked (gh_release_download, below). A check that cannot
# be made, or that fails, refuses the prebuilt.
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

# regex_literal TEXT : TEXT with each character an extended regular
# expression reads as syntax escaped, for an identity pattern.
regex_literal() {
    printf '%s' "$1" | sed 's/[]\\.*^$+?(){}|[]/\\&/g'
}

# release_signed TAG : 0 when TAG (vX.Y.Z, anything after Z ignored) is at or
# above SIGNED_SINCE, or is not a version at all; 1 when it is older.
release_signed() {
    awk -v t="$1" -v f="$SIGNED_SINCE" '
        function num(s, i,   p) { split(s, p, "."); return p[i] + 0 }
        BEGIN {
            sub(/^v/, "", t)
            if (t !~ /^[0-9]+\.[0-9]+\.[0-9]+/) exit 0
            for (i = 1; i <= 3; i++) {
                a = num(t, i); b = num(f, i)
                if (a != b) exit (a > b) ? 0 : 1
            }
            exit 0
        }'
}

gh_sha256() {
    if have sha256sum; then sha256sum "$1" | awk '{print $1}'
    elif have shasum; then shasum -a 256 "$1" | awk '{print $1}'
    elif have sha256; then sha256 -q "$1"
    elif have openssl; then openssl dgst -sha256 "$1" | awk '{print $NF}'
    fi
}

# gh_asset_line JSON NAME : "<id> <sha256 hex>" of the asset called exactly
# NAME in a release listing (the hex empty when none is listed), or nothing.
# No jq: split the JSON at commas, one field per line.
gh_asset_line() {
    tr ',' '\n' < "$1" | awk -v f="$2" '
        /"id"[[:space:]]*:/ && !hit { s=$0; sub(/.*"id"[[:space:]]*:[[:space:]]*/,"",s); sub(/[^0-9].*/,"",s); lastid=s }
        /"name"[[:space:]]*:/ { if (hit) exit
            n=$0; sub(/.*"name"[[:space:]]*:[[:space:]]*"/,"",n); sub(/".*/,"",n)
            if (n==f) { hit=1; id=lastid } }
        hit && /"digest"[[:space:]]*:/ { d=$0; sub(/.*sha256:/,"",d); sub(/[^0-9a-f].*/,"",d); dig=d }
        END { if (hit) print id, dig }'
}

# gh_fetch REPO TAG NAME ID DEST : a release file, from the release URL, then
# through the API when ID is known. 0 when fetched.
gh_fetch() {
    gh_http "$GH_WEB/$1/releases/download/$2/$3" "$5" && return 0
    rm -f "$5"
    [ -n "$4" ] || return 1
    gh_http "$GH_API/repos/$1/releases/assets/$4" "$5" octet auth && return 0
    rm -f "$5"
    return 1
}

# gh_release_download REPO TAG ASSET DEST : fetch a release asset, trying the
# normal URL and then the API, and verify it before anything in it is
# unpacked (#3748, #3752):
#   - a release made since #3752 lists SHA256SUMS (signed keyless with cosign
#     by the release workflow, SHA256SUMS.sigstore.json) and a build
#     provenance attestation per asset: the archive must match its line in
#     SHA256SUMS, and the digest the API lists when the API could be read;
#     with cosign installed the signature must verify as the release
#     workflow's, and with gh installed and logged in `gh attestation verify`
#     must accept the archive. A missing tool is said, not a refusal;
#   - a release older than SIGNED_SINCE has no SHA256SUMS: the archive must
#     match the sha256 digest the API lists for it, as before. One at or
#     above it without SHA256SUMS is refused.
# 0 when fetched and verified; 1 when it could not be fetched (said: both ways
# failed); 2 when it was fetched but cannot be verified or does not match
# (said; DEST is removed).
gh_release_download() {
    _ghd_r="$1"; _ghd_t="$2"; _ghd_a="$3"; _ghd_d="$4"
    # The listing first: the digest and the SHA256SUMS it lists are what the
    # archive is checked against, whichever way it comes, and its ids are the
    # API's way to each file.
    _ghd_j="$_ghd_d.release.json"
    _ghd_l=""; _ghd_s=""; _ghd_g=""; _ghd_why=""; _ghd_listing=0
    if gh_http "$GH_API/repos/$_ghd_r/releases/tags/$_ghd_t" "$_ghd_j" "" auth; then
        _ghd_listing=1
        _ghd_l="$(gh_asset_line "$_ghd_j" "$_ghd_a")"
        _ghd_s="$(gh_asset_line "$_ghd_j" SHA256SUMS)"
        _ghd_g="$(gh_asset_line "$_ghd_j" SHA256SUMS.sigstore.json)"
        [ -n "$_ghd_l" ] || _ghd_why="the $_ghd_t release lists no asset by that name"
    else
        _ghd_why="the API could not be read (no network, no such release, or its rate limit)"
    fi
    rm -f "$_ghd_j"
    _ghd_id="${_ghd_l%% *}"; _ghd_dig=""
    case "$_ghd_l" in *" "*) _ghd_dig="${_ghd_l#* }" ;; esac

    if ! gh_http "$GH_WEB/$_ghd_r/releases/download/$_ghd_t/$_ghd_a" "$_ghd_d"; then
        rm -f "$_ghd_d"
        say "  $GH_WEB/$_ghd_r/releases/download/$_ghd_t/$_ghd_a failed; trying the GitHub API"
        if [ -z "$_ghd_id" ]; then
            say "  both the release URL and the GitHub API failed for $_ghd_a ($_ghd_t): $_ghd_why"
            return 1
        fi
        if ! gh_http "$GH_API/repos/$_ghd_r/releases/assets/$_ghd_id" "$_ghd_d" octet auth; then
            rm -f "$_ghd_d"
            say "  both the release URL and the GitHub API failed for $_ghd_a ($_ghd_t): the API asset download failed"
            return 1
        fi
        say "  fetched $_ghd_a through the GitHub API"
    fi

    # Verify before anything is unpacked. A check that cannot be made is a
    # refusal, not a pass.
    _ghd_got="$(gh_sha256 "$_ghd_d")"
    if [ -z "$_ghd_got" ]; then
        rm -f "$_ghd_d"
        say "  $_ghd_a cannot be verified: no sha256 tool here (sha256sum, shasum, sha256 or openssl)."
        return 2
    fi
    if [ -n "$_ghd_l" ] && [ -n "$_ghd_dig" ] && [ "$_ghd_got" != "$_ghd_dig" ]; then
        rm -f "$_ghd_d"
        say "  $_ghd_a does not match the sha256 digest GitHub lists for it (expected $_ghd_dig, got $_ghd_got)"
        return 2
    fi

    # SHA256SUMS: when the listing names it, or when there is no listing
    # (a release that has it is checked by it).
    _ghd_sums="$_ghd_d.SHA256SUMS"
    _ghd_have_sums=0
    if [ -n "$_ghd_s" ] || [ "$_ghd_listing" = 0 ]; then
        if gh_fetch "$_ghd_r" "$_ghd_t" SHA256SUMS "${_ghd_s%% *}" "$_ghd_sums"; then
            _ghd_have_sums=1
        elif [ -n "$_ghd_s" ]; then
            rm -f "$_ghd_d"
            say "  $_ghd_a cannot be verified: the $_ghd_t release lists SHA256SUMS, and it could not be fetched. Try again later."
            return 2
        fi
    fi

    if [ "$_ghd_have_sums" = 0 ]; then
        # A signed release without SHA256SUMS: someone took it away, and the
        # digest the same person can rewrite is no check at all.
        if release_signed "$_ghd_t"; then
            rm -f "$_ghd_d"
            say "  $_ghd_a cannot be verified: $_ghd_t is at or above $SIGNED_SINCE, the first signed release, and its SHA256SUMS is missing."
            return 2
        fi
        # A release from before #3752: GitHub's digest is all there is.
        if [ -n "$_ghd_why" ]; then
            rm -f "$_ghd_d"
            say "  $_ghd_a cannot be verified: $_ghd_why, so its sha256 digest is unknown."
            say "  Try again later, or set GITHUB_TOKEN to raise the API's rate limit."
            return 2
        fi
        if [ "${#_ghd_dig}" != 64 ]; then
            rm -f "$_ghd_d"
            say "  $_ghd_a cannot be verified: the $_ghd_t release lists no sha256 digest for it."
            return 2
        fi
        say "  verified $_ghd_a against its sha256 digest"
        say "  $_ghd_t predates signed checksums (SHA256SUMS): it has no signature or build provenance to check"
        return 0
    fi

    # The archive's line: "<64 hex>  <name>" (or " *<name>"), exactly once.
    _ghd_want="$(awk -v f="$_ghd_a" '
        { line=$0; sub(/\r$/,"",line) }
        length(line) == 66 + length(f) && substr(line, 67) == f {
            h=substr(line, 1, 64); sep=substr(line, 65, 2)
            if (h !~ /^[0-9a-f]+$/ || (sep != "  " && sep != " *") || n++) { bad=1; exit }
            want=h }
        END { if (bad) print "bad"; else if (n == 1) print want }' "$_ghd_sums")"
    case "$_ghd_want" in
        "")  rm -f "$_ghd_d" "$_ghd_sums"
             say "  $_ghd_a cannot be verified: SHA256SUMS does not list it."
             return 2 ;;
        bad) rm -f "$_ghd_d" "$_ghd_sums"
             say "  $_ghd_a cannot be verified: SHA256SUMS lists it twice, or in a line sha256sum would not write."
             return 2 ;;
    esac
    if [ "$_ghd_got" != "$_ghd_want" ]; then
        rm -f "$_ghd_d" "$_ghd_sums"
        say "  $_ghd_a does not match its sha256 in SHA256SUMS (expected $_ghd_want, got $_ghd_got)"
        return 2
    fi
    if [ -n "$_ghd_dig" ]; then
        say "  verified $_ghd_a against SHA256SUMS and the sha256 digest GitHub lists"
    else
        say "  verified $_ghd_a against SHA256SUMS"
    fi

    # The identity both the signature and the provenance must carry: this
    # repository's release workflow, run from main (a release the tag job
    # made) or from this tag (a tag pushed by hand), and no other ref. The
    # repository and the tag are literal text in the pattern.
    _ghd_rr="$(regex_literal "$_ghd_r")"
    _ghd_rt="$(regex_literal "$_ghd_t")"
    _ghd_idre="^https://github\\.com/$_ghd_rr/\\.github/workflows/release\\.yml@refs/(heads/main|tags/$_ghd_rt)\$"

    # The signature of SHA256SUMS: keyless, with that identity, under
    # GitHub's token issuer. cosign 2 reads the sigstore bundle cosign 3
    # writes only with --new-bundle-format, so a refusal is asked again with
    # it.
    if have cosign; then
        _ghd_sig="$_ghd_d.sigstore.json"
        if ! gh_fetch "$_ghd_r" "$_ghd_t" SHA256SUMS.sigstore.json "${_ghd_g%% *}" "$_ghd_sig"; then
            rm -f "$_ghd_d" "$_ghd_sums"
            say "  $_ghd_a cannot be verified: SHA256SUMS.sigstore.json could not be fetched for cosign."
            return 2
        fi
        _ghd_ok=0
        for _ghd_fmt in "" --new-bundle-format; do
            # shellcheck disable=SC2086
            if cosign verify-blob $_ghd_fmt --bundle "$_ghd_sig" \
                --certificate-identity-regexp "$_ghd_idre" \
                --certificate-oidc-issuer https://token.actions.githubusercontent.com \
                "$_ghd_sums" >/dev/null 2>&1; then
                _ghd_ok=1
                break
            fi
        done
        rm -f "$_ghd_sig"
        if [ "$_ghd_ok" != 1 ]; then
            rm -f "$_ghd_d" "$_ghd_sums"
            say "  the signature of SHA256SUMS for $_ghd_t does not verify with cosign as made by $_ghd_r's release workflow"
            return 2
        fi
        say "  the signature of SHA256SUMS verified with cosign ($_ghd_r's release workflow)"
    else
        say "  the signature of SHA256SUMS was not checked: cosign is not installed"
    fi
    rm -f "$_ghd_sums"

    # The archive's build provenance, with an authenticated gh: the same
    # identity, anchored. --signer-workflow matches a prefix and so takes the
    # release workflow run from any branch or tag.
    if have gh && gh auth status >/dev/null 2>&1; then
        if ! gh attestation verify "$_ghd_d" --repo "$_ghd_r" \
            --cert-identity-regex "$_ghd_idre" \
            --cert-oidc-issuer https://token.actions.githubusercontent.com >/dev/null 2>&1; then
            rm -f "$_ghd_d"
            say "  $_ghd_a has no build provenance from $_ghd_r's release workflow that 'gh attestation verify' accepts"
            return 2
        fi
        say "  its build provenance verified with gh attestation verify"
    elif have gh; then
        say "  its build provenance was not checked: gh is not logged in"
    else
        say "  its build provenance was not checked: gh is not installed"
    fi
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
        gh_rc=0
        gh_release_download "$REPO" "$REF" "$asset" "$tmp/aether-bin.tar.gz" || gh_rc=$?
        case "$gh_rc" in
            0) install_prebuilt=1 ;;
            # Fetched but not verified: never unpacked, and no quiet fall back
            # to another way of installing it (#3748).
            2) die "refusing the prebuilt $asset: it was not verified (above). Nothing was installed." ;;
            *) say "no prebuilt for $slug at $REF; will build from source." ;;
        esac
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
