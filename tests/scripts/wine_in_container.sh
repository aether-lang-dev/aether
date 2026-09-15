#!/bin/sh
#
# `wine` for a host that has no x86 Wine of its own (an arm64 Mac, or any
# machine where installing Wine is unwelcome): forward every invocation into
# an x86_64 Linux container that has Wine installed and the working directory
# mounted at the SAME absolute path.
#
# Use it as the sweep's WINE:
#
#   WINE=tests/scripts/wine_in_container.sh TMPDIR=<mounted dir> \
#       bash tests/scripts/windows_wine_sweep.sh
#
# Env:  WINEBOX=winebox          container name
#       CONTAINER_ENGINE=docker  or podman; auto-detected when unset
#
# See docs/build-system.md, "Reproducing the Wine lane locally".

set -u

box="${WINEBOX:-winebox}"
engine="${CONTAINER_ENGINE:-}"
if [ -z "$engine" ]; then
    if command -v docker > /dev/null 2>&1; then engine=docker
    elif command -v podman > /dev/null 2>&1; then engine=podman
    else
        echo "wine_in_container: neither docker nor podman on PATH" >&2
        exit 127
    fi
fi

if ! "$engine" container inspect "$box" > /dev/null 2>&1; then
    echo "wine_in_container: no container named '$box'; start one first (docs/build-system.md)" >&2
    exit 127
fi

# -i keeps stdin attached so a program that reads it behaves as it would
# under a bare `wine`. No -t: a TTY would turn the captured output into
# CRLF-terminated lines and confuse any test that compares bytes.
exec "$engine" exec -i \
    -e WINEPREFIX="${WINEPREFIX_IN_BOX:-/root/.wineprefix}" \
    -e WINEDEBUG="${WINEDEBUG:--all}" \
    -e WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree=d;mshtml=d}" \
    "$box" wine "$@"
