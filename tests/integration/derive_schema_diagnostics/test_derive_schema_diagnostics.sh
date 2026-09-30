#!/bin/sh
# @derive(schema) and field attributes: the forms that must not compile
# (#2298), each with the diagnostic that names what to do.
#
#   - an attribute on a struct that does not derive schema (nothing would
#     carry it, so it is an error rather than silently dropped);
#   - the same attribute twice on one field;
#   - an argument that is not a literal;
#   - a duration argument;
#   - an unknown derive, whose message lists schema among the supported.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

fail=0

# expect_error <name> <needle> — compile $TMPDIR/<name>.ae, expect failure
# and <needle> in the output.
expect_error() {
    if AETHER_HOME="$ROOT" "$AE" run "$TMPDIR/$1.ae" >"$TMPDIR/$1.log" 2>&1; then
        echo "  [FAIL] derive_schema_diagnostics: $1 compiled clean"
        fail=1
        return
    fi
    if ! grep -qF "$2" "$TMPDIR/$1.log"; then
        echo "  [FAIL] derive_schema_diagnostics: $1: expected '$2'"
        head -10 "$TMPDIR/$1.log" | sed 's/^/          /'
        fail=1
    fi
}

cat >"$TMPDIR/no_schema.ae" <<'AE'
struct A {
    x: int @range(0, 1)
}
main() {
    a = A { x: 1 }
    println("${a.x}")
}
AE
expect_error no_schema "needs \`@derive(schema)\` on the struct"

cat >"$TMPDIR/twice.ae" <<'AE'
@derive(schema)
struct A {
    x: int @range(0, 1) @range(2, 3)
}
main() {}
AE
expect_error twice "has \`@range\` twice"

cat >"$TMPDIR/not_literal.ae" <<'AE'
@derive(schema)
struct A {
    x: int @range(y, 1)
}
main() {}
AE
expect_error not_literal "arguments are literals"

cat >"$TMPDIR/duration.ae" <<'AE'
@derive(schema)
struct A {
    x: int @timeout(5s)
}
main() {}
AE
expect_error duration "a duration is not a field attribute argument"

cat >"$TMPDIR/unknown.ae" <<'AE'
@derive(bogus)
struct A {
    x: int
}
main() {}
AE
expect_error unknown "(supported: eq, schema)"

[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] derive_schema_diagnostics: 5 rejected forms, each named"
exit 0
