#!/usr/bin/env bash
# contrib_host_demos.sh — probe dev libs + link and run host demos.
#
# For each contrib/host/<lang>/ that has a demo in examples/, probe
# whether the language's dev headers and library are installed. If
# yes, compile the demo (via aetherc + the bridge), link against the
# bridge + libaether + the language's runtime, and run the demo.
# Report a per-language OK/SKIP/FAIL. Never hard-fail on missing
# system dev libraries — that's a skip, not a regression.
#
# Called from `make contrib-host-check`.

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
BUILD_BASE_FLAGS=(
    -O2 -DAETHER_HAS_SANDBOX
    -I"$ROOT" -I"$ROOT/runtime" -I"$ROOT/runtime/actors"
    -I"$ROOT/runtime/scheduler" -I"$ROOT/runtime/utils"
    -I"$ROOT/runtime/memory" -I"$ROOT/runtime/config"
    -I"$ROOT/std" -I"$ROOT/std/string" -I"$ROOT/std/io"
    -I"$ROOT/std/math" -I"$ROOT/std/net" -I"$ROOT/std/collections"
    -I"$ROOT/std/json"
)
LINK_BASE_FLAGS=(-L"$ROOT/build" -laether -lpthread -lm)

pass=0
skip=0
fail=0
failures=""

tmpdir=$(mktemp -d 2>/dev/null || mktemp -d -t 'contrib_host')
trap 'rm -rf "$tmpdir"' EXIT

# probe_<lang> echoes two lines on stdout when the language is
# available: "-I<path>" on line 1, "-L<path> -l<lib> ..." on line 2.
# Exits 0 if available, 1 if not. Never prints to stderr.

probe_ruby() {
    for v in ruby-3.2 ruby-3.1 ruby-3.0 ruby; do
        if pkg-config --exists "$v" 2>/dev/null; then
            pkg-config --cflags-only-I "$v"
            pkg-config --libs "$v"
            return 0
        fi
    done
    return 1
}

probe_perl() {
    if command -v perl >/dev/null 2>&1; then
        ccopts=$(perl -MExtUtils::Embed -e ccopts 2>/dev/null) || return 1
        ldopts=$(perl -MExtUtils::Embed -e ldopts 2>/dev/null) || return 1
        echo "$ccopts"
        echo "$ldopts"
        return 0
    fi
    return 1
}

probe_tcl() {
    if pkg-config --exists tcl 2>/dev/null; then
        pkg-config --cflags-only-I tcl
        pkg-config --libs tcl
        return 0
    fi
    # macOS system Tcl — fall back to SDK probe
    sdk=$(xcrun --show-sdk-path 2>/dev/null)
    if [ -n "$sdk" ] && [ -f "$sdk/usr/include/tcl.h" ]; then
        echo "-I$sdk/usr/include"
        echo "-framework Tcl"
        return 0
    fi
    return 1
}

probe_duktape() {
    # Duktape — either pkg-config or a known header path.
    if pkg-config --exists duktape 2>/dev/null; then
        pkg-config --cflags-only-I duktape
        pkg-config --libs duktape
        return 0
    fi
    return 1
}

probe_go() {
    # Go host runs as a separate subprocess (like the aether host).
    # No libgo embedding — just need the `go` binary in PATH at runtime.
    if command -v go >/dev/null 2>&1; then
        echo ""   # no -I needed; the bridge is pure POSIX C
        echo ""   # no -L/-l needed
        return 0
    fi
    return 1
}

probe_tinygo() {
    # TinyGo host loads c-shared .so/.dylib/.dll via std.dl. The C
    # bridge itself is dlopen-only (no Go runtime in-process), so the
    # only requirement at link time is libdl on Linux. macOS ships
    # dlopen in libc — no extra -l flag.
    #
    # If libffi is present, also enable AETHER_HAS_LIBFFI so the
    # tinygo.call_dynamic escape hatch is exposed for unusual
    # signatures the fixed wrappers don't cover.
    if command -v tinygo >/dev/null 2>&1; then
        ffi_cflags=""
        ffi_libs=""
        if pkg-config --exists libffi 2>/dev/null; then
            ffi_cflags="-DAETHER_HAS_LIBFFI $(pkg-config --cflags libffi 2>/dev/null)"
            ffi_libs="$(pkg-config --libs libffi 2>/dev/null)"
        fi
        echo "$ffi_cflags"
        case "$(uname -s)" in
            Linux)  echo "-ldl $ffi_libs" ;;
            *)      echo "$ffi_libs" ;;
        esac
        return 0
    fi
    return 1
}

# Build + run one language's demo. $1 = lang, $2 = LANG_UPPER,
# $3 = flag_suffix (AETHER_HAS_<LANG>). Returns 0 success, 1 fail.
run_demo() {
    local lang="$1"
    local flag="$2"
    local demo="$ROOT/examples/04-interop/05-hosts/${lang}.ae"

    if [ ! -f "$demo" ]; then
        return 2  # no demo, not a failure
    fi

    # Get per-language flags from the probe.
    local probe_out
    if ! probe_out=$("probe_${lang}"); then
        return 3  # dev lib not available, skip
    fi
    local inc_flags link_flags
    inc_flags=$(echo "$probe_out" | sed -n '1p')
    link_flags=$(echo "$probe_out" | sed -n '2p')

    local src_c="$tmpdir/${lang}_demo.c"
    local bin="$tmpdir/${lang}_demo_bin"

    if ! "$AETHERC" "$demo" "$src_c" >"$tmpdir/${lang}_aetherc.log" 2>&1; then
        cp "$tmpdir/${lang}_aetherc.log" "$tmpdir/${lang}_fail.log"
        return 1
    fi

    # Assemble link command. Use arrays; let the shell glob per-language
    # flags which may be multi-word (e.g. "-I/a -I/b -DPY_SSIZE_T_CLEAN").
    # shellcheck disable=SC2086
    if ! $CC "${BUILD_BASE_FLAGS[@]}" -D"$flag" $inc_flags \
            "$src_c" "$ROOT/contrib/host/${lang}/aether_host_${lang}.c" \
            "${LINK_BASE_FLAGS[@]}" $link_flags \
            -o "$bin" >"$tmpdir/${lang}_link.log" 2>&1; then
        cp "$tmpdir/${lang}_link.log" "$tmpdir/${lang}_fail.log"
        return 1
    fi

    # Run the demo. Don't trust stdout; just check exit code.
    if ! "$bin" >"$tmpdir/${lang}_run.log" 2>&1; then
        cp "$tmpdir/${lang}_run.log" "$tmpdir/${lang}_fail.log"
        return 1
    fi

    return 0
}

CC="${CC:-cc}"
printf "  using CC=%s\n" "$CC"
printf "\n"

for lang in duktape perl ruby tcl go tinygo; do
    flag=""
    case "$lang" in
        duktape) flag="AETHER_HAS_DUKTAPE" ;;
        perl)    flag="AETHER_HAS_PERL" ;;
        ruby)    flag="AETHER_HAS_RUBY" ;;
        tcl)     flag="AETHER_HAS_TCL" ;;
        go)      flag="AETHER_HAS_GO" ;;
        tinygo)  flag="AETHER_HAS_TINYGO" ;;
    esac

    printf "  %-10s " "$lang"
    run_demo "$lang" "$flag"
    rc=$?
    case $rc in
        0) echo "PASS (built + ran)"; pass=$((pass + 1)) ;;
        2) echo "SKIP (no demo)"; skip=$((skip + 1)) ;;
        3) echo "SKIP (dev lib not installed)"; skip=$((skip + 1)) ;;
        *) echo "FAIL"
           if [ -f "$tmpdir/${lang}_fail.log" ]; then
               sed 's/^/    /' "$tmpdir/${lang}_fail.log" | head -10
           fi
           fail=$((fail + 1))
           failures="$failures $lang" ;;
    esac
done

printf "\n  %d passed, %d skipped, %d failed\n" "$pass" "$skip" "$fail"
if [ "$fail" -gt 0 ]; then
    printf "  failing: %s\n" "$failures"
    exit 1
fi
