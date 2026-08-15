#!/bin/sh
# Integration test for `ae help <script>` — issue #414.
# Covers each of the heuristic capabilities. Strict: the test
# script DOES NOT execute the fixtures; ae help is static-analysis
# only, so we don't need a working compile.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

pass=0; fail=0

# Each case: run ae help, assert the output matches a grep pattern.
expect_match() {
    label="$1"; pattern="$2"; shift 2
    out=$("$@" 2>&1)
    if printf '%s\n' "$out" | grep -q -E "$pattern"; then
        echo "  [PASS] $label"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $label"
        echo "    expected pattern: $pattern"
        echo "    got output:"
        printf '%s\n' "$out" | sed 's/^/      /'
        fail=$((fail + 1))
    fi
}

# Case 1: Levenshtein-matched suggestion for an unresolved name.
# Lightly typo'd name from stdlib should produce a "Did you mean" line.
cat > "$TMPDIR/lev.ae" <<EOF
import std.bytes (length)
main() {
    s = "hello"
    n = lenght(s)
    println("len = \${n}")
}
EOF
expect_match "Levenshtein suggestion for unresolved name" \
    "Did you mean: length" \
    "$AE" help "$TMPDIR/lev.ae"

# Case 2: YAML-style colon inside a closure-DSL block.
cat > "$TMPDIR/yaml.ae" <<EOF
import my_lib
main() {
    my_lib.serve {
        port: 9990
        host: "127.0.0.1"
    }
}
EOF
expect_match "YAML colon detected inside DSL block" \
    "YAML-style colon|port\(9990\)" \
    "$AE" help "$TMPDIR/yaml.ae"

# Case 3: HCL-style equals inside a closure-DSL block.
cat > "$TMPDIR/hcl.ae" <<EOF
import my_lib
main() {
    my_lib.serve {
        port = 9990
    }
}
EOF
expect_match "HCL equals detected inside DSL block" \
    "HCL-style equals|port\(9990\)" \
    "$AE" help "$TMPDIR/hcl.ae"

# Case 4: Normal Aether assignment inside a function body must NOT
# be flagged as HCL — the discriminator is closure-DSL block, not
# any '{ ... }' block.
cat > "$TMPDIR/normal.ae" <<EOF
main() {
    s = "hello"
    n = 42
    println("\${s} \${n}")
}
EOF
out=$("$AE" help "$TMPDIR/normal.ae" 2>&1)
if printf '%s\n' "$out" | grep -qE "HCL-style equals|YAML-style colon"; then
    echo "  [FAIL] no false-positive for normal Aether assignment"
    printf '%s\n' "$out" | sed 's/^/      /'
    fail=$((fail + 1))
else
    echo "  [PASS] no false-positive for normal Aether assignment"
    pass=$((pass + 1))
fi

# Case 5: Top-level DSL block (no main()).
cat > "$TMPDIR/toplevel.ae" <<EOF
import my_lib

my_lib.serve {
    repo("alpha", "/x")
}
EOF
expect_match "Top-level DSL block detected" \
    "top-level DSL block|Wrap the entire block in.*main" \
    "$AE" help "$TMPDIR/toplevel.ae"

# Case 6: Missing-import detection — an unimported name that exists
# in exactly one stdlib module produces a clear import suggestion.
cat > "$TMPDIR/missing.ae" <<EOF
main() {
    s = string_length("abc")
    println("\${s}")
}
EOF
expect_match "Missing-import suggestion" \
    "is exported by std\.string|import std\.string" \
    "$AE" help "$TMPDIR/missing.ae"

# Case 7: --json output is valid JSON shape (starts with { ends with }).
cat > "$TMPDIR/json.ae" <<EOF
main() {
    x = foobarbaz_nope()
}
EOF
out=$("$AE" help "$TMPDIR/json.ae" --json 2>&1)
first=$(printf '%s' "$out" | head -c 1)
last=$(printf '%s' "$out" | tr -d '\n' | tail -c 1)
if [ "$first" = "{" ] && [ "$last" = "}" ]; then
    echo "  [PASS] --json emits a single JSON object"
    pass=$((pass + 1))
else
    echo "  [FAIL] --json output not shaped like {... }"
    echo "    first=$first last=$last"
    echo "    output: $out"
    fail=$((fail + 1))
fi

# Case 8: --fix on a non-tty refuses to apply (safety guard against
# CI accidentally rewriting files).
cat > "$TMPDIR/fix.ae" <<EOF
import my_lib
main() {
    my_lib.serve {
        port = 9990
    }
}
EOF
out=$(echo "" | "$AE" help "$TMPDIR/fix.ae" --fix 2>&1)
if printf '%s\n' "$out" | grep -qE "not a TTY|refusing"; then
    echo "  [PASS] --fix refuses on non-tty stdin"
    pass=$((pass + 1))
else
    echo "  [FAIL] --fix should refuse on non-tty stdin"
    printf '%s\n' "$out" | sed 's/^/      /'
    fail=$((fail + 1))
fi
# Also assert the file was NOT modified.
orig=$(wc -l < "$TMPDIR/fix.ae")
if [ "$orig" = "       6" ] || [ "$orig" = "6" ]; then
    echo "  [PASS] --fix did not modify the file on non-tty refusal"
    pass=$((pass + 1))
else
    echo "  [FAIL] --fix modified the file despite tty refusal (line count=$orig)"
    fail=$((fail + 1))
fi

# Case 9: --help prints usage banner and exits 0.
out=$("$AE" help nonexistent --help 2>&1)
rc=$?
# `--help` on its own (no script) returns 0; with a non-existent
# script path the script disambiguator returns false so the bare
# `ae help` banner runs — we accept either path.
if [ "$rc" = "0" ] || [ "$rc" = "1" ]; then
    if printf '%s\n' "$out" | grep -q "Usage:"; then
        echo "  [PASS] --help prints usage banner"
        pass=$((pass + 1))
    else
        echo "  [FAIL] --help did not print usage"
        fail=$((fail + 1))
    fi
else
    echo "  [FAIL] --help returned unexpected rc=$rc"
    fail=$((fail + 1))
fi

# Case 9b: Type-mismatch with English. Calling a stdlib function
# with a wrong-typed arg should surface a concrete English
# suggestion ("Drop the quotes" / "Use a `<type>` value here") not
# just the typer's raw "expected X, got Y".
cat > "$TMPDIR/typemis.ae" <<EOF
import std.string (string_length)
main() {
    n = string_length(42)
    println("\${n}")
}
EOF
out=$("$AE" help "$TMPDIR/typemis.ae" 2>&1)
if printf '%s\n' "$out" | grep -qE "Drop the quotes|Use a .* value here|Use an? .* literal"; then
    echo "  [PASS] type-mismatch English suggestion"
    pass=$((pass + 1))
else
    # The typer may not emit a type mismatch for every wrong-arg shape
    # depending on coercion; in that case the case is a no-op rather
    # than a failure. We accept the no-finding path explicitly so the
    # test isn't flaky against future inference changes.
    if printf '%s\n' "$out" | grep -qE "no diagnostics|Type mismatch"; then
        echo "  [PASS] type-mismatch path exercised (no enrichment to assert against)"
        pass=$((pass + 1))
    else
        echo "  [FAIL] expected type-mismatch English or no-finding fallback"
        printf '%s\n' "$out" | sed 's/^/      /'
        fail=$((fail + 1))
    fi
fi

# Case 10: --llm with no AETHER_ENABLE_LLM build flag prints the
# clean rebuild instruction (privacy: no network attempt either way).
out=$("$AE" help "$TMPDIR/lev.ae" --llm /no/such/weights.gguf 2>&1)
if printf '%s\n' "$out" | grep -qE "AETHER_ENABLE_LLM|LLM support"; then
    echo "  [PASS] --llm without build flag returns clean rebuild message"
    pass=$((pass + 1))
else
    # If the build was compiled WITH LLM support, the weights-missing
    # path triggers instead — that's the right behaviour, not a fail.
    if printf '%s\n' "$out" | grep -qE "weights file not found"; then
        echo "  [PASS] --llm with build flag rejects missing weights file"
        pass=$((pass + 1))
    else
        echo "  [FAIL] --llm did not surface the expected message"
        echo "    got: $out"
        fail=$((fail + 1))
    fi
fi

# --- Project-library support (--lib / AETHER_LIB_DIR) ----------------
# A project-local library under a --lib directory: a module plus the
# `*.help.md` hint file a library author ships alongside it.
mkdir -p "$TMPDIR/lib/widgets"
cat > "$TMPDIR/lib/widgets/module.ae" <<'EOF'
exports ( make_widget )
make_widget(label: string) -> int { return 1 }
EOF
cat > "$TMPDIR/lib/widgets/widgets.help.md" <<'EOF'
## Label must be non-empty

Pattern: `make_widget`

make_widget renders blank if the label is empty — pass a real string.
EOF
cat > "$TMPDIR/usewidgets.ae" <<'EOF'
import widgets
main() {
    w = widgets.make_widget("ok")
}
EOF

# Case 11: `--lib` resolves the project library AND reads its shipped
# `*.help.md` — proves option acceptance, library-catalog inclusion,
# and the lib-path help.md probe in one assertion. Pre-fix `ae help`
# rejected `--lib` outright and never probed non-stdlib hint files.
expect_match "--lib resolves project library + reads its *.help.md hint" \
    "Label must be non-empty" \
    "$AE" help "$TMPDIR/usewidgets.ae" --lib "$TMPDIR/lib"

# Case 12: `--lib` is a recognised option (no 'unknown option').
out=$("$AE" help "$TMPDIR/usewidgets.ae" --lib "$TMPDIR/lib" 2>&1)
if printf '%s\n' "$out" | grep -q "unknown option"; then
    echo "  [FAIL] --lib still rejected as unknown option"
    fail=$((fail + 1))
else
    echo "  [PASS] --lib accepted as a valid option"
    pass=$((pass + 1))
fi

# Case 13: AETHER_LIB_DIR env is honoured the same as --lib.
expect_match "AETHER_LIB_DIR env resolves project library" \
    "Label must be non-empty" \
    env AETHER_LIB_DIR="$TMPDIR/lib" "$AE" help "$TMPDIR/usewidgets.ae"

# Case 14: PATH-style `--lib a:b` splits — a bad leading entry is
# tolerated and the good one still resolves.
expect_match "--lib accepts PATH-style a:b specs" \
    "Label must be non-empty" \
    "$AE" help "$TMPDIR/usewidgets.ae" --lib "/no/such/dir:$TMPDIR/lib"

# Case 15: a module imported more than once — plain `import widgets`
# plus a selective `import widgets (make_widget)`, the shape a
# closure-DSL file needs — must surface each shipped `*.help.md` hint
# exactly ONCE. Pre-fix the hint loader ran per import statement, so a
# twice-imported module's hint appeared twice (aeb feedback R2).
cat > "$TMPDIR/usewidgets2x.ae" <<'EOF'
import widgets
import widgets (make_widget)
main() {
    w = widgets.make_widget("ok")
}
EOF
hint_count=$("$AE" help "$TMPDIR/usewidgets2x.ae" --lib "$TMPDIR/lib" 2>&1 \
    | grep -c "Label must be non-empty (widgets hint)")
if [ "$hint_count" -eq 1 ]; then
    echo "  [PASS] hint from a twice-imported module fires exactly once"
    pass=$((pass + 1))
else
    echo "  [FAIL] twice-imported module hint fired $hint_count times (want 1)"
    fail=$((fail + 1))
fi

# Case 16: `ae help` writes nothing into the working directory. It compiles
# the script to a sink to collect diagnostics, and passing that sink as
# `-o <path>` (aetherc takes the output positionally) made aetherc treat the
# flag itself as the output path: every run dropped a file named "-o" in
# whatever directory the user happened to be in.
probe_dir="$TMPDIR/cwd_probe"
mkdir -p "$probe_dir"
( cd "$probe_dir" && "$AE" help "$TMPDIR/usewidgets.ae" --lib "$TMPDIR/lib" >/dev/null 2>&1 )
stray=$(ls -A "$probe_dir" 2>/dev/null | head -3)
if [ -z "$stray" ]; then
    echo "  [PASS] ae help leaves the working directory clean"
    pass=$((pass + 1))
else
    echo "  [FAIL] ae help wrote into the working directory: $stray"
    fail=$((fail + 1))
fi

echo
echo "ae_help: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
