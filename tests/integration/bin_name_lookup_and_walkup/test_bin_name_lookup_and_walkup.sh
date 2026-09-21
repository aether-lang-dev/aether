#!/bin/sh
# Tests two related improvements to `ae build` (#280):
#   (1) Bin-name lookup: `ae build myprobe` resolves `myprobe` to the
#       [[bin]] entry's `path` field instead of treating it as a
#       literal file path that doesn't exist.
#   (2) aether.toml walk-up: running `ae build foo.ae` from a
#       subdirectory where the toml lives in an ancestor directory
#       finds and uses that toml — extra_sources are applied, the
#       link succeeds.
#   (3) The two TOGETHER (#1905): `ae build <bin-name>` from a
#       subdirectory. The walk-up rebases a relative positional so a
#       file path still resolves after the chdir, and it used to do
#       that to a bin NAME as well: `myprobe` became `ae/myprobe`,
#       which is not a file, so the build failed from a subdirectory
#       while the identical command worked from the root.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

# Runs on Windows too: the walk-up is the same code (it scans both
# separators), and the case below with a relative --extra / -o from a
# subdirectory is exactly the kind of path handling a Windows lane should
# see. (An earlier skip here gave no reason.)

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

pass=0
fail=0

# Case 1: ae build <bin-name> from project root.
cd "$SCRIPT_DIR" || exit 1
if "$AE" build myprobe -o "$TMPDIR/probe1" >"$TMPDIR/build1.log" 2>&1; then
    if "$TMPDIR/probe1" 2>&1 | grep -q "^42$"; then
        echo "  [PASS] ae build <bin-name> resolves to [[bin]] entry"
        pass=$((pass + 1))
    else
        echo "  [FAIL] bin built but didn't print 42 (extra_sources not applied?)"
        fail=$((fail + 1))
    fi
else
    echo "  [FAIL] ae build myprobe failed"
    cat "$TMPDIR/build1.log"
    fail=$((fail + 1))
fi

# Case 2: ae build <file.ae> from a subdir, walking up to find toml.
cd "$SCRIPT_DIR/ae" || exit 1
if "$AE" build myprobe.ae -o "$TMPDIR/probe2" >"$TMPDIR/build2.log" 2>&1; then
    if "$TMPDIR/probe2" 2>&1 | grep -q "^42$"; then
        echo "  [PASS] ae build foo.ae walks up to find aether.toml"
        pass=$((pass + 1))
    else
        echo "  [FAIL] walk-up bin built but didn't print 42"
        fail=$((fail + 1))
    fi
else
    echo "  [FAIL] ae build myprobe.ae from subdir failed"
    cat "$TMPDIR/build2.log"
    fail=$((fail + 1))
fi

# Case 3 (#1905): ae build <bin-name> from a subdir. The name must NOT be
# rebased into a path.
cd "$SCRIPT_DIR/ae" || exit 1
if "$AE" build myprobe -o "$TMPDIR/probe3" >"$TMPDIR/build3.log" 2>&1; then
    if "$TMPDIR/probe3" 2>&1 | grep -q "^42$"; then
        echo "  [PASS] ae build <bin-name> works from a subdirectory"
        pass=$((pass + 1))
    else
        echo "  [FAIL] subdir bin-name built but didn't print 42"
        fail=$((fail + 1))
    fi
else
    echo "  [FAIL] ae build myprobe from a subdir failed"
    cat "$TMPDIR/build3.log"
    fail=$((fail + 1))
fi

# Case 4: an argument that is neither a bin name nor a file is reported as
# what the user TYPED, not as a path they never mentioned.
cd "$SCRIPT_DIR/ae" || exit 1
"$AE" build nosuchthing -o "$TMPDIR/probe4" >"$TMPDIR/build4.log" 2>&1
if grep -q "nosuchthing" "$TMPDIR/build4.log" && \
   ! grep -q "ae/nosuchthing" "$TMPDIR/build4.log"; then
    echo "  [PASS] an unknown positional is reported as typed"
    pass=$((pass + 1))
else
    echo "  [FAIL] unknown positional was rebased in the error message"
    cat "$TMPDIR/build4.log"
    fail=$((fail + 1))
fi

# Case 5: a relative --extra and a relative -o typed from a subdirectory
# mean "relative to where I stand", like `cc -o`. The walk-up re-based only
# the source: `ae build ex.ae --extra shim.c -o ex1` from `sub/` looked for
# `<root>/shim.c` ("No such file or directory") and wrote `<root>/ex1`.
WORK="$TMPDIR/work"
mkdir -p "$WORK/sub"
printf '[[bin]]\nname = "unrelated"\npath = "unrelated.ae"\n' > "$WORK/aether.toml"
printf 'int sub_shim(void) { return 7; }\n' > "$WORK/sub/shim.c"
printf 'extern sub_shim() -> int\nmain() { println("${sub_shim()}") }\n' > "$WORK/sub/ex.ae"
cd "$WORK/sub" || exit 1
if "$AE" build ex.ae --extra shim.c -o ex1 >"$TMPDIR/build5.log" 2>&1 \
   && { [ -x "$WORK/sub/ex1" ] || [ -x "$WORK/sub/ex1.exe" ]; }; then
    got="$(cd "$WORK/sub" && ./ex1 2>&1)"
    if [ "$got" = "7" ]; then
        echo "  [PASS] a relative --extra and -o from a subdirectory resolve where they were typed"
        pass=$((pass + 1))
    else
        echo "  [FAIL] subdirectory build printed '$got'"
        fail=$((fail + 1))
    fi
else
    echo "  [FAIL] ae build ex.ae --extra shim.c -o ex1 from a subdirectory"
    cat "$TMPDIR/build5.log"
    ls "$WORK" "$WORK/sub"
    fail=$((fail + 1))
fi

# Case 6: the same without a positional (project mode, src/main.ae): the
# subdirectory is recorded whether or not a file was named.
mkdir -p "$WORK/src"
printf 'extern sub_shim() -> int\nmain() { println("${sub_shim()}") }\n' > "$WORK/src/main.ae"
cp "$WORK/sub/shim.c" "$WORK/src/shim.c"
cd "$WORK/src" || exit 1
if "$AE" build -o app --extra shim.c >"$TMPDIR/build6.log" 2>&1 \
   && { [ -x "$WORK/src/app" ] || [ -x "$WORK/src/app.exe" ]; }; then
    got="$(cd "$WORK/src" && ./app 2>&1)"
    if [ "$got" = "7" ]; then
        echo "  [PASS] project-mode build from src/ re-bases -o and --extra too"
        pass=$((pass + 1))
    else
        echo "  [FAIL] project-mode subdirectory build printed '$got'"
        fail=$((fail + 1))
    fi
else
    echo "  [FAIL] ae build -o app --extra shim.c from src/ (project mode)"
    cat "$TMPDIR/build6.log"
    fail=$((fail + 1))
fi

echo ""
echo "bin_name_lookup_and_walkup: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
