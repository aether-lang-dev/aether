#!/bin/sh
# Differential testing across lowering paths (#523).
#
# Per-path correctness is weaker than cross-path agreement. Each path today is
# checked against its own expected output, so a bug that miscompiles one path
# passes as long as that path's expectations were generated from the same buggy
# build. This runs every case through BOTH lowering paths and compares, which
# catches the divergence class directly.
#
# Paths compared:
#   --emit=exe   the case's own main() runs run()
#   --emit=lib   driver.c dlopens the artifact and calls aether_run
#
# A divergence is a hard failure naming both paths and showing the diff.
# Carved-out cases are reported with their reason, never silently skipped.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] differential: ae not built"
    exit 0
fi

# The lib half needs dlopen. MSYS2/MinGW has no libdl and the runtime's -ldl
# dependency does not exist there, the same reason the C-interop link cases
# skip case 4 on Windows. Reported, not silent.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] differential: no dlopen on Windows, the --emit=lib half cannot be driven"
        exit 0
        ;;
esac

case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT="dylib" ;;
    *)      LIB_EXT="so" ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! cc -o "$TMP/driver" "$SCRIPT_DIR/driver.c" >"$TMP/driver.log" 2>&1; then
    echo "  [FAIL] differential: could not build the lib driver"
    sed 's/^/        /' "$TMP/driver.log" | head -10
    exit 1
fi

# Carveouts: "<case> <reason>". Comments and blank lines ignored.
carveout_reason() {
    awk -v want="$1" '
        /^[[:space:]]*#/ { next }
        NF == 0          { next }
        $1 == want       { $1 = ""; sub(/^[[:space:]]+/, ""); print; exit }
    ' "$SCRIPT_DIR/carveouts.txt" 2>/dev/null
}

# A carveout for a case that no longer exists is an error, so the file cannot
# rot into a list of ghosts that quietly excuse nothing.
stale=0
while read -r name _rest; do
    case "$name" in ''|'#'*) continue ;; esac
    if [ ! -f "$SCRIPT_DIR/cases/$name.ae" ]; then
        echo "  [FAIL] differential: carveout '$name' names a case that does not exist"
        stale=1
    fi
done < "$SCRIPT_DIR/carveouts.txt"
[ "$stale" -eq 0 ] || exit 1

pass=0; fail=0; carved=0

for case_file in "$SCRIPT_DIR"/cases/*.ae; do
    [ -f "$case_file" ] || continue
    name=$(basename "$case_file" .ae)

    reason=$(carveout_reason "$name")
    if [ -n "$reason" ]; then
        echo "  [CARVEOUT] differential/$name: $reason"
        carved=$((carved + 1))
        continue
    fi

    # Path A: --emit=exe, run the binary.
    if ! "$AE" build "$case_file" -o "$TMP/$name.exe" >"$TMP/$name.exebuild.log" 2>&1; then
        echo "  [FAIL] differential/$name: --emit=exe build failed"
        sed 's/^/        /' "$TMP/$name.exebuild.log" | head -10
        fail=$((fail + 1))
        continue
    fi
    "$TMP/$name.exe" >"$TMP/$name.exe.out" 2>&1
    exe_rc=$?

    # Path B: --emit=lib, call aether_run through the driver.
    if ! "$AE" build "$case_file" --emit=lib -o "$TMP/lib$name.$LIB_EXT" \
            >"$TMP/$name.libbuild.log" 2>&1; then
        echo "  [FAIL] differential/$name: --emit=lib build failed"
        sed 's/^/        /' "$TMP/$name.libbuild.log" | head -10
        fail=$((fail + 1))
        continue
    fi
    "$TMP/driver" "$TMP/lib$name.$LIB_EXT" >"$TMP/$name.lib.out" 2>&1
    lib_rc=$?

    if [ "$exe_rc" != "$lib_rc" ]; then
        echo "  [FAIL] differential/$name: exit code differs between lowering paths"
        echo "         --emit=exe exited $exe_rc, --emit=lib exited $lib_rc"
        fail=$((fail + 1))
        continue
    fi
    if ! diff -u "$TMP/$name.exe.out" "$TMP/$name.lib.out" >"$TMP/$name.diff" 2>&1; then
        echo "  [FAIL] differential/$name: output differs between lowering paths"
        echo "         --- --emit=exe / +++ --emit=lib"
        sed 's/^/         /' "$TMP/$name.diff" | head -20
        fail=$((fail + 1))
        continue
    fi
    pass=$((pass + 1))
done

# Self-check: prove the comparison can actually fail. Without this the suite
# would report success just as happily if the diff were broken and every
# comparison were vacuous.
printf 'a\n' > "$TMP/self.a"
printf 'b\n' > "$TMP/self.b"
if diff -q "$TMP/self.a" "$TMP/self.b" >/dev/null 2>&1; then
    echo "  [FAIL] differential: the comparison does not detect a difference"
    exit 1
fi

if [ "$fail" -gt 0 ]; then
    echo "  differential: $pass agreed, $fail diverged, $carved carved out"
    exit 1
fi
echo "  [PASS] differential: $pass cases agree across --emit=exe and --emit=lib ($carved carved out)"
exit 0
