#!/bin/sh
# A compile step that is KILLED must say so.
#
# A compiler that rejects the program exits 1 and has already explained
# itself. A compiler the OS takes away explains nothing, and `ae` reported
# only "Build failed." / "Compilation failed." — which cannot be told apart
# from a rejected program. A sweep run hit exactly that: one test failed with
# `Compilation failed.` and nothing else, and there was no way to know
# whether the code was wrong or the machine was under memory pressure.
#
# `posix_run` already distinguishes the two (it returns -signal for a child
# that died of one); this asserts the driver surfaces it, and — just as
# importantly — that an ORDINARY compile error is not decorated with an exit
# status it does not need.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] killed_compiler_diagnostic: ae not built"; exit 0; }

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # The native driver cannot execute a shell wrapper as AE_CC, so the
        # kill half cannot be staged here. The ordinary-error half still can.
        WINDOWS=1 ;;
    *) WINDOWS=0 ;;
esac

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"

printf 'main() { println("ok") }\n' > "$tmp/good.ae"
printf 'main() { let x = }\n'       > "$tmp/bad.ae"

# 1. An ordinary compile error stays exactly as it was: the diagnostic, then
#    the summary line, with no exit status bolted onto it.
out="$("$AE" run "$tmp/bad.ae" 2>&1)"
if ! printf '%s\n' "$out" | grep -q '^Compilation failed\.$'; then
    echo "  [FAIL] killed_compiler_diagnostic: an ordinary compile error should not carry an exit status"
    printf '%s\n' "$out" | tail -3 | sed 's/^/        /'
    fail=1
fi

# 2. A compile step killed by a signal says which signal, and says why it
#    had nothing else to report.
killed="kill case: not stageable on this platform, skipped"
if [ "$WINDOWS" = 0 ]; then
    cat > "$tmp/suicidal-cc" <<'EOF'
#!/bin/sh
# Stand in for a C compiler the OS takes away mid-build.
kill -TERM $$
sleep 5
EOF
    chmod +x "$tmp/suicidal-cc"
    out="$(AE_CC="$tmp/suicidal-cc" "$AE" run "$tmp/good.ae" 2>&1)"
    if printf '%s\n' "$out" | grep -q 'Build failed\. (killed by signal 15'; then
        killed="a killed compile step names its signal"
    else
        echo "  [FAIL] killed_compiler_diagnostic: a killed compile step did not name its signal"
        printf '%s\n' "$out" | tail -3 | sed 's/^/        /'
        fail=1
    fi
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] killed_compiler_diagnostic: an ordinary compile error is undecorated; $killed"
fi
exit $fail
