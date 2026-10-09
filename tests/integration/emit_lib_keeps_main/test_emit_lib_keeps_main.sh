#!/bin/sh
# --emit=lib keeps a program's main() callable (aether-ui
# asks/aether-emit-lib-keeps-main.md): a library built from a .ae that defines
# main() exports
#
#   int  aether_main(int argc, char** argv)   the executable's prologue + main()'s
#                                             body; returns main()'s result and
#                                             leaves the actors running
#   void aether_main_exit(void)               the executable's epilogue: drain and
#                                             join the scheduler
#
# Before, the library dropped main()'s body and exported neither, so a host
# (an Android app's onCreate) had nothing to call.
#
# Checks:
#   1. both symbols are exported by the native library
#   2. a C host loads it (dlopen, LoadLibrary on Windows): aether_main runs
#      main() (args seen, an ask/reply
#      answered, 42 returned) and returns while the slow workers are still
#      busy; a second aether_main is rejected (-1); aether_main_exit drains the
#      workers before it returns, and is idempotent; aether_main runs again
#      after it, on a fresh scheduler
#   3. the executable built from the same source prints the same lines in the
#      same order and exits 42
#   4. a library without main() exports no aether_main
#   5. a top-level `main_exit`, whose export would be aether_main_exit, is a
#      compile error when the program defines main(), and fine when it doesn't
#   6. macOS: leaks(1) finds nothing after one run, and no scheduler table from
#      the first of two runs (a restarted scheduler frees its old tables)
#   7. cross: --target=aarch64-linux (zig) exports both from the ELF .so; with
#      an Android AETHER_SYSROOT, so does --target=aarch64-linux-android
#   8. --emit=csrc's header declares both for a program with main(), and
#      neither for one without
#   9. the --emit=obj object carries the executable's entry as a WEAK main(),
#      so a plain `cc app.o $(ae cflags --libs)` -- or any build tool (aeb's
#      c.program; aeb asks/c-program-aether-source-main-entry.md) -- links a
#      working program with no C main() of its own, exit code and all; and a
#      host that brings its own main() still gets its own (strong beats weak);
#      a shared library ae links (native and cross) exports no main at all.
#      On Windows the object carries no main() and the entry is
#      libaether_main.a, which `ae cflags --libs` names (a COFF weak main left
#      `main` to archive search, and libmingw32's WinMain entry won the link:
#      "undefined reference to `WinMain'"); 9c checks that layout there.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) LIB_EXT=".dll" ;;
    Darwin) LIB_EXT=".dylib" ;;
    *)      LIB_EXT=".so" ;;
esac
CC="${CC:-cc}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM
pass=0; fail=0
ok()  { echo "  [PASS] $1"; pass=$((pass + 1)); }
bad() { echo "  [FAIL] $1"; fail=$((fail + 1)); }

# Dynamic symbols, portably: a DLL's export table (objdump -p), GNU nm -D,
# else macOS nm -gU (leading '_').
syms() {
    case "$1" in
        *.dll)
            objdump -p "$1" 2>/dev/null | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p'
            return ;;
    esac
    s="$(nm -D --defined-only "$1" 2>/dev/null || true)"
    [ -n "$s" ] || s="$(nm -gU "$1" 2>/dev/null || true)"
    printf '%s\n' "$s"
}
exports() {  # exports <lib> <symbol>
    syms "$1" | grep -E "[[:space:]]_?$2\$" >/dev/null
}

cd "$SCRIPT_DIR"
LIB="$TMP/libapp$LIB_EXT"
# app.ae reads argv through its own extern (aether_args_count), which a
# capability-empty library may not declare: --with=extern opts in.
if ! AETHER_HOME="" "$AE" build --emit=lib --with=extern app.ae -o "$LIB" >"$TMP/build.log" 2>&1; then
    bad "ae build --emit=lib app.ae"; cat "$TMP/build.log"
    echo "emit_lib_keeps_main: $pass passed, $fail failed"; exit 1
fi

# 1. exports
if exports "$LIB" aether_main && exports "$LIB" aether_main_exit; then
    ok "library exports aether_main and aether_main_exit"
else
    bad "library does not export aether_main / aether_main_exit"
    syms "$LIB" | grep aether_ | head
fi

# 1b. ...and no entry point: the weak main() a library build of a program
#     carries is for an object linked into an executable (check 9); a shared
#     library ae links leaves it out.
if syms "$LIB" | grep -E "[[:space:]]_?main\$" >/dev/null; then
    bad "the shared library exports main"; syms "$LIB" | grep -E "[[:space:]]_?main\$"
else
    ok "the shared library exports no main"
fi

# 2. the C host
LDL=""; [ "$(uname -s)" = Linux ] && LDL="-ldl"
if ! $CC host.c -o "$TMP/host" $LDL >"$TMP/cc.log" 2>&1; then
    bad "cc host.c"; cat "$TMP/cc.log"
else
    "$TMP/host" "$LIB" >"$TMP/host.out" 2>&1; rc=$?
    expected='main: start, 3 args
main: 7 squared is 49
main: returning
host: aether_main returned 42
aether_main: already running; call aether_main_exit() first
host: second aether_main returned -1
worker done 1
worker done 2
host: aether_main_exit returned
host: second aether_main_exit returned
main: start, 1 args
main: 7 squared is 49
main: returning
host: rerun returned 42
worker done 1
worker done 2
host: done'
    # The message-pool report, when the runtime has one, is the epilogue's
    # too; it is not what this checks.
    got="$(grep -v -e '^$' -e 'Pool' -e 'Too large' -e 'Hit rate' -e '===' "$TMP/host.out")"
    if [ "$rc" -eq 0 ] && [ "$got" = "$expected" ]; then
        ok "host: aether_main runs main() and returns 42 with the workers still busy; aether_main_exit drains them; rerun works"
    else
        bad "host run (rc=$rc)"
        printf '%s\n' "--- expected" "$expected" "--- got" "$got"
    fi

    # 3. the executable is the same program
    if AETHER_HOME="" "$AE" build app.ae -o "$TMP/app" >"$TMP/exe.log" 2>&1; then
        "$TMP/app" one two >"$TMP/exe.out" 2>&1; erc=$?
        exe_lines="$(grep -e '^main:' -e '^worker' "$TMP/exe.out")"
        lib_lines="$(sed -n '1,/^host: aether_main_exit returned/p' "$TMP/host.out" | grep -e '^main:' -e '^worker')"
        if [ "$erc" -eq 42 ] && [ "$exe_lines" = "$lib_lines" ]; then
            ok "executable prints the same lines in the same order and exits 42"
        else
            bad "executable differs (exit $erc)"
            printf '%s\n' "--- exe" "$exe_lines" "--- lib" "$lib_lines"
        fi
    else
        bad "ae build app.ae (exe)"; cat "$TMP/exe.log"
    fi

    # 6. leaks (macOS)
    if [ "$(uname -s)" = Darwin ] && command -v leaks >/dev/null 2>&1; then
        MallocStackLogging=1 leaks --atExit -- "$TMP/host" "$LIB" once >"$TMP/leaks1.out" 2>&1
        if grep -q "^host: second aether_main_exit returned" "$TMP/leaks1.out" \
           && grep -q "0 leaks for 0 total leaked bytes" "$TMP/leaks1.out"; then
            ok "leaks: one aether_main / aether_main_exit run leaks nothing"
        else
            bad "leaks after one run"; grep -E "leaks for|ROOT LEAK" "$TMP/leaks1.out" | head
        fi
        MallocStackLogging=1 leaks --atExit -- "$TMP/host" "$LIB" >"$TMP/leaks2.out" 2>&1
        if grep -q "^host: done" "$TMP/leaks2.out" && grep -q "leaks for" "$TMP/leaks2.out" \
           && ! grep -q "in scheduler_init" "$TMP/leaks2.out"; then
            ok "leaks: a rerun frees the first run's scheduler tables"
        else
            bad "leaks after a rerun"; grep -E "leaks for|ROOT LEAK" "$TMP/leaks2.out" | head
        fi
    fi
fi

# 4. no main(), no entry point
if AETHER_HOME="" "$AE" build --emit=lib nomain.ae -o "$TMP/libnomain$LIB_EXT" >"$TMP/nomain.log" 2>&1; then
    if exports "$TMP/libnomain$LIB_EXT" aether_add && ! exports "$TMP/libnomain$LIB_EXT" aether_main \
       && ! exports "$TMP/libnomain$LIB_EXT" aether_main_exit; then
        ok "a library without main() exports no aether_main"
    else
        bad "library without main(): unexpected exports"; syms "$TMP/libnomain$LIB_EXT" | grep aether_ | head
    fi
else
    bad "ae build --emit=lib nomain.ae"; cat "$TMP/nomain.log"
fi

# 5. the reserved names
if AETHER_HOME="" "$AETHERC" --emit=lib collide.ae "$TMP/collide.c" >"$TMP/collide.log" 2>&1; then
    bad "--emit=lib accepted a top-level main_exit beside main()"
elif grep -q "would be exported as 'aether_main_exit'" "$TMP/collide.log"; then
    ok "main_exit beside main() is a compile error under --emit=lib"
else
    bad "collide.ae failed without the reserved-name error"; cat "$TMP/collide.log"
fi
if AETHER_HOME="" "$AETHERC" --emit=exe collide.ae "$TMP/collide_exe.c" >"$TMP/collide_exe.log" 2>&1 \
   && AETHER_HOME="" "$AETHERC" --emit=lib collide_ok.ae "$TMP/collide_ok.c" >"$TMP/collide_ok.log" 2>&1; then
    ok "main_exit is fine in an executable, and in a library without main()"
else
    bad "main_exit rejected outside the reserved case"; cat "$TMP/collide_exe.log" "$TMP/collide_ok.log"
fi

# 8. the --emit=csrc header (checked before the slow cross builds)
mkdir -p "$TMP/csrc"
if AETHER_HOME="" "$AE" build --emit=csrc --with=extern app.ae -o "$TMP/csrc/app" >"$TMP/csrc.log" 2>&1 \
   && AETHER_HOME="" "$AE" build --emit=csrc nomain.ae -o "$TMP/csrc/nomain" >>"$TMP/csrc.log" 2>&1; then
    if grep -q '^int aether_main(int argc, char\*\* argv);' "$TMP/csrc/app.h" \
       && grep -q '^void aether_main_exit(void);' "$TMP/csrc/app.h" \
       && ! grep -q 'aether_main' "$TMP/csrc/nomain.h"; then
        ok "--emit=csrc header declares aether_main / aether_main_exit only when there is a main()"
    else
        bad "--emit=csrc header prototypes"; grep -n aether_main "$TMP/csrc/app.h" "$TMP/csrc/nomain.h"
    fi
else
    bad "ae build --emit=csrc"; cat "$TMP/csrc.log"
fi

# 9. the weak main(): a linked object needs no hand-written C entry
if AETHER_HOME="" "$AE" build --emit=obj --with=extern app.ae -o "$TMP/app.o" >"$TMP/obj.log" 2>&1; then
    LIBS="$("$AE" cflags --libs 2>/dev/null)"
    if $CC "$TMP/app.o" $LIBS -o "$TMP/objprog" >"$TMP/objlink.log" 2>&1; then
        "$TMP/objprog" one two >"$TMP/objprog.out" 2>&1; orc=$?
        if [ "$orc" -eq 42 ] && grep -q '^main: start, 3 args' "$TMP/objprog.out" \
           && grep -q '^worker done 2' "$TMP/objprog.out"; then
            ok "an --emit=obj object links into a working program with no C main() (exit 42, workers drained)"
        else
            bad "object-only program (exit $orc)"; cat "$TMP/objprog.out"
        fi
    else
        bad "cc app.o with no C main(): link failed"; tail -5 "$TMP/objlink.log"
    fi
    cat > "$TMP/hostmain.c" <<'HC'
#include <stdio.h>
int  aether_main(int argc, char** argv);
void aether_main_exit(void);
int main(int argc, char** argv) {
    printf("hostmain: first\n");
    int rc = aether_main(argc, argv);
    aether_main_exit();
    printf("hostmain: last, rc %d\n", rc);
    return 0;
}
HC
    if $CC "$TMP/app.o" "$TMP/hostmain.c" $LIBS -o "$TMP/hostprog" >"$TMP/hostlink.log" 2>&1; then
        "$TMP/hostprog" >"$TMP/hostprog.out" 2>&1; hrc=$?
        if [ "$hrc" -eq 0 ] && [ "$(head -1 "$TMP/hostprog.out")" = "hostmain: first" ] \
           && grep -q '^hostmain: last, rc 42' "$TMP/hostprog.out"; then
            ok "a host's own main() wins over the object's entry"
        else
            bad "host main() did not win (exit $hrc)"; cat "$TMP/hostprog.out"
        fi
    else
        bad "cc app.o hostmain.c: link failed (duplicate main?)"; tail -5 "$TMP/hostlink.log"
    fi
    # 9c. Windows: the object leaves main() to libaether_main.a, named by
    #     `ae cflags --libs`, so no linker's handling of COFF weak externals
    #     decides which entry a program gets.
    case "$(uname -s 2>/dev/null)" in
        MINGW*|MSYS*|CYGWIN*|Windows_NT)
            if nm "$TMP/app.o" 2>/dev/null | grep -E "[[:space:]][TtWw][[:space:]]+main\$" >/dev/null; then
                bad "windows: the object defines main() (it belongs to libaether_main.a)"
            elif ! printf '%s\n' "$LIBS" | grep -q -- "-laether_main -laether"; then
                bad "windows: ae cflags --libs does not name -laether_main before -laether: $LIBS"
            else
                ok "windows: the object leaves main() to libaether_main.a, named by ae cflags --libs"
            fi ;;
    esac
else
    bad "ae build --emit=obj --with=extern app.ae"; cat "$TMP/obj.log"
fi

# 7. cross
if command -v zig >/dev/null 2>&1; then
    if AETHER_HOME="" "$AE" build --target=aarch64-linux --emit=lib --with=extern app.ae -o "$TMP/libapp_arm.so" >"$TMP/x.log" 2>&1 \
       && exports "$TMP/libapp_arm.so" aether_main && exports "$TMP/libapp_arm.so" aether_main_exit \
       && ! exports "$TMP/libapp_arm.so" main; then
        ok "cross aarch64-linux: the ELF .so exports both, and no main"
    else
        bad "cross aarch64-linux"; tail -15 "$TMP/x.log"
    fi
    if [ -n "$AETHER_SYSROOT" ] && ls "$AETHER_SYSROOT"/usr/lib/aarch64-linux-android/*/libc.so >/dev/null 2>&1; then
        if AETHER_HOME="" "$AE" build --target=aarch64-linux-android --emit=lib app.ae -o "$TMP/libapp_android.so" >"$TMP/xa.log" 2>&1 \
           && exports "$TMP/libapp_android.so" aether_main && exports "$TMP/libapp_android.so" aether_main_exit; then
            ok "cross aarch64-linux-android: the .so exports both"
        else
            bad "cross aarch64-linux-android"; tail -15 "$TMP/xa.log"
        fi
    fi
fi

echo ""
echo "emit_lib_keeps_main: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
