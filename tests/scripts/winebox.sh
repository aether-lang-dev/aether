#!/bin/sh
#
# Start (or stop) the x86_64 Linux container that tests/scripts/wine_in_container.sh
# forwards into. One container, Ubuntu 24.04 with the distro Wine, which is
# the same Wine CI's "Windows / runtime under Wine" lane installs.
#
#   tests/scripts/winebox.sh up <dir>    create + provision, mounting <dir> at
#                                        the same absolute path inside
#   tests/scripts/winebox.sh down        remove it
#
# <dir> must hold everything the sweep will hand to Wine: point TMPDIR at it
# when running windows_wine_sweep.sh so the .exe paths resolve on both sides.
#
# Env:  WINEBOX=winebox          container name
#       CONTAINER_ENGINE=docker  or podman; auto-detected when unset
#
# See docs/build-system.md, "Reproducing the Wine lane locally".

set -eu

box="${WINEBOX:-winebox}"
engine="${CONTAINER_ENGINE:-}"
if [ -z "$engine" ]; then
    if command -v docker > /dev/null 2>&1; then engine=docker
    elif command -v podman > /dev/null 2>&1; then engine=podman
    else
        echo "winebox: neither docker nor podman on PATH" >&2
        exit 127
    fi
fi

usage() {
    echo "usage: $0 up <dir> | down" >&2
    exit 2
}

case "${1:-}" in
up)
    dir="${2:-}"
    [ -n "$dir" ] || usage
    mkdir -p "$dir"
    dir="$(cd "$dir" && pwd -P)"

    if "$engine" container inspect "$box" > /dev/null 2>&1; then
        echo "winebox: '$box' already exists; run '$0 down' first"
        exit 1
    fi

    # --platform linux/amd64 is the whole point on an arm64 host: the PE is
    # x86_64 and so is the Wine that runs it. Under Docker Desktop or Podman
    # machine with Rosetta enabled the container is emulated transparently.
    # The mount uses the same path on both sides so an .exe path printed by
    # the sweep on the host is the path Wine opens in the box.
    "$engine" run -d --name "$box" --platform linux/amd64 \
        -v "$dir:$dir" docker.io/library/ubuntu:24.04 sleep infinity > /dev/null

    "$engine" exec "$box" sh -c '
        apt-get update > /dev/null &&
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
            wine file > /dev/null'

    # Boot the prefix once, serially, so the first parallel sweep job does
    # not race N siblings to create it.
    "$engine" exec \
        -e WINEPREFIX=/root/.wineprefix -e WINEDEBUG=-all \
        -e WINEDLLOVERRIDES="mscoree=d;mshtml=d" \
        "$box" sh -c 'wine wineboot --init > /dev/null 2>&1 || true; wine --version'

    echo "winebox: '$box' is up with $dir mounted at the same path"
    ;;
down)
    "$engine" rm -f "$box" > /dev/null 2>&1 || true
    echo "winebox: '$box' removed"
    ;;
*)
    usage
    ;;
esac
