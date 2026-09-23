#!/bin/sh
# Regression (#2171): on Windows, std.os's argv-based spawn resolves the
# program the way POSIX execvp does, and a batch file cannot be made to run
# a second command through its arguments.
#
# Every launch passed lpApplicationName = NULL, so CreateProcessW found the
# program itself: the application's directory and the CURRENT directory
# came before PATH, so a .\tool.exe planted where the program ran won over
# the real one on PATH. And a .bat/.cmd ran under cmd.exe, which re-reads
# the command line by its own rules, so an argument like
# `x" & echo INJECTED & "` ran the second command (BatBadBut,
# CVE-2024-24576).
#
# POSIX has neither behaviour (execvp searches PATH only, and there is no
# batch interpreter re-parsing argv), so this runs on Windows only.
set -eu

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) echo "  [SKIP] win_spawn_resolution: Windows-only (POSIX spawn has neither behaviour)"; exit 0 ;;
esac

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
AE="$ROOT/build/ae"
if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] win_spawn_resolution: build/ae not built"
    exit 0
fi
AETHER_HOME=""
export AETHER_HOME

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/bin" "$W/cwd" "$W/launch"

fail() {
    echo "  [FAIL] win_spawn_resolution: $1"
    exit 1
}

build() {   # build <src> <out-exe>
    "$AE" build "$1" -o "$2" > "$W/build.log" 2>&1 \
        || { sed 's/^/    /' "$W/build.log" | head -20; fail "could not build $(basename "$1")"; }
}

# The real tool (on PATH) and a planted one (in the working directory).
printf 'main() {\n    println("real")\n}\n' > "$W/real.ae"
printf 'main() {\n    println("planted")\n}\n' > "$W/planted.ae"
build "$W/real.ae" "$W/bin/aetool"
build "$W/planted.ae" "$W/cwd/aetool"
[ -f "$W/bin/aetool.exe" ] && [ -f "$W/cwd/aetool.exe" ] || fail "helper executables missing"

# A batch file that echoes its first two arguments exactly as they arrive.
# (%1, not %~1: stripping the quotes and echoing unquoted would have the
# SCRIPT run `& b` itself, which is the script's own bug, not the launcher's.)
printf '@echo off\r\necho [%%1] [%%2]\r\n' > "$W/bin/echoargs.bat"
# Forward slashes: the path is embedded in an Aether string literal, where a
# backslash starts an escape. Windows accepts either separator.
BAT="$(cygpath -m "$W/bin/echoargs.bat")"

cat > "$W/launcher.ae" <<AE
import std.os
import std.list
import std.string

show(label: string, prog: string, a: string, b: string) {
    argv = list.new()
    if string.length(a) > 0 { _x = list.add(argv, a) }
    if string.length(b) > 0 { _y = list.add(argv, b) }
    out, code, err = os.run_capture(prog, argv, null)
    println("\${label}|\${string.trim(out)}|\${code}|\${err}")
    list.free(argv)
}

main() {
    // 1. bare name: PATH's aetool, not the working directory's
    show("bare", "aetool", "", "")

    // 2. the same through the non-blocking path
    no_args = list.new()
    tok, serr = os.spawn_proc("aetool", no_args, null)
    if serr != "" { println("spawn|\${serr}") } else {
        st, _w = os.wait(tok)
        println("spawn-status|\${st}")
    }
    list.free(no_args)

    // 3. a batch file found by bare name through PATHEXT, with an
    //    argument cmd would otherwise act on
    show("bat-amp", "echoargs", "a & b", "plain")

    // 4. an argument that closes cmd's quoting: refused, nothing runs
    show("bat-quote", "$BAT", "x\" & echo INJECTED & \"", "")

    // 5. an argument cmd would expand: refused
    show("bat-percent", "$BAT", "%PATH%", "")

    // 6. a program that exists nowhere on PATH
    show("missing", "no-such-program-2171", "", "")
}
AE
build "$W/launcher.ae" "$W/launch/launcher"

# NoDefaultCurrentDirectoryInExePath makes CreateProcessW skip the current
# directory. MSYS2 shells set it; a program started from cmd.exe, PowerShell
# or Explorer does not, so it is cleared here: with it set, the planted
# executable could never be found and this would prove nothing.
out="$(cd "$W/cwd" && env -u NoDefaultCurrentDirectoryInExePath PATH="$W/bin:$PATH" "$W/launch/launcher.exe" 2>&1)" \
    || { echo "$out" | sed 's/^/    /'; fail "the launcher failed"; }
echo "$out" | tr -d '\r' > "$W/out.txt"

line() { grep "^$1|" "$W/out.txt" || true; }

[ "$(line bare)" = "bare|real|0|" ] \
    || { sed 's/^/    /' "$W/out.txt"; fail "a bare name ran the working directory's copy, not PATH's: '$(line bare)'"; }

# spawn_proc's child writes to our stdout: "real" must appear, "planted" not.
grep -qx "real" "$W/out.txt" || { sed 's/^/    /' "$W/out.txt"; fail "spawn_proc did not run PATH's aetool"; }
grep -q "planted" "$W/out.txt" && { sed 's/^/    /' "$W/out.txt"; fail "a planted executable in the working directory ran"; }
[ "$(line spawn-status)" = "spawn-status|0" ] || { sed 's/^/    /' "$W/out.txt"; fail "spawn_proc: '$(line spawn-status)'"; }

[ "$(line bat-amp)" = "bat-amp|[\"a & b\"] [plain]|0|" ] \
    || { sed 's/^/    /' "$W/out.txt"; fail "batch argument with & was not passed intact: '$(line bat-amp)'"; }

grep -q "INJECTED" "$W/out.txt" && { sed 's/^/    /' "$W/out.txt"; fail "an argument ran a second command through the batch file"; }
case "$(line bat-quote)" in
    "bat-quote||-1|argument cannot be passed to a batch file safely"*) ;;
    *) sed 's/^/    /' "$W/out.txt"; fail "a quote-carrying batch argument was not refused: '$(line bat-quote)'" ;;
esac
case "$(line bat-percent)" in
    "bat-percent||-1|argument cannot be passed to a batch file safely"*) ;;
    *) sed 's/^/    /' "$W/out.txt"; fail "a %-carrying batch argument was not refused: '$(line bat-percent)'" ;;
esac

[ "$(line missing)" = "missing||-1|program not found" ] \
    || { sed 's/^/    /' "$W/out.txt"; fail "a missing program: '$(line missing)'"; }

echo "  [PASS] win_spawn_resolution: PATH-only resolution, batch arguments quoted or refused"
