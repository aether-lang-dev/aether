#!/bin/sh
# Regression (#2172): a module's exports(...) list is enforced, for std
# modules as for user modules, qualified and selective.
#
# The qualified check looked the module up by exact name. A qualified use
# carries the leaf (`language.to_title_case`) and a std module registers
# under its full path (`std.language`), so no std module was ever found and
# nothing was blocked: std export lists were advisory, and `mem.long_to_ptr`
# was used across the tree while missing from std.mem's. A selective import
# (`import m (name)`) was never checked against the list at all, for any
# module.
set -eu

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
AE="$ROOT/build/ae"
if [ ! -x "$AE" ]; then
    echo "  [SKIP] exports_enforced: build/ae not built"
    exit 0
fi
AETHER_HOME=""
export AETHER_HOME

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "  [FAIL] exports_enforced: $1"
    exit 1
}

mkdir -p "$WORK/um" "$WORK/mid"
cat > "$WORK/um/module.ae" <<'AE'
exports(pub)

pub() -> int {
    return helper() + 1
}

helper() -> int {
    return 41
}
AE

# rejected <name> <want-in-message>: the program must fail to build with
# E0303 naming the symbol, and nothing else.
rejected() {
    if (cd "$WORK" && "$AE" build "$1.ae" -o "$1" > "$1.log" 2>&1); then
        fail "$1: built, but it uses an unexported name"
    fi
    grep -q "E0303" "$WORK/$1.log" && grep -q "'$2' is not exported" "$WORK/$1.log" \
        || { sed 's/^/    /' "$WORK/$1.log" | head -12; fail "$1: expected E0303 for '$2'"; }
}

# accepted <name> <expected last line>
accepted() {
    (cd "$WORK" && "$AE" run "$1.ae" > "$1.log" 2>&1) \
        || { sed 's/^/    /' "$WORK/$1.log" | head -12; fail "$1: should build and run"; }
    [ "$(tail -1 "$WORK/$1.log")" = "$2" ] \
        || { sed 's/^/    /' "$WORK/$1.log" | head -12; fail "$1: expected '$2'"; }
}

# --- qualified ----------------------------------------------------------
cat > "$WORK/q_std.ae" <<'AE'
import std.language

main() {
    println(language.to_title_case("abc"))
}
AE
rejected q_std to_title_case

cat > "$WORK/q_user.ae" <<'AE'
import um

main() {
    println("${um.helper()}")
}
AE
rejected q_user helper

# --- selective ----------------------------------------------------------
cat > "$WORK/s_std.ae" <<'AE'
import std.language (to_title_case)

main() {
    println(to_title_case("abc"))
}
AE
rejected s_std to_title_case

cat > "$WORK/s_user.ae" <<'AE'
import um (helper)

main() {
    println("${helper()}")
}
AE
rejected s_user helper

# ...and from inside a module, reported against that module's file.
cat > "$WORK/mid/module.ae" <<'AE'
import um (helper)
exports(go)

go() -> int {
    return helper()
}
AE
cat > "$WORK/s_nested.ae" <<'AE'
import mid

main() {
    println("${mid.go()}")
}
AE
rejected s_nested helper
grep -q "mid/module.ae:1:" "$WORK/s_nested.log" \
    || { sed 's/^/    /' "$WORK/s_nested.log" | head -8; fail "s_nested: the error should point at mid/module.ae"; }

# --- still allowed ------------------------------------------------------
# Exported names, and the `<leaf>_name` convention: std.math exports
# math_sqrt, which `math.sqrt` and `import std.math (sqrt)` both reach.
cat > "$WORK/ok.ae" <<'AE'
import um
import std.math
import std.math (sqrt)
import std.language

main() {
    t, _ = language.parse("en-us")
    println("${um.pub()} ${math.sqrt(16.0)} ${sqrt(9.0)} ${t}")
}
AE
accepted ok "42 4 3 en-US"

echo "  [PASS] exports_enforced: unexported names rejected qualified and selective, std and user; exported and prefixed names allowed"
