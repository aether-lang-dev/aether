#!/bin/sh
# #2297: a binary library's actors run in a program that has none of its own.
#
# crowd spawns actors from its exports; relay, a second library, calls crowd
# and has no actors itself; app imports relay and has no actors either.
# Before, nothing initialized the scheduler: only a program's own main()
# did, and only when the program defined actors, so the second spawn divided
# by a zero core count. Now the first spawn initializes the scheduler if
# nothing has, and a library's catalog (schema 1.7) says it runs actors,
# relay's included, so app's main() runs the scheduler and drains it on the
# way out: crowd's slow fire-and-forget message is still delivered.
#
# Two passes over the same sources:
#   static runtime  each library carries its own libaether. The actors run
#                   wherever the importer's calls land; the exit drain reaches
#                   them only where ELF interposition merges the copies
#                   (Linux, FreeBSD). Elsewhere `ae` warns that it cannot.
#   shared runtime  `--shared-runtime`: one runtime, one scheduler, and the
#                   program's exit drains it on every platform.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] binary_import_actors: ae not built"; exit 0; }

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) SO_EXT=".dll";   ELF=0; EXE=".exe" ;;
    Darwin)                          SO_EXT=".dylib"; ELF=0; EXE="" ;;
    Linux|FreeBSD)                   SO_EXT=".so";    ELF=1; EXE="" ;;
    *)                               SO_EXT=".so";    ELF=0; EXE="" ;;
esac

fail() {
    echo "  [FAIL] binary_import_actors: $1"
    if [ -n "$2" ] && [ -f "$2" ]; then sed 's/^/    /' "$2" | head -30; fi
    exit 1
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK" || true' EXIT

# run_pass <dir> <ae build flags for the libraries> <label> <expect the drain: 1|0>
run_pass() {
    dir="$1"; flags="$2"; label="$3"; drains="$4"
    mkdir -p "$dir"
    cp "$SCRIPT_DIR/crowd.ae" "$SCRIPT_DIR/relay.ae" "$SCRIPT_DIR/app.ae" "$dir/"
    cd "$dir"

    AETHER_HOME="$ROOT" "$AE" build --emit=lib $flags crowd.ae -o "libcrowd$SO_EXT" >crowd.log 2>&1 \
        || fail "$label: ae build --emit=lib crowd.ae" crowd.log
    rm -f crowd.ae
    AETHER_HOME="$ROOT" "$AE" build --emit=lib $flags relay.ae -o "librelay$SO_EXT" >relay.log 2>&1 \
        || fail "$label: ae build --emit=lib relay.ae (importing crowd's binary)" relay.log
    rm -f relay.ae

    for lib in crowd relay; do
        INFO="$(AETHER_HOME="$ROOT" "$AE" lib-info "./lib$lib$SO_EXT" 2>&1)" || { echo "$INFO"; fail "$label: ae lib-info lib$lib"; }
        echo "$INFO" | grep -q "Schema:[[:space:]]*1\.7" || { echo "$INFO"; fail "$label: lib$lib's schema is not 1.7"; }
        echo "$INFO" | grep -q "Actors:[[:space:]]*yes" || { echo "$INFO"; fail "$label: lib$lib's catalog does not say it runs actors"; }
    done

    OUT="$(AETHER_HOME="$ROOT" "$AE" run app.ae 2>run.log)" || { echo "$OUT"; fail "$label: ae run app.ae" run.log; }
    echo "$OUT" | grep -q "^OK" || { echo "$OUT"; fail "$label: the actors did not count to 300"; }
    if [ "$drains" = 1 ]; then
        echo "$OUT" | grep -q "^late 7" || { echo "$OUT"; fail "$label: the exit did not drain the library's in-flight message" run.log; }
    elif [ "$ELF" = 0 ]; then
        grep -q "runs actors on its own static runtime" run.log \
            || fail "$label: no warning that the static runtime's messages are not drained" run.log
    fi

    AETHER_HOME="$ROOT" "$AE" build app.ae -o app >app.log 2>&1 || fail "$label: ae build app.ae" app.log
    OUT2="$(./app$EXE 2>&1)" || { echo "$OUT2"; fail "$label: the built program failed"; }
    echo "$OUT2" | grep -q "^OK" || { echo "$OUT2"; fail "$label: the built program did not count to 300"; }
    if [ "$drains" = 1 ]; then
        echo "$OUT2" | grep -q "^late 7" || { echo "$OUT2"; fail "$label: the built program's exit did not drain"; }
    fi
    cd "$WORK"
}

# ---- static runtime ----
run_pass "$WORK/static" "" "static runtime" "$ELF"

# ---- shared runtime (where this toolchain built one) ----
if [ -d "$ROOT/build/shared" ]; then
    run_pass "$WORK/shared" "--shared-runtime" "shared runtime" 1
    echo "  [PASS] binary_import_actors: a library's actors run in a program without its own, and its exit drains them"
else
    echo "  [PASS] binary_import_actors: a library's actors run in a program without its own (no shared runtime built here)"
fi
