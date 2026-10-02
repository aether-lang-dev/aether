#!/bin/sh
# #2335: a dependency's own [dependencies] resolve for the project using it.
#
# Only the project's manifest was read, so `app -> pkga -> pkgb` failed with
# "unresolved import 'beta'" from app: the consumer had to declare pkgb itself
# and patch it to a path inside pkga's checkout. The real case was a game
# built on ae3d having to patch ae3d's physics submodule.
#
# Asserts:
#   - a dependency's [dependencies] resolve transitively, its [patch] paths
#     against ITS root; `ae lib-path` lists both packages
#   - the project's [patch] wins over a dependency's for the same package
#   - one package resolving to two places is an error naming both paths and
#     who required each, and the command stops
#   - a diamond (two paths to one place) and a cycle are fine
#   - a missing transitive dependency names who requires it
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] dep_transitive: ae not built"; exit 0; }

T="$(mktemp -d)"
trap 'rm -rf "$T" || true' EXIT

# A private, empty package cache (see dep_resolution for why both HOME and
# USERPROFILE are set, and why the Windows spelling).
mkdir -p "$T/home"
export HOME="$T/home"
if command -v cygpath >/dev/null 2>&1; then
    USERPROFILE="$(cygpath -m "$T/home")"
else
    USERPROFILE="$T/home"
fi
export USERPROFILE

fail() {
    echo "  [FAIL] dep_transitive: $1"
    if [ -n "$2" ]; then printf '%s\n' "$2" | sed 's/^/    /' | head -15; fi
    exit 1
}

# pkgb: twice(x) = 2x. pkgb2: the same module, twice(x) = 3x.
mkdir -p "$T/pkgb/beta" "$T/pkgb2/beta" "$T/pkga/alpha" "$T/pkgc/gamma" "$T/app"
printf '[package]\nname = "pkgb"\nmodules = "."\n' > "$T/pkgb/aether.toml"
printf 'exports (twice)\ntwice(x: int) -> int { return x * 2 }\n' > "$T/pkgb/beta/module.ae"
printf '[package]\nname = "pkgb"\nmodules = "."\n' > "$T/pkgb2/aether.toml"
printf 'exports (twice)\ntwice(x: int) -> int { return x * 3 }\n' > "$T/pkgb2/beta/module.ae"

# pkga depends on pkgb, patched relative to pkga's own root.
cat > "$T/pkga/aether.toml" <<'TOMLEOF'
[package]
name = "pkga"
modules = "."

[dependencies]
"example.com/pkgb" = "0.1.0"

[patch]
"example.com/pkgb" = "../pkgb"
TOMLEOF
printf 'import beta\nexports (quad)\nquad(x: int) -> int { return beta.twice(beta.twice(x)) }\n' > "$T/pkga/alpha/module.ae"

printf 'import alpha\nmain() {\n    println("quad(3) = ${alpha.quad(3)}")\n}\n' > "$T/app/main.ae"

write_app() {
    # $1: extra [patch] lines for the project ("" for none);
    # $APP_EXTRA_DEPS: extra [dependencies] lines
    {
        printf '[package]\nname = "app"\n\n[dependencies]\n"example.com/pkga" = "0.1.0"\n'
        if [ -n "$APP_EXTRA_DEPS" ]; then printf '%s\n' "$APP_EXTRA_DEPS"; fi
        printf '\n[patch]\n"example.com/pkga" = "../pkga"\n'
        if [ -n "$1" ]; then printf '%s\n' "$1"; fi
    } > "$T/app/aether.toml"
}

cd "$T/app"

# 1. The chain resolves from app, through pkga's own [patch].
APP_EXTRA_DEPS=""
write_app ""
OUT=$("$AE" run main.ae 2>&1) || fail "app -> pkga -> pkgb did not run" "$OUT"
case "$OUT" in
    *"quad(3) = 12"*) ;;
    *) fail "expected quad(3) = 12 through pkga's patched pkgb" "$OUT" ;;
esac
LP=$("$AE" lib-path 2>/dev/null) || fail "ae lib-path failed" "$LP"
case "$LP" in *pkga*) ;; *) fail "lib-path is missing pkga" "$LP" ;; esac
case "$LP" in *pkgb*) ;; *) fail "lib-path is missing pkgb (transitive)" "$LP" ;; esac

# 2. The project's [patch] wins over pkga's for the same package.
write_app '"example.com/pkgb" = "../pkgb2"'
OUT=$("$AE" run main.ae 2>&1) || fail "consumer patch of a transitive dependency failed" "$OUT"
case "$OUT" in
    *"quad(3) = 27"*) ;;
    *) fail "the project's [patch] did not win over pkga's" "$OUT" ;;
esac

# 3. Two packages patching pkgb to different places is an error naming both.
mkdir -p "$T/pkgc"
cat > "$T/pkgc/aether.toml" <<'TOMLEOF'
[package]
name = "pkgc"
modules = "."

[dependencies]
"example.com/pkgb" = "0.1.0"

[patch]
"example.com/pkgb" = "../pkgb2"
TOMLEOF
printf 'import beta\nexports (six)\nsix(x: int) -> int { return beta.twice(x) }\n' > "$T/pkgc/gamma/module.ae"
APP_EXTRA_DEPS='"example.com/pkgc" = "0.1.0"'
write_app '"example.com/pkgc" = "../pkgc"'
if OUT=$("$AE" run main.ae 2>&1); then
    fail "a package resolving to two places built anyway" "$OUT"
fi
case "$OUT" in
    *"resolves to two places"*) ;;
    *) fail "the conflict was not reported" "$OUT" ;;
esac
case "$OUT" in *pkgb2*) ;; *) fail "the conflict does not name pkgc's path" "$OUT" ;; esac
case "$OUT" in *"required by example.com/pkga"*) ;; *) fail "the conflict does not name pkga" "$OUT" ;; esac
case "$OUT" in *"required by example.com/pkgc"*) ;; *) fail "the conflict does not name pkgc" "$OUT" ;; esac
if "$AE" lib-path >/dev/null 2>&1; then
    fail "ae lib-path succeeded on a conflicting graph"
fi

# ...and the project settles it with its own [patch].
write_app '"example.com/pkgc" = "../pkgc"
"example.com/pkgb" = "../pkgb"'
OUT=$("$AE" run main.ae 2>&1) || fail "the project's [patch] did not settle the conflict" "$OUT"
case "$OUT" in *"quad(3) = 12"*) ;; *) fail "settled conflict ran the wrong pkgb" "$OUT" ;; esac

# 4. A diamond: pkgc patches pkgb to the same place pkga does.
sed 's#"../pkgb2"#"../pkgb"#' "$T/pkgc/aether.toml" > "$T/pkgc/aether.toml.new"
mv "$T/pkgc/aether.toml.new" "$T/pkgc/aether.toml"
write_app '"example.com/pkgc" = "../pkgc"'
OUT=$("$AE" run main.ae 2>&1) || fail "a diamond to one pkgb failed" "$OUT"
case "$OUT" in *"quad(3) = 12"*) ;; *) fail "diamond ran the wrong pkgb" "$OUT" ;; esac

# 5. A cycle: pkgb depends back on pkga. Resolution terminates.
cat >> "$T/pkgb/aether.toml" <<'TOMLEOF'

[dependencies]
"example.com/pkga" = "0.1.0"

[patch]
"example.com/pkga" = "../pkga"
TOMLEOF
APP_EXTRA_DEPS=""
write_app ""
OUT=$("$AE" run main.ae 2>&1) || fail "a dependency cycle failed" "$OUT"
case "$OUT" in *"quad(3) = 12"*) ;; *) fail "the cycle ran the wrong program" "$OUT" ;; esac

# 6. A transitive dependency that is neither patched nor installed names
#    who requires it.
printf '[package]\nname = "pkga"\nmodules = "."\n\n[dependencies]\n"example.com/nowhere" = "0.1.0"\n' > "$T/pkga/aether.toml"
OUT=$("$AE" run main.ae 2>&1 || true)
case "$OUT" in
    *"'example.com/nowhere' (required by example.com/pkga) is not installed"*) ;;
    *) fail "a missing transitive dependency does not name its requirer" "$OUT" ;;
esac

echo "  [PASS] dep_transitive: dependencies resolve transitively, the project's patch wins, conflicts stop"
