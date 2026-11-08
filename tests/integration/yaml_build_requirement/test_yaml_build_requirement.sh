#!/bin/sh
# std.yaml's build requirement says so at build time, and the toolchain does
# not carry it.
#
# The 0.889.0 release archives (macOS arm64 and Linux x86_64 were checked)
# shipped std.yaml's stubs: the runners had no libfyaml, YAML=auto found
# nothing and said nothing, and every yaml call in a user's program returned
# "std.yaml unavailable: this build has no libfyaml". YAML=1, which the stub
# message itself recommended, did the same as auto. Now:
#   - YAML=1 stops make when pkg-config finds no libfyaml (the release builds
#     pass it), naming the packages;
#   - ae and aetherc link without libfyaml even in a build that has it, so a
#     release built with it still starts on a machine without it (only a
#     program importing std.yaml links the library);
#   - in a build with libfyaml, a yaml program parses.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] yaml_build_requirement: ae not built"; exit 0; }
command -v "${MAKE:-make}" >/dev/null 2>&1 || { echo "  [SKIP] yaml_build_requirement: no make"; exit 0; }
command -v pkg-config >/dev/null 2>&1 || { echo "  [SKIP] yaml_build_requirement: no pkg-config"; exit 0; }
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) echo "  [SKIP] yaml_build_requirement: the Windows build has no libfyaml leg"; exit 0 ;;
esac

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

# --- 1. YAML=1 without libfyaml stops the build --------------------------
# An empty pkg-config search path stands for a machine without it. make -n:
# the check is at parse time, so nothing is built either way.
out="$(cd "$ROOT" && PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$tmp/none" ${MAKE:-make} -n YAML=1 compiler 2>&1)"
rc=$?
case "$out" in
    *"YAML=1 but pkg-config finds no libfyaml"*) [ "$rc" -ne 0 ] || {
        echo "  [FAIL] yaml_build_requirement: YAML=1 named the missing libfyaml but make exited 0"; fail=1; } ;;
    *) echo "  [FAIL] yaml_build_requirement: YAML=1 without libfyaml did not stop the build"
       echo "$out" | tail -3 | sed 's/^/        /'; fail=1 ;;
esac
# YAML=0 and auto still build without it.
for mode in 0 auto; do
    if ! (cd "$ROOT" && PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$tmp/none" ${MAKE:-make} -n YAML=$mode compiler >/dev/null 2>&1); then
        echo "  [FAIL] yaml_build_requirement: YAML=$mode without libfyaml stopped the build"; fail=1
    fi
done

# --- 2. the toolchain does not link libfyaml ------------------------------
if command -v otool >/dev/null 2>&1; then
    deps="$(otool -L "$ROOT/build/ae" "$ROOT/build/aetherc" 2>&1)"
elif command -v ldd >/dev/null 2>&1; then
    deps="$(ldd "$ROOT/build/ae" "$ROOT/build/aetherc" 2>&1)"
else
    deps=""
fi
if echo "$deps" | grep -q fyaml; then
    echo "  [FAIL] yaml_build_requirement: ae or aetherc links libfyaml"
    echo "$deps" | grep fyaml | sed 's/^/        /'; fail=1
fi

# --- 3. with libfyaml, a yaml program parses ------------------------------
if pkg-config --exists libfyaml 2>/dev/null; then
    cat > "$tmp/y.ae" <<'AE'
import std.yaml
import std.string

main() {
    doc, err = yaml.parse("name: widget\n")
    if string.length(err) > 0 {
        println("err ${err}")
        return
    }
    defer yaml.free(doc)
    name, _e = yaml.get_scalar(yaml.mapping_lookup(yaml.root(doc), "name"))
    println("ok ${name}")
}
AE
    got="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" run y.ae 2>&1 | tail -1)"
    if [ "$got" != "ok widget" ]; then
        echo "  [FAIL] yaml_build_requirement: a yaml program did not parse in a build with libfyaml (got '$got')"
        fail=1
    fi
else
    echo "  [SKIP] yaml_build_requirement: no libfyaml here, the parse check needs it"
fi

[ "$fail" = 0 ] && echo "  [PASS] yaml_build_requirement"
exit $fail
