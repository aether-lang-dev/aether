#!/bin/sh
# getsh_download.sh TAG ASSET DEST : run get.sh's gh_release_download on its
# own, for TAG of the repository get.sh names, and exit with its status.
#
# get.sh only installs a prebuilt archive on Linux, macOS and FreeBSD, so on
# Windows (Git Bash, MSYS2) it never reaches the download and its checks. The
# checks are what a Windows-only argument rewriting broke in
# scripts/verify-release-asset.sh, and what this runs there: get.sh up to the
# line where its work starts, then the one function. Used by
# test_getsh_native_args.sh and by ci.yml's ci-windows-release-verify.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
GET_SH="${GET_SH:-$HERE/../../../get.sh}"
[ $# = 3 ] || { echo "usage: getsh_download.sh TAG ASSET DEST" >&2; exit 64; }
tag="$1"; asset="$2"; dest="$3"
fns="$(mktemp)" || exit 1
trap 'rm -f "$fns"' EXIT
sed '/^tmp=\$(mktemp -d)$/,$d' "$GET_SH" > "$fns"
grep -q '^gh_release_download()' "$fns" || { echo "getsh_download.sh: no gh_release_download in $GET_SH" >&2; exit 65; }
set -- "$tag"
. "$fns"
rc=0
gh_release_download "$REPO" "$tag" "$asset" "$dest" || rc=$?
exit "$rc"
