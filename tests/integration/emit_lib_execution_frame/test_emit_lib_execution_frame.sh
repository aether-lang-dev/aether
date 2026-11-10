#!/bin/sh
# #2955 (D3) and #2956 (D4): every export runs in an execution frame.
#
# A panic in an export a C host called printed "panic outside any
# try/catch or actor" and aborted the host. An export that called a host
# service, which set another caller and deadline and called a second export,
# saw the second one's caller and deadline from then on, and a sandbox level
# the second left pushed stayed in force. Now a panic ends the call with
# AETHER_PANICKED and its reason, the export returns its zero value, the
# library takes the next call, and the outer call's caller info, deadline
# and sandbox levels are back when the inner one returns, however it ends.
#
# 1. a C host with the library's C linked in (one runtime);
# 2. a C host loading the library built the default way (its own runtime);
# 3. the same with --shared-runtime (skipped where build/shared has none);
# 4. an Aether program importing the library in binary form: a failed call
#    is raised again as a panic with the same reason, a `try` catches it,
#    and std.host's last_outcome() says how the call ended.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/syslibs.sh"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
CC="${CC:-cc}"
export AETHER_HOME="$ROOT"
export AETHER_STACK_TRACE=0

[ -x "$AE" ] || { echo "  [SKIP] emit_lib_execution_frame: $AE not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
fail=0
want='boom 0 outcome 1 "boom 1"; plain 7 outcome 0; again 1 outcome 0
which 0: inner 7 outcome 0 ""; outer 7 outcome 0
which 1: inner -1 outcome 0 ""; outer 7 outcome 0
which 2: inner 0 outcome 1 "inner panicked"; outer 7 outcome 0
which 3: inner 0 outcome 2 "the call'"'"'s deadline passed"; outer 7 outcome 0'

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) ext=dll; exe=.exe; dl="" ;;
    Darwin) ext=dylib; exe=""; dl="" ;;
    Linux) ext=so; exe=""; dl="-ldl" ;;
    *) ext=so; exe=""; dl="" ;;
esac

# $1 = label, $2 = output
check() {
    if [ "$2" != "$want" ]; then
        echo "  [FAIL] emit_lib_execution_frame: $1:"
        echo "      got:"; printf '%s\n' "$2" | sed 's/^/        /'
        echo "      want:"; printf '%s\n' "$want" | sed 's/^/        /'
        fail=1
    fi
}

# 1. one runtime
if ! "$AETHERC" --emit=lib --with=extern "$SCRIPT_DIR/frames.ae" "$TMP/frames.c" > "$TMP/frames.log" 2>&1; then
    echo "  [FAIL] emit_lib_execution_frame: aetherc --emit=lib failed"
    tail -5 "$TMP/frames.log" | sed 's/^/        /'
    exit 1
fi
# shellcheck disable=SC2046
if ! "$CC" -O1 -DHOST_STATIC -I"$ROOT/include" -I"$ROOT/runtime" $("$AE" cflags --cflags) \
        "$TMP/frames.c" "$SCRIPT_DIR/hooks.c" "$SCRIPT_DIR/host.c" -o "$TMP/host_static" \
        $("$AE" cflags --libs) > "$TMP/cc.log" 2>&1; then
    echo "  [FAIL] emit_lib_execution_frame: the one-runtime host did not build"
    head -10 "$TMP/cc.log" | sed 's/^/        /'
    exit 1
fi
check "one runtime" "$("$TMP/host_static" 2>"$TMP/static.err" | tr -d '\r')"

if ! "$CC" -O1 "$SCRIPT_DIR/host.c" -o "$TMP/host_dyn" $dl > "$TMP/cc_dyn.log" 2>&1; then
    echo "  [FAIL] emit_lib_execution_frame: the loading host did not build"
    head -10 "$TMP/cc_dyn.log" | sed 's/^/        /'
    exit 1
fi

# $1 = label, $2 = extra build flags
build_lib() {
    mkdir -p "$TMP/$1"
    # shellcheck disable=SC2086
    (cd "$TMP/$1" && "$AE" build --emit=lib $2 --with=extern "$SCRIPT_DIR/frames.ae" \
        --extra "$SCRIPT_DIR/hooks.c" -o "$TMP/$1/libframes.$ext" > "$TMP/$1/build.log" 2>&1)
}

# $1 = label, $2 = library dir: the Aether program importing it in binary form
run_app() {
    cp "$SCRIPT_DIR/app.ae" "$2/app.ae"
    got="$(cd "$2" && "$AE" build app.ae -o app > app_build.log 2>&1 && \
           PATH="$2:$ROOT/build/shared:$PATH" \
           LD_LIBRARY_PATH="$2:$ROOT/build/shared${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
           DYLD_LIBRARY_PATH="$2:$ROOT/build/shared${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
           ./app$exe 2>"$2/app.err" | tr -d '\r')"
    app_want='caught: boom 1, outcome 1 "boom 1"
then: 1, outcome 0'
    if [ "$got" != "$app_want" ]; then
        echo "  [FAIL] emit_lib_execution_frame: $1, Aether host: got:"
        printf '%s\n' "$got" | sed 's/^/        /'
        tail -5 "$2/app_build.log" "$2/app.err" 2>/dev/null | sed 's/^/        /'
        fail=1
    fi
}

# 2. the library's own runtime
if ! build_lib static ""; then
    echo "  [FAIL] emit_lib_execution_frame: ae build --emit=lib failed"
    tail -5 "$TMP/static/build.log" | sed 's/^/        /'
    exit 1
fi
check "its own runtime" "$("$TMP/host_dyn" "$TMP/static/libframes.$ext" 2>"$TMP/dyn.err" | tr -d '\r')"
run_app "its own runtime" "$TMP/static"

# 3. one shared runtime
if ls "$ROOT/build/shared/" 2>/dev/null | grep -q "aether"; then
    if ! build_lib shared "--shared-runtime"; then
        echo "  [FAIL] emit_lib_execution_frame: ae build --emit=lib --shared-runtime failed"
        tail -5 "$TMP/shared/build.log" | sed 's/^/        /'
        exit 1
    fi
    rt="$(ls "$ROOT/build/shared/"aether.dll "$ROOT/build/shared/"libaether.dylib \
          "$ROOT/build/shared/"libaether.so 2>/dev/null | head -1)"
    check "--shared-runtime" "$(PATH="$ROOT/build/shared:$PATH" \
        LD_LIBRARY_PATH="$ROOT/build/shared${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        DYLD_LIBRARY_PATH="$ROOT/build/shared${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
        "$TMP/host_dyn" "$TMP/shared/libframes.$ext" "$rt" 2>"$TMP/shared.err" | tr -d '\r')"
    run_app "--shared-runtime" "$TMP/shared"
    shared_note=", and on the shared runtime"
else
    shared_note=" (no shared runtime in build/shared: that leg skipped)"
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] emit_lib_execution_frame: a panicking export returns AETHER_PANICKED to its C host and the next call works; the outer call's caller, deadline and sandbox levels are back after an inner call that returns, refuses or panics; an Aether importer catches the failure as a panic: one runtime, the library's own$shared_note"
fi
exit $fail
