#!/bin/sh
# Compiled module artifacts (.aea, issue #1746), end to end through the
# resolver of an installed toolchain.
#
# Builds a throwaway install prefix (bin/aetherc, share/aether/std, and the
# artifacts `make install` puts in lib/aether/modules, made by the same
# script), then checks that an import compiles to byte-identical C whether
# its module comes from the artifact or from the installed source, and that
# every reason to distrust an artifact sends the build back to the source.
# The codec's own rules are unit-tested in tests/compiler/test_aea.c.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -f "$AETHERC.exe" ] && AETHERC="$AETHERC.exe"
EXE=""
case "$AETHERC" in *.exe) EXE=".exe" ;; esac

pass=0
fail=0
ok()  { echo "  [PASS] $1"; pass=$((pass + 1)); }
bad() { echo "  [FAIL] $1"; fail=$((fail + 1)); }

# Byte-compare via cksum (coreutils: CRC + byte count), not cmp/diff: those
# live in diffutils, which the Windows MSYS2 CI shell does not install, and a
# missing binary's exit 127 read as "differs" and failed five cases. `same`
# and `differ` require two nonempty outputs: missing compiler output must
# never count as an identical build or as a changed build.
same()   { [ -s "$1" ] && [ -s "$2" ] && [ "$(cksum < "$1" 2>/dev/null)" = "$(cksum < "$2" 2>/dev/null)" ]; }
differ() { [ -s "$1" ] && [ -s "$2" ] && ! same "$1" "$2"; }
# For a diagnostic dump when two text files differ (cmp/diff unavailable).
show_both() { echo "--- $1 ---"; cat "$1"; echo "--- $2 ---"; cat "$2"; }

T="$(mktemp -d 2>/dev/null || mktemp -d -t aea)"
trap '[ -n "${AEA_KEEP:-}" ] || rm -rf "$T"' EXIT INT TERM
PFX="$T/prefix"
SHARE="$PFX/share/aether"
MODS="$PFX/lib/aether/modules"
mkdir -p "$PFX/bin" "$SHARE" "$T/work"
cp "$AETHERC" "$PFX/bin/aetherc$EXE"
cp -R "$ROOT/std" "$SHARE/"

# A module whose parse depends on a build symbol. No shipped std module has a
# `when defined` region yet, so the prefix gets one of its own.
mkdir -p "$SHARE/std/aeafixture"
cat > "$SHARE/std/aeafixture/module.ae" <<'AE'
exports (value)

when defined(AEA_FIXTURE_ON) {
    value() -> int { return 1 }
}

when !defined(AEA_FIXTURE_ON) {
    value() -> int { return 0 }
}
AE

# A module that parses cleanly but does not type-check, so a diagnostic has
# to point into an installed module's own source.
mkdir -p "$SHARE/std/aeabroken"
cat > "$SHARE/std/aeabroken/module.ae" <<'AE'
exports (broken)

broken() -> int {
    return broken(1, 2)
}
AE

if sh "$ROOT/scripts/build_module_artifacts.sh" "$PFX/bin/aetherc$EXE" "$SHARE" "$MODS" >"$T/build.log" 2>&1; then
    ok "every std module encodes as an artifact"
else
    bad "artifact build failed"; cat "$T/build.log"
    echo "aea_artifacts: $pass passed, $fail failed"; exit 1
fi

# The installed compiler, run the way `ae` runs it: nothing in the working
# directory or the environment points at another std tree.
aec() { (cd "$T/work" && env -u AETHER_HOME -u AETHER_NO_AEA "$PFX/bin/aetherc$EXE" "$@"); }
from_source() { (cd "$T/work" && env -u AETHER_HOME AETHER_NO_AEA=1 "$PFX/bin/aetherc$EXE" "$@"); }

cat > "$T/work/app.ae" <<'AE'
import std.string
import std.cryptography.md2
import std.cryptography.sha3
import std.cryptography.blake2
import std.cryptography.pbkdf2
import std.jsonpath
import std.aeafixture

main() {
    println(md2.md2_hex(string.bytes("abc")))
    println(aeafixture.value())
}
AE

# 1. Artifacts are used, and change nothing about the generated C.
AETHER_AEA_TRACE=1 aec app.ae "$T/aea.c" 2>"$T/trace.txt" || {
    bad "artifact build did not compile"; cat "$T/trace.txt"; exit 1
}
from_source app.ae "$T/src.c" 2>"$T/src_trace.txt" || {
    bad "source build did not compile"; cat "$T/src_trace.txt"; exit 1
}
if same "$T/aea.c" "$T/src.c"; then ok "generated C is identical with and without artifacts"
else bad "generated C differs between artifact and source builds"; fi
for m in cryptography/md2 cryptography/sha3 cryptography/blake2 cryptography/pbkdf2 jsonpath jsonpath/parser aeafixture bytes; do
    if grep -q "aea: using .*/lib/aether/modules/std/$m.aea" "$T/trace.txt"; then :
    else bad "std/$m was not loaded from its artifact"; cat "$T/trace.txt"; fi
done
if ! grep -q "not using" "$T/trace.txt"; then ok "every imported std module came from its artifact"
else bad "an artifact was refused on a clean install"; cat "$T/trace.txt"; fi

# 2. The build cache must see the artifact as an input.
aec --emit-deps="$T/deps.txt" app.ae "$T/deps.c" 2>/dev/null
if grep -q "^read .*/lib/aether/modules/std/cryptography/md2.aea$" "$T/deps.txt"; then
    ok "--emit-deps records the artifact that was read"
else bad "--emit-deps does not list the artifact"; fi

# 3. A build symbol answered differently from the artifact's parse.
AETHER_AEA_TRACE=1 aec -D AEA_FIXTURE_ON app.ae "$T/def_aea.c" 2>"$T/def_trace.txt"
from_source -D AEA_FIXTURE_ON app.ae "$T/def_src.c" 2>/dev/null
if grep -q "not using .*/aeafixture.aea: a build symbol the module tests is set differently" "$T/def_trace.txt" &&
   same "$T/def_aea.c" "$T/def_src.c" && differ "$T/def_aea.c" "$T/aea.c"; then
    ok "a changed build symbol falls back to the source"
else bad "build symbol mismatch not handled"; cat "$T/def_trace.txt"; fi

# 4. Diagnostics name the installed source, the same either way.
cat > "$T/work/bad.ae" <<'AE'
import std.cryptography.md2

main() {
    x = md2.transform(0, 0)
}
AE
aec bad.ae "$T/bad1.c" >"$T/diag_aea.txt" 2>&1
from_source bad.ae "$T/bad2.c" >"$T/diag_src.txt" 2>&1
if [ -s "$T/diag_aea.txt" ] && same "$T/diag_aea.txt" "$T/diag_src.txt"; then
    ok "diagnostics are identical with and without artifacts"
else bad "diagnostics differ"; show_both "$T/diag_aea.txt" "$T/diag_src.txt"; fi

cat > "$T/work/bad_module.ae" <<'AE'
import std.aeabroken

main() {
    println(aeabroken.broken())
}
AE
aec bad_module.ae "$T/badm1.c" >"$T/mdiag_aea.txt" 2>&1
from_source bad_module.ae "$T/badm2.c" >"$T/mdiag_src.txt" 2>&1
if grep -q "share/aether/std/aeabroken/module.ae" "$T/mdiag_aea.txt" &&
   same "$T/mdiag_aea.txt" "$T/mdiag_src.txt"; then
    ok "an error inside an installed module points at its source either way"
else bad "module-internal diagnostics differ"; show_both "$T/mdiag_aea.txt" "$T/mdiag_src.txt"; fi

# 5. An edited installed source retires its artifact.
printf '\n// local patch\n' >> "$SHARE/std/cryptography/md2/module.ae"
AETHER_AEA_TRACE=1 aec app.ae "$T/edit.c" 2>"$T/edit_trace.txt"
if grep -q "not using .*/md2.aea: source changed since the artifact was built" "$T/edit_trace.txt" &&
   grep -q "aea: using .*/sha3.aea" "$T/edit_trace.txt"; then
    ok "an edited source is parsed; its neighbours still use artifacts"
else bad "edited source not detected"; cat "$T/edit_trace.txt"; fi

# 6. A damaged artifact is ignored, not trusted.
head -c 200 "$MODS/std/cryptography/sha3.aea" > "$T/short.aea"
cp "$T/short.aea" "$MODS/std/cryptography/sha3.aea"
AETHER_AEA_TRACE=1 aec app.ae "$T/damaged.c" 2>"$T/dmg_trace.txt"
from_source app.ae "$T/damaged_src.c" 2>/dev/null
if grep -q "not using .*/sha3.aea: artifact" "$T/dmg_trace.txt" && same "$T/damaged.c" "$T/damaged_src.c"; then
    ok "a damaged artifact falls back to the source"
else bad "damaged artifact not handled"; cat "$T/dmg_trace.txt"; fi

# 7. AETHER_NO_AEA turns artifacts off entirely.
(cd "$T/work" && env -u AETHER_HOME AETHER_NO_AEA=1 AETHER_AEA_TRACE=1 "$PFX/bin/aetherc$EXE" app.ae "$T/off.c" 2>"$T/off_trace.txt")
if ! grep -q "aea:" "$T/off_trace.txt"; then ok "AETHER_NO_AEA=1 skips artifacts"
else bad "AETHER_NO_AEA=1 still consulted artifacts"; fi

# 8. Local source always wins: a std/ tree in the working directory (a
#    development checkout) and a project's own modules never use artifacts.
mkdir -p "$T/dev/lib/util"
cp -R "$ROOT/std" "$T/dev/"
cat > "$T/dev/lib/util/module.ae" <<'AE'
exports (twice)
twice(x: int) -> int { return x * 2 }
AE
cat > "$T/dev/local.ae" <<'AE'
import std.cryptography.md2
import util

main() {
    println(util.twice(21))
}
AE
(cd "$T/dev" && env -u AETHER_HOME AETHER_AEA_TRACE=1 "$PFX/bin/aetherc$EXE" local.ae "$T/local.c" 2>"$T/local_trace.txt")
if [ -s "$T/local.c" ] && ! grep -q "aea:" "$T/local_trace.txt"; then
    ok "local std/ and project modules are parsed from source"
else bad "local sources consulted artifacts"; cat "$T/local_trace.txt"; fi

echo "aea_artifacts: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
