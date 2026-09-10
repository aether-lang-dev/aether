#!/bin/sh
# #1988: native link requirements follow the resolved import graph in both
# ae build and ae run. Reject unwanted libraries before invoking the linker,
# so --as-needed cannot hide the defect on a well-provisioned developer box.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
AE="$ROOT/build/ae"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
export AETHER_HOME="$ROOT" AETHER_CACHE_DIR="$WORK/cache"
cd "$WORK"

fail() { echo "[FAIL] optional_library_linking: $*"; exit 1; }

# Capture actual C compiler argv, including ae run (which need not print it).
REAL_CC=${AE_CC:-${CC:-gcc}}
export REAL_CC
cat > cc-probe <<'EOF'
#!/bin/sh
for arg do
    printf '%s\n' "$arg" >> "$LINK_ARGS"
    case "$arg" in
        -lssl|-lcrypto|-lz|-lnghttp2|-lpcre2-8|-lbrotlienc|-lbrotlicommon|-lzstd|-lfyaml)
            case " $ALLOWED_LIBS " in
                *" $arg "*) ;;
                *) echo "unexpected optional dependency: $arg" >&2; exit 1 ;;
            esac ;;
    esac
done
# Allow conventional CC values with compiler flags, as ae itself does.
exec $REAL_CC "$@"
EOF
chmod +x cc-probe
WINDOWS=0
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;; # native driver cannot execute a shell wrapper
    *) export AE_CC="$WORK/cc-probe" ;;
esac
export LINK_ARGS="$WORK/link-args" ALLOWED_LIBS=""

check_args() {
    # Windows uses the native compiler; inspect its verbose command instead.
    if [ "$WINDOWS" = 1 ]; then tr ' ' '\n' < "$1" > "$LINK_ARGS"; fi
    [ -s "$LINK_ARGS" ] || fail 'C compiler command missing'
    for lib in -lssl -lcrypto -lz -lnghttp2 -lpcre2-8 -lbrotlienc -lbrotlicommon -lzstd -lfyaml; do
        if grep -Fxq -- "$lib" "$LINK_ARGS"; then
            case " $ALLOWED_LIBS " in
                *" $lib "*) ;;
                *) fail "unexpected optional dependency: $lib" ;;
            esac
        fi
    done
}

cat > hello.ae <<'EOF'
main() { println("hello") }
EOF
"$AE" build hello.ae -o hello --verbose > hello.log 2>&1 || { cat hello.log; fail 'hello build'; }
check_args hello.log
[ -f hello ] || fail 'hello binary missing'
[ "$(./hello)" = hello ] || fail 'hello output'
"$AE" run hello.ae --verbose > run.log 2>&1 || { cat run.log; fail 'hello run'; }
check_args run.log
grep -q '^hello$' run.log || fail 'run output'

# A library build also has no optional dependencies. Windows' native driver
# does not implement the POSIX shared-library path.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *)
        "$AE" build hello.ae --emit=lib -o libhello.so --verbose > lib.log 2>&1 \
            || { cat lib.log; fail 'hello library build'; }
        [ -f libhello.so ] || fail 'shared library missing' ;;
esac

# Each import must select only its own optional groups. Linking real modules
# catches lost dependencies as well as unexpected ones. Disabled backends may
# legitimately add no flags, so also assert the compiler's unconditional ABI
# metadata, independently of this toolchain's feature configuration.
for entry in \
    'regex|-lpcre2-8' \
    'zlib|-lz' \
    'brotli|-lbrotlienc -lbrotlicommon' \
    'zstd|-lzstd' \
    'yaml|-lfyaml' \
    'cryptography|-lssl -lcrypto' \
    'encoding|-lssl -lcrypto' \
    'http|-lssl -lcrypto -lnghttp2' \
    'http.client|-lssl -lcrypto' \
    'http.middleware|-lz -lssl -lcrypto -lnghttp2' \
    'http.proxy|-lssl -lcrypto -lnghttp2' \
    'http.script_gateway|-lssl -lcrypto -lnghttp2'
do
    module=${entry%%|*}
    ALLOWED_LIBS=${entry#*|}
    export ALLOWED_LIBS
    printf 'import std.%s\nmain() { println("linked") }\n' "$module" > uses.ae
    "$ROOT/build/aetherc" uses.ae uses.c > compile.log 2>&1 \
        || { cat compile.log; fail "metadata for $module"; }
    header=$(head -n 1 uses.c)
    for lib in $ALLOWED_LIBS; do
        case " $header " in
            *" $lib "*) ;;
            *) fail "$module metadata missing $lib: $header" ;;
        esac
    done
    : > "$LINK_ARGS"
    "$AE" build uses.ae -o "uses-$module" --verbose > build.log 2>&1 \
        || { cat build.log; fail "build std.$module"; }
    check_args build.log
    [ -f "uses-$module" ] || { cat build.log; fail "binary for std.$module missing"; }
    [ "$("./uses-$module")" = linked ] || fail "run std.$module"
    # No managed dependency is duplicated through the raw header.
    for lib in $ALLOWED_LIBS; do
        count=$(grep -Fxc -- "$lib" "$LINK_ARGS" || true)
        [ "$count" -le 1 ] || fail "$module repeats $lib"
    done
    "$AE" run uses.ae --verbose > module-run.log 2>&1 \
        || { cat module-run.log; fail "ae run std.$module"; }
    check_args module-run.log
    grep -q '^linked$' module-run.log || fail "ae run output for std.$module"
done

# Transitive and conditional imports: the caller never imports the backend
# directly; losing the when arm must remove the optional link group too.
mkdir helper
cat > helper/module.ae <<'EOF'
import std.brotli
exports(available)
available() -> int { return brotli.available() }
EOF
cat > conditional.ae <<'EOF'
when defined(WITH_CODEC) {
    import helper
}
main() {
    when defined(WITH_CODEC) { println(helper.available()) }
    else { println("excluded") }
}
EOF
ALLOWED_LIBS='-lbrotlienc -lbrotlicommon'
export ALLOWED_LIBS
: > "$LINK_ARGS"
"$AE" build conditional.ae -D WITH_CODEC -o conditional-on --verbose > conditional-on.log 2>&1 \
    || { cat conditional-on.log; fail 'transitive import'; }
check_args conditional-on.log
./conditional-on > conditional.out
grep -Eq '^[01]$' conditional.out || fail 'backend availability output'
ALLOWED_LIBS=''
export ALLOWED_LIBS
: > "$LINK_ARGS"
"$AE" build conditional.ae -o conditional-off --verbose > conditional-off.log 2>&1 \
    || { cat conditional-off.log; fail 'excluded import'; }
check_args conditional-off.log
[ "$(./conditional-off)" = excluded ] || fail 'excluded branch output'

echo '[PASS] optional_library_linking: build/run/lib, per-module groups, transitive and conditional imports'
