#!/usr/bin/env sh
# Aether remote installer — fetch a pinned source tarball and `make install`.
#
# The one-command, no-clone path (mirrors aeb's install.sh). For the full
# clone-time installer (editor extension, `ae version` management, shell-rc
# setup, `~/.aether` layout) use ./install.sh after `git clone`; for the
# from-HEAD developer flow see docs/bootstrap-from-source.md.
#
# Usage:
#   curl -sSL https://raw.githubusercontent.com/aether-lang-dev/aether/main/get.sh | sh
#   AETHER_REF=v0.184.0 sh get.sh
#   AETHER_REF=v0.184.0 PREFIX=/usr/local sh get.sh   # system-wide (needs sudo)
#
# Env knobs:
#   AETHER_REF  tag (vX.Y.Z), branch, or commit SHA to install.
#               Default: the latest vX.Y.Z tag (falls back to `main`).
#   PREFIX      install prefix. Default: $HOME/.local  (no sudo).
#   CC          C compiler to build with. Default: cc (gcc/clang).
#
# Aether compiles to C, so the only build prerequisites are a C compiler
# and GNU make — there is no chicken-and-egg toolchain dependency. Tests
# are NOT run (building + installing is enough to use Aether).
set -eu

REPO="aether-lang-dev/aether"
PREFIX="${PREFIX:-$HOME/.local}"
CC="${CC:-cc}"

say()  { printf 'aether-install: %s\n' "$*"; }
die()  { printf 'aether-install: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

have curl || die "curl is required."
have tar  || die "tar is required."
have make || die "GNU make is required."
have "$CC" || die "a C compiler ('$CC') is required — install gcc or clang, or set CC."

# Resolve the ref to install. Default: highest vX.Y.Z tag on the remote.
REF="${AETHER_REF:-}"
if [ -z "$REF" ]; then
    latest=$(curl -fsSL "https://api.github.com/repos/$REPO/tags?per_page=100" 2>/dev/null \
        | grep -o '"name"[[:space:]]*:[[:space:]]*"v[0-9][0-9.]*"' \
        | sed -n 's/.*"\(v[0-9][0-9.]*\)".*/\1/p' \
        | sort -t. -k1.2,1n -k2,2n -k3,3n | tail -1)
    if [ -n "$latest" ]; then
        REF="$latest"
    else
        REF="main"
        say "no vX.Y.Z tag found; falling back to 'main' (not pinned)."
    fi
fi

say "installing aether @ $REF  ->  PREFIX=$PREFIX  (CC=$CC)"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

# GitHub serves a source tarball for any ref (tag/branch/sha) at this URL.
url="https://github.com/$REPO/archive/$REF.tar.gz"
say "fetching $url"
curl -fSL "$url" -o "$tmp/aether.tar.gz" || die "download failed for ref '$REF'."
tar -xzf "$tmp/aether.tar.gz" -C "$tmp" || die "extract failed."

# GitHub names the top dir <repo>-<ref> (leading 'v' stripped for tags).
src=$(find "$tmp" -mindepth 1 -maxdepth 1 -type d -name 'aether-*' | head -1)
[ -n "$src" ] && [ -d "$src" ] || die "could not locate extracted source dir."

say "building + installing (no tests run) — this compiles the toolchain, ~1-2 min"
make -C "$src" install PREFIX="$PREFIX" CC="$CC"

bin="$PREFIX/bin/ae"
[ -x "$bin" ] || die "install finished but $bin is missing."
say "installed: $bin"
"$bin" version || true

case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) say "note: $PREFIX/bin is not on your PATH — add it to use 'ae' directly." ;;
esac

say "optional: native contrib modules (sqlite, host_python, …) — from a"
say "          checkout run 'make contrib && make install-contrib PREFIX=$PREFIX'"
say "          (built only where the dev libraries are present)."
say "done. Pin this in CI with: AETHER_REF=$REF"
