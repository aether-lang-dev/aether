#!/bin/sh
# `ae test` and `ae check` resolve [dependencies] and [patch] as `ae build`
# and `ae run` do.
#
# Reported while moving contrib/host's Lua bridge to aether-guest-languages:
# a project that ran `ae add github.com/aether-lang-dev/aether-guest-languages`
# and imported `lua` built and ran, but `ae test` failed every test that
# imported it ("FAIL (compile)", the import unresolved) and `ae check` refused
# the program the same way. Only build and run called the resolver.
#
# Asserts, for a dependency reached through [patch] (a local path, relative
# to the manifest) and one installed in the package cache:
#   - `ae run` resolves both (the baseline the other two must match)
#   - `ae check` passes on a program importing both
#   - `ae test` passes a test importing both, from the project root and from
#     a subdirectory (the manifest is found by walking up)
#   - `ae test` with a broken [patch] path fails, naming it, rather than
#     passing or failing anonymously
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] ae_test_check_dependencies: ae not built"; exit 0; }

TMPDIR_T="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_T" || true' EXIT

# A private package cache, as dep_resolution has it: never the real ~/.aether.
# get_home_dir() prefers USERPROFILE on Windows, so set both.
export HOME="$TMPDIR_T/home"
if command -v cygpath >/dev/null 2>&1; then
    USERPROFILE="$(cygpath -m "$TMPDIR_T/home")"
else
    USERPROFILE="$TMPDIR_T/home"
fi
export USERPROFILE

# An installed package: example.com/acme/gadgets, module `gadget`.
PKG="$TMPDIR_T/home/.aether/packages/example.com/acme/gadgets"
mkdir -p "$PKG/gadget"
[ -d "$PKG/gadget" ] || { echo "  [SKIP] ae_test_check_dependencies: cannot create fixture dir $PKG"; exit 0; }
printf '[package]\nname = "gadgets"\nmodules = "gadget"\n' > "$PKG/aether.toml"
printf 'exports(spin)\nspin() -> int { return 7 }\n' > "$PKG/gadget/module.ae"

# A local package reached by [patch]: example.com/acme/guests, module `guest`,
# a sibling of the project as a checkout would be.
LOCAL="$TMPDIR_T/guests"
mkdir -p "$LOCAL/guest"
printf '[package]\nname = "guests"\nmodules = "guest"\n' > "$LOCAL/aether.toml"
printf 'exports(greet)\ngreet() -> string { return "hello" }\n' > "$LOCAL/guest/module.ae"

PROJ="$TMPDIR_T/proj"
mkdir -p "$PROJ/src" "$PROJ/tests" "$PROJ/deep"
cat > "$PROJ/aether.toml" <<'TOMLEOF'
[package]
name = "consumer"

[dependencies]
"example.com/acme/gadgets" = "1.0.0"
"example.com/acme/guests" = "1.0.0"

[patch]
"example.com/acme/guests" = "../guests"
TOMLEOF
cat > "$PROJ/src/main.ae" <<'AEEOF'
import gadget
import guest
main() {
    println("${guest.greet()} ${gadget.spin()}")
    return 0
}
AEEOF
cat > "$PROJ/tests/test_deps.ae" <<'AEEOF'
import gadget
import guest
main() {
    if gadget.spin() != 7 { return 1 }
    if guest.greet() != "hello" { return 1 }
    return 0
}
AEEOF

cd "$PROJ"
fail() {
    echo "  [FAIL] ae_test_check_dependencies: $1"
    echo "$2" | sed 's/^/    /' | head -15
    exit 1
}

# --- 1. the baseline: ae run resolves both ---------------------------------
OUT=$("$AE" run src/main.ae 2>&1 || true)
case "$OUT" in *"hello 7"*) ;; *) fail "ae run did not resolve the dependencies" "$OUT" ;; esac

# --- 2. ae check ------------------------------------------------------------
if ! OUT=$("$AE" check src/main.ae 2>&1); then
    fail "ae check refused a program whose dependencies ae run resolves" "$OUT"
fi
if ! OUT=$("$AE" check 2>&1); then
    fail "ae check (project mode) refused the project" "$OUT"
fi

# --- 3. ae test, from the root and from a subdirectory ----------------------
if ! OUT=$("$AE" test 2>&1); then
    fail "ae test failed a test whose imports ae run resolves" "$OUT"
fi
case "$OUT" in *PASS*) ;; *) fail "ae test ran no passing test" "$OUT" ;; esac
if ! OUT=$(cd deep && "$AE" test ../tests/test_deps.ae 2>&1); then
    fail "ae test from a subdirectory lost the dependencies" "$OUT"
fi

# --- 4. a broken [patch] path is named, not passed over ----------------------
sed 's#"\.\./guests"#"../nowhere"#' aether.toml > aether.toml.new && mv aether.toml.new aether.toml
if OUT=$("$AE" test 2>&1); then
    fail "ae test passed with the [patch] path pointing nowhere" "$OUT"
fi
case "$OUT" in *nowhere*) ;; *) fail "ae test did not name the missing [patch] path" "$OUT" ;; esac

echo "  [PASS] ae_test_check_dependencies"
