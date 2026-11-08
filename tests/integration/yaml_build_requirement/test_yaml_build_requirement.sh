#!/bin/sh
# std.yaml's libfyaml: which builds need a system library, and which link one.
#
# History. The 0.889.0 release archives shipped std.yaml's stubs (YAML=auto
# found no libfyaml on the runners and said nothing). YAML=1 then made the
# release require a system libfyaml, and so put -lfyaml in `ae cflags
# --libs`, which every program links: on a machine without libfyaml even
# hello-world failed to link ("cannot find -lfyaml", aeb's release runner,
# 2026-11-08). libfyaml is now vendored (YAML=vendored, the default). So:
#   - YAML=1 still stops make when pkg-config finds no libfyaml, naming the
#     packages; YAML=0, auto and vendored build without it, and auto then
#     takes the vendored copy;
#   - ae and aetherc never link libfyaml;
#   - a vendored build names no libfyaml in `ae cflags --libs`, and a yaml
#     program parses, on a machine with or without the library.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] yaml_build_requirement: ae not built"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

# --- 1. YAML=1 without libfyaml stops the build; the others do not ---------
# An empty pkg-config search path stands for a machine without it. make -n:
# the check is at parse time, so nothing is built either way.
if command -v "${MAKE:-make}" >/dev/null 2>&1 && command -v pkg-config >/dev/null 2>&1; then
    out="$(cd "$ROOT" && PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$tmp/none" ${MAKE:-make} -n YAML=1 compiler 2>&1)"
    rc=$?
    case "$out" in
        *"YAML=1 but pkg-config finds no libfyaml"*) [ "$rc" -ne 0 ] || {
            echo "  [FAIL] yaml_build_requirement: YAML=1 named the missing libfyaml but make exited 0"; fail=1; } ;;
        *) echo "  [FAIL] yaml_build_requirement: YAML=1 without libfyaml did not stop the build"
           echo "$out" | tail -3 | sed 's/^/        /'; fail=1 ;;
    esac
    for mode in 0 auto vendored; do
        if ! (cd "$ROOT" && PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$tmp/none" ${MAKE:-make} -n YAML=$mode compiler >/dev/null 2>&1); then
            echo "  [FAIL] yaml_build_requirement: YAML=$mode without libfyaml stopped the build"; fail=1
        fi
    done
    # auto without a system library is the vendored build, not the stubs:
    # -B so make -n prints the compile even when the object is up to date.
    out="$(cd "$ROOT" && PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$tmp/none" ${MAKE:-make} -n -B YAML=auto build/obj/std/yaml/aether_yaml.o 2>&1 || true)"
    case "$out" in
        *-DAETHER_VENDOR_YAML*) ;;
        *) echo "  [FAIL] yaml_build_requirement: YAML=auto without libfyaml did not pick the vendored copy"; fail=1 ;;
    esac
else
    echo "  [SKIP] yaml_build_requirement: no make or pkg-config, the build-mode checks need them"
fi

# --- 2. the toolchain does not link libfyaml ------------------------------
# Library lines only: otool and ldd head each file with its own path, which
# may itself contain "libfyaml" (a checkout named so).
if command -v otool >/dev/null 2>&1; then
    deps="$(otool -L "$ROOT/build/ae" "$ROOT/build/aetherc" 2>&1 | grep -v ':$')"
elif command -v ldd >/dev/null 2>&1; then
    deps="$(ldd "$ROOT/build/ae" "$ROOT/build/aetherc" 2>&1 | grep -v ':$')"
else
    deps=""
fi
if echo "$deps" | grep -q 'libfyaml[.-]'; then
    echo "  [FAIL] yaml_build_requirement: ae or aetherc links libfyaml"
    echo "$deps" | grep fyaml | sed 's/^/        /'; fail=1
fi

# --- 3. a vendored build links no libfyaml, and parses ---------------------
vendored=0
if [ -f "$ROOT/build/libaether.a" ] && command -v nm >/dev/null 2>&1 \
   && nm "$ROOT/build/libaether.a" 2>/dev/null | grep -Eq ' T _?aether_fy_document_build_from_string$'; then
    vendored=1
fi
if [ "$vendored" = 1 ]; then
    libs="$("$AE" cflags --libs 2>&1)"
    case " $libs " in
        *" -lfyaml "*) echo "  [FAIL] yaml_build_requirement: a vendored build's 'ae cflags --libs' names -lfyaml"
                       echo "        $libs"; fail=1 ;;
    esac
fi
if [ "$vendored" = 1 ] || { command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libfyaml 2>/dev/null; }; then
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
        echo "  [FAIL] yaml_build_requirement: a yaml program did not parse (got '$got')"
        fail=1
    fi
else
    echo "  [SKIP] yaml_build_requirement: a YAML=0 build, nothing parses"
fi

[ "$fail" = 0 ] && echo "  [PASS] yaml_build_requirement"
exit $fail
