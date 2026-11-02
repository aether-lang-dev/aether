# Sourced by the test_stunt_*.sh beside it. Builds stunt (libstunt and the
# stunt program), the contrib.host.stunt bridge and host.ae, then gives the
# test two calls:
#
#   policy <grants>                 the grants the following checks run under
#   stunt <check> <expectation>     run one check under them, in every mode
#                                   that applies here; stop at the first
#                                   mismatch
#
# The modes, as host.ae runs them:
#   inproc    libstunt loaded by the bridge, under libaether_sandbox.so where
#             the preload works (Linux, FreeBSD)
#   child     the stunt program under sandbox.spawn_socket (Linux)
#   uncont    the same under sandbox.spawn_socket_uncontained, where the
#             contained spawn refuses (macOS, FreeBSD): only the checks that
#             are about the host, not the child's containment
#
# Windows has the in-process mode alone, without the libc rows (POSIX calls)
# or the fs ones (POSIX paths): no socket channel exists there.
#
# What applies is chosen by check name: the catalogue's modes column
# (`stunt list`) and, for inproc, whether the preload holds this platform's
# libc calls. Paths in a policy may use $WORK, the directory the checks run
# in, which holds stunt-fixture.txt.

STUNT_SRC="$ROOT/contrib/host/stunt"
AE="$ROOT/build/ae"
NAME="${STUNT_TEST:-contrib_host_stunt}"

skip_all() {
    echo "  [SKIP] $NAME: $1"
    exit 0
}

die() {
    echo "  [FAIL] $NAME: $1"
    [ -n "${2:-}" ] && [ -f "$2" ] && sed 's/^/    /' "$2" | tail -30
    exit 1
}

case "$(uname -s 2>/dev/null)" in
    Linux) PLAT=linux ;;
    FreeBSD) PLAT=freebsd ;;
    Darwin) PLAT=macos ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) PLAT=windows ;;
    *) PLAT=other ;;
esac
[ -x "$AE" ] || skip_all "ae not built"

TMP="$(mktemp -d)"
TMP="$(cd "$TMP" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
BIN="$TMP/bin"
WORK="$TMP/work"
mkdir -p "$BIN" "$WORK"
echo "stunt fixture" > "$WORK/stunt-fixture.txt"
export AETHER_HOME="$ROOT"
export HOME="${HOME:-$TMP}"

CC="${CC:-cc}"
case "$PLAT" in
    macos) SHARED="-dynamiclib -fPIC"; LIBNAME="libstunt.dylib"; DL="" ;;
    linux) SHARED="-shared -fPIC"; LIBNAME="libstunt.so"; DL="-ldl" ;;
    windows) SHARED="-shared"; LIBNAME="stunt.dll"; DL="" ;;
    *) SHARED="-shared -fPIC"; LIBNAME="libstunt.so"; DL="" ;;
esac
$CC -std=c11 -O1 -Wall -Wextra -Werror $SHARED -o "$BIN/$LIBNAME" "$STUNT_SRC/stunt.c" $DL \
    > "$TMP/cc.log" 2>&1 || die "libstunt did not build" "$TMP/cc.log"
$CC -std=c11 -O1 -Wall -Wextra -Werror -o "$BIN/stunt" "$STUNT_SRC/stunt.c" "$STUNT_SRC/stunt_main.c" $DL \
    > "$TMP/cc.log" 2>&1 || die "the stunt program did not build" "$TMP/cc.log"
export AETHER_STUNT_LIB="$BIN/$LIBNAME"
# LoadLibrary takes a Windows path, not MSYS2's /tmp/...
[ "$PLAT" = windows ] && command -v cygpath > /dev/null 2>&1 && AETHER_STUNT_LIB="$(cygpath -m "$AETHER_STUNT_LIB")"
export STUNT_EXE="$BIN/stunt"

BRIDGE="$ROOT/build/contrib/libaether_host_stunt.a"
if [ ! -f "$BRIDGE" ] || [ "$STUNT_SRC/aether_host_stunt.c" -nt "$BRIDGE" ] ||
   [ "$STUNT_SRC/stunt.h" -nt "$BRIDGE" ]; then
    (cd "$ROOT" && MODULES=stunt bash tests/scripts/contrib_build.sh) > "$TMP/bridge.log" 2>&1 ||
        die "the contrib.host.stunt bridge did not build" "$TMP/bridge.log"
fi
"$AE" build "$SCRIPT_DIR/host.ae" -o "$BIN/host" > "$TMP/host.log" 2>&1 ||
    die "host.ae did not build" "$TMP/host.log"
"$BIN/stunt" list > "$TMP/catalogue.txt" || die "stunt list failed" "$TMP/catalogue.txt"

# In-process, the libc checks are held by the preload; where it does not
# exist they would only show that nothing holds them.
PRELOAD="$ROOT/build/libaether_sandbox.so"
INPROC_LIBC=0
if { [ "$PLAT" = linux ] || [ "$PLAT" = freebsd ]; } && [ -f "$PRELOAD" ]; then
    INPROC_LIBC=1
fi

# The contained child: Linux with Landlock (spawn_socket refuses without it).
CHILD=0
UNCONT=0
RAW=0
if [ "$PLAT" = linux ] && [ -f "$PRELOAD" ]; then
    (cd "$WORK" && "$BIN/host" child - callback-greet succeed) > "$TMP/probe.out" 2>&1
    if grep -q "has no Landlock" "$TMP/probe.out"; then
        echo "  [SKIP] $NAME: this kernel has no Landlock, so spawn_socket starts nothing: the child mode is left out"
    elif grep -q "callback-greet: succeeded" "$TMP/probe.out"; then
        CHILD=1
    else
        die "the contained child did not start" "$TMP/probe.out"
    fi
    case "$(uname -m)" in x86_64|aarch64) RAW=1 ;; esac
elif [ "$PLAT" = macos ] || [ "$PLAT" = freebsd ]; then
    UNCONT=1
fi

POLICY="-"
policy() { POLICY="$1"; }

modes_of() {
    awk -v c="$1" '$1 == c { print $2 }' "$TMP/catalogue.txt"
}

applies() {   # applies <mode> <check>
    modes="$(modes_of "$2")"
    [ -n "$modes" ] || die "'$2' is not in stunt's catalogue"
    case "$1" in
        inproc)
            case "$modes" in *in-process*) ;; *) return 1 ;; esac
            case "$2" in
                open-*|write-*|getenv-home|connect-*|exec-bin-sh) [ "$INPROC_LIBC" = 1 ] ;;
                # Windows: no libc rows (they are POSIX calls), and fs grants
                # name POSIX paths.
                dlopen-libm|binding-read-*) [ "$PLAT" != windows ] ;;
                *) return 0 ;;
            esac ;;
        child)
            [ "$CHILD" = 1 ] || return 1
            case "$2" in
                syscall-openat|syscall-socket) [ "$RAW" = 1 ] ;;
                *) return 0 ;;
            esac ;;
        uncont)
            [ "$UNCONT" = 1 ] || return 1
            case "$modes" in *child*) ;; *) return 1 ;; esac
            case "$2" in
                callback-*|map-*|binding-*|env-cached|spin|silent|oversize|half-frame) return 0 ;;
                *) return 1 ;;
            esac ;;
    esac
}

RAN=0
stunt() {
    check="$1"
    expect="$2"
    for mode in inproc child uncont; do
        applies "$mode" "$check" || continue
        # "<outer>|<inner>" nests the bridge's level in an enforce block:
        # an in-process shape.
        case "$POLICY" in *"|"*) [ "$mode" = inproc ] || continue ;; esac
        out="$TMP/out.txt"
        case "$mode" in
            inproc)
                if [ "$INPROC_LIBC" = 1 ]; then
                    (cd "$WORK" && LD_PRELOAD="$PRELOAD" "$BIN/host" inproc "$POLICY" "$check" "$expect") > "$out" 2>&1
                else
                    (cd "$WORK" && "$BIN/host" inproc "$POLICY" "$check" "$expect") > "$out" 2>&1
                fi ;;
            child) (cd "$WORK" && "$BIN/host" child "$POLICY" "$check" "$expect") > "$out" 2>&1 ;;
            uncont) (cd "$WORK" && "$BIN/host" child-uncontained "$POLICY" "$check" "$expect") > "$out" 2>&1 ;;
        esac
        rc=$?
        if [ "$rc" -ne 0 ]; then
            echo "  [FAIL] $NAME ($mode, policy $POLICY): exit $rc"
            sed 's/^/    /' "$out"
            exit 1
        fi
        printf '  [PASS] %-6s %s\n' "$mode" "$(tail -1 "$out")"
        RAN=$((RAN + 1))
    done
}

done_checks() {
    if [ "$RAN" -eq 0 ]; then
        echo "  [SKIP] $NAME: none of its checks applies on $PLAT"
    else
        echo "  [PASS] $NAME: $RAN runs"
    fi
}
