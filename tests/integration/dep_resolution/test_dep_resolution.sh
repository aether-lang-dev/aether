#!/bin/sh
# #1901: [dependencies] resolve onto the module search path, with overrides.
#
# `ae add` installed packages that nothing read back, so every consumer wrote
# its own shell script to guess the cache layout and spell each importable
# subdirectory into --lib. The datastar-aether line reported this after their
# hand-written resolver guessed wrong (two path levels, not three), silently
# fell through to a sibling checkout that happened to exist, and stayed green
# for weeks while the package path had never once worked.
#
# Asserts:
#   - a declared dependency's module roots join `ae lib-path`
#   - `ae run` resolves an import from it with NO --lib at all
#   - the CONSUMER never spells the package's internal paths: the publishing
#     package declares them in its own [package] modules
#   - a missing dependency names itself and the fix, not "unknown module"
#   - a package that declares nothing exports nothing, and says so
#   - --override and [patch] both redirect, and both ANNOUNCE it -- the
#     property the ask cared most about, since a silent override means a green
#     local run against a working copy CI does not have
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] dep_resolution: ae not built"; exit 0; }

TMPDIR_T="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_T"' EXIT

# A private package cache, so the test never touches the real ~/.aether and
# never depends on what happens to be installed on the machine.
PKGS="$TMPDIR_T/home/.aether/packages/example.com/acme/widgets"
mkdir -p "$PKGS/frontend" "$PKGS/engine/util" "$PKGS/docs"
# Verify the fixture actually landed before asserting anything about it. A
# deep `mkdir -p` under a dot-directory silently fails on some MSYS2 setups
# (observed on a real Win11/MSYS2 box: step-by-step mkdir works, one-shot
# `mkdir -p a/.hidden/b/c/d` does not), and a fixture that was never created
# would otherwise surface as a confusing resolver failure rather than as the
# environment problem it is.
for d in "$PKGS/frontend" "$PKGS/engine/util" "$PKGS/docs"; do
    [ -d "$d" ] || { echo "  [SKIP] dep_resolution: cannot create fixture dir $d"; exit 0; }
done
cat > "$PKGS/aether.toml" <<'TOMLEOF'
[package]
name = "widgets"
modules = "frontend, engine, engine/util"
TOMLEOF
printf 'exports(paint)\npaint() -> string { return "painted" }\n' > "$PKGS/frontend/module.ae"
printf 'exports(spin)\nspin() -> int { return 7 }\n'              > "$PKGS/engine/module.ae"
printf 'exports(helper)\nhelper() -> int { return 1 }\n'          > "$PKGS/engine/util/module.ae"
# A non-module directory at the package root: joining the root would put this
# on the search path, which is why the package declares roots instead.
echo "not a module" > "$PKGS/docs/README.md"

PROJ="$TMPDIR_T/proj"
mkdir -p "$PROJ"
cat > "$PROJ/aether.toml" <<'TOMLEOF'
[package]
name = "consumer"

[dependencies]
"example.com/acme/widgets" = "1.0.0"
TOMLEOF
printf 'import frontend\nmain() {\n    println(frontend.paint())\n    return 0\n}\n' > "$PROJ/use.ae"

cd "$PROJ"

# The package cache is found via get_home_dir(), which on Windows prefers
# USERPROFILE and only falls back to HOME (tools/ae_cache.c). Setting HOME
# alone leaves a native ae.exe reading the REAL user profile, where the
# fixture does not exist. On a dev box HOME and USERPROFILE usually name the
# same directory in two spellings, so that failure appears only in CI --
# exactly how the cache_subdir_entry_root_module test was caught.
#
# A native ae.exe also cannot resolve an MSYS /tmp/... path, so hand it the
# Windows spelling, the same way http_middleware_d2 does.
export HOME="$TMPDIR_T/home"
if command -v cygpath >/dev/null 2>&1; then
    USERPROFILE="$(cygpath -m "$TMPDIR_T/home")"
else
    USERPROFILE="$TMPDIR_T/home"
fi
export USERPROFILE

# --- 1. the declared modules become search paths -----------------------
# `--lib D` means "D CONTAINS modules", so a declared module joins the path
# as its PARENT: `frontend` and `engine` put the package root on, and
# `engine/util` puts `<root>/engine` on. Asserting the leaves instead would
# pass a resolver that makes every module unimportable.
OUT=$("$AE" lib-path 2>&1 || true)
for m in "widgets$" "widgets/engine$"; do
    echo "$OUT" | grep -qE "$m" || {
        echo "  [FAIL] dep_resolution: no search path matching '$m'"
        # CI logs for the Windows legs come back empty from the API, so the
        # test has to carry its own diagnosis or a failure is unactionable.
        echo "    --- lib-path output ---"
        echo "$OUT" | sed 's/^/    /' | head -10
        echo "    --- environment ---"
        echo "    HOME=$HOME"
        echo "    USERPROFILE=${USERPROFILE:-<unset>}"
        echo "    pkg dir exists: $([ -d "$PKGS" ] && echo yes || echo NO) ($PKGS)"
        echo "    pkg manifest:   $([ -f "$PKGS/aether.toml" ] && echo yes || echo NO)"
        echo "    cwd=$(pwd)"
        exit 1; }
done
echo "$OUT" | grep -q "widgets/docs" && {
    echo "  [FAIL] dep_resolution: a non-module directory reached the search path"; exit 1; }

# --- 2. an import resolves with NO --lib -------------------------------
RUN=$("$AE" run use.ae 2>&1 || true)
case "$RUN" in
    *painted*) ;;
    *) echo "  [FAIL] dep_resolution: import did not resolve without --lib"
       echo "$RUN" | sed 's/^/    /' | head -12
       echo "    --- lib-path at this point ---"
       "$AE" lib-path 2>&1 | sed 's/^/    /' | head -8
       echo "    HOME=$HOME USERPROFILE=${USERPROFILE:-<unset>}"
       exit 1 ;;
esac

# --- 3. a missing dependency names itself and the fix -------------------
cat > "$PROJ/aether.toml" <<'TOMLEOF'
[package]
name = "consumer"

[dependencies]
"example.com/acme/absent" = "1.0.0"
TOMLEOF
MISS=$("$AE" lib-path 2>&1 || true)
case "$MISS" in
    *"is not installed"*"ae add example.com/acme/absent"*) ;;
    *) echo "  [FAIL] dep_resolution: missing dependency did not name itself and the fix"
       echo "$MISS" | sed 's/^/    /' | head -6; exit 1 ;;
esac

# --- 4. a package declaring nothing exports nothing, loudly -------------
SILENT="$TMPDIR_T/home/.aether/packages/example.com/acme/silent"
mkdir -p "$SILENT/lib"
cat > "$PROJ/aether.toml" <<'TOMLEOF'
[package]
name = "consumer"

[dependencies]
"example.com/acme/silent" = "1.0.0"
TOMLEOF
QUIET=$("$AE" lib-path 2>&1 || true)
case "$QUIET" in
    *"no aether.toml"*|*"declares no"*) ;;
    *) echo "  [FAIL] dep_resolution: an undeclaring package failed silently"
       echo "$QUIET" | sed 's/^/    /' | head -6; exit 1 ;;
esac
echo "$QUIET" | grep -q "silent/lib" && {
    echo "  [FAIL] dep_resolution: guessed a module root for a package that declares none"; exit 1; }

# --- 5. --override redirects, and SAYS so -------------------------------
FAKE="$TMPDIR_T/fake"
mkdir -p "$FAKE/frontend"
# The override path is handed to ae and echoed back in its announcement, so on
# Windows both the value we pass and the string we match must be the native
# spelling -- a native ae.exe cannot resolve an MSYS /tmp/... path.
if command -v cygpath >/dev/null 2>&1; then
    FAKE_N="$(cygpath -m "$FAKE")"
else
    FAKE_N="$FAKE"
fi
printf '[package]\nmodules = "frontend"\n' > "$FAKE/aether.toml"
printf 'exports(paint)\npaint() -> string { return "OVERRIDDEN" }\n' > "$FAKE/frontend/module.ae"
cat > "$PROJ/aether.toml" <<'TOMLEOF'
[package]
name = "consumer"

[dependencies]
"example.com/acme/widgets" = "1.0.0"
TOMLEOF
OVR=$("$AE" run use.ae --override "example.com/acme/widgets=$FAKE_N" 2>&1 || true)
case "$OVR" in
    *OVERRIDDEN*) ;;
    *) echo "  [FAIL] dep_resolution: --override did not redirect the build"
       echo "$OVR" | sed 's/^/    /' | head -8; exit 1 ;;
esac
case "$OVR" in
    *Overriding*) ;;
    *) echo "  [FAIL] dep_resolution: --override applied SILENTLY; an overridden"
       echo "         build must announce itself or CI and local disagree unseen"; exit 1 ;;
esac

# --- 6. [patch] does the same from the manifest -------------------------
cat >> "$PROJ/aether.toml" <<TOMLEOF

[patch]
"example.com/acme/widgets" = "$FAKE_N"
TOMLEOF
PATCHED=$("$AE" run use.ae 2>&1 || true)
case "$PATCHED" in
    *OVERRIDDEN*) ;;
    *) echo "  [FAIL] dep_resolution: [patch] did not redirect the build"
       echo "$PATCHED" | sed 's/^/    /' | head -8; exit 1 ;;
esac
case "$PATCHED" in
    *Overriding*) ;;
    *) echo "  [FAIL] dep_resolution: [patch] applied silently"; exit 1 ;;
esac

# --- 6b. [patch] in Cargo's inline-table form ---------------------------
# The ask quoted `{ path = "../selaenium" }`, so someone will write it. Left
# unhandled the whole brace expression reaches the filesystem as a filename.
cat > "$PROJ/aether.toml" <<TOMLEOF
[package]
name = "consumer"

[dependencies]
"example.com/acme/widgets" = "1.0.0"

[patch]
"example.com/acme/widgets" = { path = "$FAKE_N" }
TOMLEOF
TBL=$("$AE" run use.ae 2>&1 || true)
case "$TBL" in
    *OVERRIDDEN*) ;;
    *) echo "  [FAIL] dep_resolution: inline-table [patch] did not redirect"
       echo "$TBL" | sed 's/^/    /' | head -6; exit 1 ;;
esac
# The announced path must be the unwrapped one, not the raw braces.
case "$TBL" in
    *"Overriding example.com/acme/widgets -> $FAKE_N"*) ;;
    *) echo "  [FAIL] dep_resolution: inline-table override announced the raw table"
       echo "$TBL" | sed 's/^/    /' | head -4; exit 1 ;;
esac

# A table naming something we cannot honour must SAY so rather than silently
# building against the unpatched package.
cat > "$PROJ/aether.toml" <<'TOMLEOF'
[package]
name = "consumer"

[dependencies]
"example.com/acme/widgets" = "1.0.0"

[patch]
"example.com/acme/widgets" = { git = "https://example.com/x" }
TOMLEOF
GITP=$("$AE" run use.ae 2>&1 || true)
case "$GITP" in
    *"no path"*) ;;
    *) echo "  [FAIL] dep_resolution: an unusable [patch] table failed silently"
       echo "$GITP" | sed 's/^/    /' | head -6; exit 1 ;;
esac

# --- 7. `ae build` too, INCLUDING from a subdirectory -------------------
# `ae build` walks up to the manifest and chdirs there, so resolution has to
# happen after that walk-up rather than alongside the other flag handling.
# Resolving first reads no manifest and yields an empty search path -- which
# still "works" from the project root, so only the subdirectory case catches it.
cat > "$PROJ/aether.toml" <<'TOMLEOF'
[package]
name = "consumer"

[dependencies]
"example.com/acme/widgets" = "1.0.0"
TOMLEOF
mkdir -p "$PROJ/sub"
cp "$PROJ/use.ae" "$PROJ/sub/use.ae"
for where in "$PROJ" "$PROJ/sub"; do
    cd "$where"
    BOUT=$("$AE" build use.ae -o "$TMPDIR_T/built" 2>&1 || true)
    if [ ! -x "$TMPDIR_T/built" ]; then
        echo "  [FAIL] dep_resolution: ae build failed in $where"
        echo "$BOUT" | sed 's/^/    /' | head -8; exit 1
    fi
    RES=$("$TMPDIR_T/built" 2>&1 || true)
    case "$RES" in
        *painted*) ;;
        *) echo "  [FAIL] dep_resolution: built binary from $where printed '$RES'"; exit 1 ;;
    esac
    rm -f "$TMPDIR_T/built"
done
cd "$PROJ"

echo "  [PASS] dep_resolution: declared roots resolve, overrides redirect and announce"
