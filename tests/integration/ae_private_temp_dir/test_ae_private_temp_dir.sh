#!/bin/sh
# ae writes the C it generates, and every other temp file it compiles, reads
# back or runs, into one private directory of its own (#3745), and an unset
# HOME no longer means /tmp.
#
# It used to write them to predictable names in the shared temp directory
# ($TMPDIR/_ae_<pid>.c, ae_build_<pid>_<name>.c, probe sources), then compile
# and run them. Another local user could make such a file first, or a symlink
# there, and change it between the write and the compile; the pid in the
# name stopped none of that. And with HOME unset, ae kept its cache, versions
# and packages under /tmp/.aether, which any local user can write.
#
# A C compiler wrapper (CC) records each argument ae hands it and, on POSIX,
# the mode of the directory each .c file is in, while the build runs.
# Asserted:
#   1. the generated C sits in a directory under the temp directory named
#      ae-..., with mode 0700 (drwx------) on POSIX,
#   2. nothing of ae's is left in the temp directory after it exits,
#   3. on POSIX, with HOME unset, the build cache is under the password
#      database's home for this user, not under /tmp.
# Windows has no mode bits, and its %TEMP% is per user already; there the
# location and the cleanup are checked.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

WIN=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) WIN=1 ;;
esac
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] ae_private_temp_dir: ae not built"; exit 0; }
REAL_CC="$(command -v cc 2>/dev/null || command -v gcc 2>/dev/null || command -v clang 2>/dev/null)"
[ -n "$REAL_CC" ] || { echo "  [SKIP] ae_private_temp_dir: no C compiler on PATH"; exit 0; }

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
fail() {
    echo "  [FAIL] ae_private_temp_dir: $1"
    [ -n "${2:-}" ] && [ -f "$2" ] && sed 's/^/        /' "$2"
    exit 1
}

# The path a native ae.exe reads and writes; the MSYS spelling elsewhere.
native_path() {
    if [ "$WIN" = 1 ]; then cygpath -m "$1"; else printf '%s' "$1"; fi
}

SHARED="$TMP/shared"
LOG="$TMP/cc.log"
mkdir -p "$SHARED" "$TMP/home" "$TMP/proj"
: > "$LOG"
SHARED_N="$(native_path "$SHARED")"
# ae.exe starts programs natively on Windows, where a shell script is not a
# program, so the wrapper is a .cmd file there.
if [ "$WIN" = 1 ]; then
    printf '@echo %%*>> "%s"\r\n@"%s" %%*\r\n' "$(cygpath -w "$LOG")" "$(cygpath -w "$REAL_CC")" > "$TMP/cc.cmd"
    CC_FILE="$(native_path "$TMP/cc.cmd")"
else
    cat > "$TMP/cc" <<EOF
#!/bin/sh
for a in "\$@"; do
    case "\$a" in
        *.c) printf '%s %s\n' "\$(ls -ld "\$(dirname "\$a")" | cut -c1-10)" "\$a" >> "$LOG" ;;
    esac
done
exec "$REAL_CC" "\$@"
EOF
    chmod +x "$TMP/cc"
    CC_FILE="$TMP/cc"
fi

cat > "$TMP/proj/hello.ae" <<'EOF'
main() {
    println("hello")
}
EOF

# 1 + 2 ----------------------------------------------------------------------
( cd "$TMP/proj" && env -u AE_CC -u AE_KEEP_C CC="$CC_FILE" TMPDIR="$SHARED_N" TEMP="$SHARED_N" \
    TMP="$SHARED_N" HOME="$TMP/home" USERPROFILE="$(native_path "$TMP/home")" \
    AETHER_CACHE_DIR="$(native_path "$TMP/cache")" "$AE" build hello.ae -o hello ) >"$TMP/build.log" 2>&1 \
    || fail "ae build failed" "$TMP/build.log"
[ "$("$TMP/proj/hello" | tr -d '\r')" = hello ] || fail "the program did not run" "$TMP/build.log"
grep -q "$SHARED_N/ae-[^/ ]*/build_hello\.c" "$LOG" \
    || fail "the generated C was not in a private directory under the temp directory" "$LOG"
if [ "$WIN" = 0 ]; then
    grep "$SHARED/" "$LOG" > "$TMP/in_tmp.log"
    if grep -v "^drwx------ $SHARED/ae-[^/]*/[^/]*\.c$" "$TMP/in_tmp.log" > "$TMP/bad.log"; then
        fail "a C file ae compiled from the temp directory was outside a 0700 private directory" "$TMP/bad.log"
    fi
fi
if [ -n "$(ls -A "$SHARED")" ]; then
    ls -la "$SHARED" > "$TMP/left.log"
    fail "ae left files in the temp directory" "$TMP/left.log"
fi

# 3 ----------------------------------------------------------------------------
if [ "$WIN" = 0 ] && command -v python3 >/dev/null 2>&1; then
    PW_HOME="$(python3 -c 'import os, pwd; print(pwd.getpwuid(os.getuid()).pw_dir)' 2>/dev/null)"
    if [ -n "$PW_HOME" ]; then
        env -u HOME -u AETHER_CACHE_DIR "$AE" cache info >"$TMP/nohome.log" 2>&1 \
            || fail "ae cache info failed with HOME unset" "$TMP/nohome.log"
        grep -q "^Location: /tmp/" "$TMP/nohome.log" \
            && fail "with HOME unset the cache is under /tmp" "$TMP/nohome.log"
        grep -qF "Location: $PW_HOME/.aether/cache" "$TMP/nohome.log" \
            || fail "with HOME unset the cache is not under the password database's home ($PW_HOME)" "$TMP/nohome.log"
    fi
fi

echo "  [PASS] ae_private_temp_dir"
