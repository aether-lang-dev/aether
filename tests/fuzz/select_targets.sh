#!/bin/sh
# Which fuzz targets to run (tests/fuzz/fuzz_ci.sh's FUZZ_TARGETS).
#
#   sh tests/fuzz/select_targets.sh --all [--shard I/N]
#   sh tests/fuzz/select_targets.sh --changed FILE [--shard I/N]
#
# --all prints every target. --changed prints the targets whose inputs a list
# of changed paths (FILE, one a line, as `git diff --name-only` writes it)
# reaches: a decoder's own sources, what it is built on, its harness and
# seeds. A change to what every target is built with (the runtime, the
# compiler and ae, std.string, std.bytes, std.mem, the Makefile, the runner,
# the workflow) reaches them all. --shard I/N keeps the I-th of N shares, the
# targets taken in turn, so N jobs split the list evenly. The output is one
# line of names separated by spaces, empty when nothing is reached.
#
# The table below is the one place a target's inputs are named; the
# workflow's paths filter (.github/workflows/fuzz.yml) covers the same
# paths, and test_fuzz_targets_listed checks that every target directory is
# in it.

TABLE='
cbor            std/cbor/ tests/fuzz/cbor/
ipc_frames      std/ipc/ std/cbor/ tests/fuzz/ipc_frames/
http_head       std/net/ tests/fuzz/http_head/
http_chunked    std/net/ tests/fuzz/http_chunked/
url             std/net/ tests/fuzz/url/
json            std/json/ tests/fuzz/json/
websocket       std/net/ tests/fuzz/websocket/
h2              std/http/server/h2/ std/net/ tests/fuzz/h2/
proxy_response  std/http/proxy/ std/net/ tests/fuzz/proxy_response/
xml             std/xml/ tests/fuzz/xml/
yaml            std/yaml/ tests/fuzz/yaml/
msgpack         std/msgpack/ std/list/ tests/fuzz/msgpack/
tar             std/tar/ std/fs/ std/io/ std/list/ tests/fuzz/tar/
zip             std/zip/ std/zlib/ std/hash/ std/fs/ std/io/ std/list/ tests/fuzz/zip/
tls13_record    std/cryptography/ tests/fuzz/tls13_record/
tls13_handshake std/cryptography/ std/list/ tests/fuzz/tls13_handshake/
x509            std/cryptography/ std/list/ tests/fuzz/x509/
'

# Paths every target is built with.
SHARED='runtime/ compiler/ tools/ std/string/ std/bytes/ std/mem/ Makefile
tests/fuzz/run_fuzz_c.sh tests/fuzz/fuzz_ci.sh tests/fuzz/replay_main.c
tests/fuzz/ae_target.c tests/fuzz/select_targets.sh .github/workflows/fuzz.yml
.github/actions/apt-install/'

usage() {
    echo "usage: $0 --all | --changed FILE   [--shard I/N]" >&2
    exit 2
}

mode=""; changed=""; shard=""
while [ $# -gt 0 ]; do
    case "$1" in
        --all) mode=all ;;
        --changed) mode=changed; shift; changed="$1" ;;
        --shard) shift; shard="$1" ;;
        *) usage ;;
    esac
    shift
done
[ -n "$mode" ] || usage

all=$(printf '%s\n' "$TABLE" | awk 'NF { print $1 }')

# Does any changed path start with one of the prefixes given?
touches() {
    for prefix in "$@"; do
        while IFS= read -r path; do
            case "$path" in "$prefix"*) return 0 ;; esac
        done < "$changed"
    done
    return 1
}

if [ "$mode" = all ]; then
    picked="$all"
else
    [ -f "$changed" ] || { echo "$0: $changed: no such file" >&2; exit 2; }
    # shellcheck disable=SC2086
    if touches $SHARED; then
        picked="$all"
    else
        picked=""
        for t in $all; do
            # shellcheck disable=SC2046
            if touches $(printf '%s\n' "$TABLE" | awk -v t="$t" '$1 == t { $1 = ""; print }'); then
                picked="$picked $t"
            fi
        done
    fi
fi

if [ -n "$shard" ]; then
    i="${shard%/*}"; n="${shard#*/}"
    case "$i$n" in *[!0-9]*|"") usage ;; esac
    [ "$i" -ge 1 ] && [ "$i" -le "$n" ] || usage
    k=0; kept=""
    for t in $picked; do
        [ $((k % n + 1)) -eq "$i" ] && kept="$kept $t"
        k=$((k + 1))
    done
    picked="$kept"
fi

# shellcheck disable=SC2086
echo $picked
