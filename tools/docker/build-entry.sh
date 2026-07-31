#!/bin/sh
# Aether build container entrypoint.
#
# Two dispatch modes:
#
#   1. Direct ae build (the common case):
#        docker run ... aether-builder myprog.ae [output_name] [extra ae args...]
#      Compiles /work/myprog.ae to /out/<output_name>
#      (default name: source basename minus .ae). Extra args pass
#      through to `ae build` after `-o /out/<name>`. Use this for
#      --emit=lib, --no-contracts, etc.
#
#   2. aeb-driven build (polyglot monorepo case):
#        docker run ... aether-builder --aeb [target] [extra aeb args...]
#      Runs aeb against the bind-mounted source tree, then copies
#      every artifact under /work/target/ that matches *_binary or
#      bin/* into /out/. Lets a Bazzite-style host with no toolchain
#      drive an aeb build entirely through the container.
#
# Why /work read-only is supported:
#   ae build defaults to writing the output to the same dir as the
#   source unless `-o` overrides. We always pass `-o /out/...`, so
#   /work staying read-only is fine — no temp files land in /work.
#   aeb builds DO need to write target/ next to the source, so the
#   --aeb path requires /work to be writable (the user drops the
#   :ro on the mount).

set -eu

usage() {
    cat >&2 <<EOF
Aether builder container.

Direct ae build:
  $0 <source.ae> [<output_name>] [extra ae args...]

aeb-driven build:
  $0 --aeb [<target>] [extra aeb args...]

Examples:
  $0 hello.ae
  $0 hello.ae greeter --emit=lib
  $0 --aeb
  $0 --aeb tools/aeb-main.build.ae
EOF
    exit 2
}

if [ "$#" -lt 1 ]; then
    usage
fi

case "$1" in
    --help|-h)
        usage
        ;;
    --aeb)
        shift
        # aeb writes target/ next to its sources. Require /work to be
        # writable; the user must drop :ro from the bind mount.
        if [ ! -w /work ]; then
            echo "build-entry: /work is read-only; aeb needs to write target/. Drop :ro from the bind mount." >&2
            exit 1
        fi
        if ! command -v aeb >/dev/null 2>&1; then
            # --aeb is a guarded extension seam: the aether-builder
            # base ships ae/aetherc but not aeb. aeb's own Docker
            # layer is expected to FROM this base, install aeb, and
            # inherit this entrypoint so the --aeb branch goes live.
            echo "build-entry: --aeb needs an aeb-bearing image."                    >&2
            echo "  The aether-builder base ships ae/aetherc but NOT aeb."           >&2
            echo "  Layer aeb on top: see aeb's tools/container/Containerfile.aeb-toolchain" >&2
            echo "  ( https://github.com/aether-lang-dev/aeb )"                      >&2
            echo "  Then re-run with the aeb image:"                                 >&2
            echo "    podman run ... aeb-toolchain:slim --aeb <target>"              >&2
            exit 1
        fi
        aeb "$@"
        # Copy artifacts to /out. aeb's output convention is target/<module>/
        # with binaries under bin/. Generalise to "anything executable
        # under target/" plus the target/ directory metadata.
        if [ -d /work/target ]; then
            find /work/target -type f \( -perm -u+x -o -name '*.so' -o -name '*.a' -o -name '*.jar' \) \
                -exec sh -c 'install -D "$1" "/out/${1#/work/target/}"' _ {} \;
            echo "build-entry: extracted artifacts to /out/"
        fi
        ;;
    -*)
        echo "build-entry: unknown flag '$1'. Use --help for usage." >&2
        exit 2
        ;;
    *)
        src="$1"
        shift
        out_name="${1:-}"
        if [ -n "$out_name" ]; then
            shift
        else
            out_name="$(basename "$src" .ae)"
        fi
        if [ ! -f "/work/$src" ]; then
            echo "build-entry: /work/$src not found" >&2
            exit 1
        fi
        # Remaining args pass through to ae build.
        ae build "/work/$src" -o "/out/$out_name" "$@"
        echo "build-entry: wrote /out/$out_name"
        ls -l "/out/$out_name"
        ;;
esac
