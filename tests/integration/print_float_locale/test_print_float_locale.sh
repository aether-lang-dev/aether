#!/bin/sh
# #3525: print and println write a float with '.', whatever locale the
# process is in. print(x) emitted printf("%f"), and "${x}" went through plain
# snprintf, so in a host that had called setlocale(LC_ALL, "de_DE.UTF-8")
# they wrote 0,500000 and 0,5 where string.from_float wrote 0.5. Both now go
# through the runtime's locale independent formatter.
#
# The program sets the locale itself: the locale environment variables do
# nothing to a C program until something calls setlocale. It prints SKIP when
# no comma-decimal locale is installed.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/same_bytes.sh"
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
AE="$ROOT/build/ae$EXE"
if [ ! -x "$AE" ]; then
    echo "  [SKIP] print_float_locale: build/ae not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/print_float.ae" -o "$tmp/print_float" >"$tmp/build.log" 2>&1; then
    echo "  [FAIL] print_float_locale: the program did not build"
    sed 's/^/    /' "$tmp/build.log" | head -10
    exit 1
fi
"$tmp/print_float$EXE" > "$tmp/out.txt" 2>&1 || {
    echo "  [FAIL] print_float_locale: the program failed"
    sed 's/^/    /' "$tmp/out.txt" | head -10
    exit 1
}
tr -d '\r' < "$tmp/out.txt" > "$tmp/got.txt"
if [ "$(cat "$tmp/got.txt")" = "SKIP" ]; then
    echo "  [SKIP] print_float_locale: no comma-decimal locale installed"
    exit 0
fi

cat > "$tmp/want.txt" <<'EOF'
0.500000
0.250000
fmt 2.500000
pfmt 3.500000
0.5
-1.25
1.500000
EOF
if ! same_bytes "$tmp/want.txt" "$tmp/got.txt"; then
    echo "  [FAIL] print_float_locale: a float was printed by the process locale"
    echo "    want:"; sed 's/^/      /' "$tmp/want.txt"
    echo "    got:"; sed 's/^/      /' "$tmp/got.txt"
    exit 1
fi
echo "  [PASS] print_float_locale: print and println write floats with '.' under a comma locale"
