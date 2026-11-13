#!/bin/sh
# A bare `ptr` passed where an Aether function declares a `string` parameter
# is refused, error[E0202], as a `ptr` bound to a `string` local already is
# (tests/integration/ptr_into_string_error). The callee reads whatever the
# pointer points at as text: nib's TUI passed a parsed JSON node to
# `long_of(text: string)`, which compiled, ran and read garbage (nib-code ask
# ptr-passed-as-string). The message names the parameter and the typed
# accessor that says the value is a string.
#
# Still accepted: `null`, a `string.*` producer typed `ptr`, the typed
# accessors' results, a typed pointer's own rules, and a `ptr` given to an
# EXTERN's `string` parameter (the C boundary, where both are a char*).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] ptr_string_argument: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# refused <name> <line:col> <message part> <hint>
refused() {
    name="$1"; at="$2"; what="$3"; hint="$4"
    out=$(cd "$TMP" && AETHER_HOME="" "$AE" check "$name.ae" 2>&1)
    if [ $? -eq 0 ]; then
        echo "  [FAIL] $name: accepted"; echo "$out" | sed 's/^/        /'
        fails=$((fails + 1)); return
    fi
    n=$(printf '%s\n' "$out" | grep -c "error\[E0202\]")
    case "$out" in
        *"error[E0202]: $what: expected string, got ptr"*"$name.ae:$at"*"$hint"*)
            if [ "$n" != "1" ]; then
                echo "  [FAIL] $name: $n E0202 errors, want 1"
                fails=$((fails + 1))
            fi ;;
        *) echo "  [FAIL] $name: no E0202 '$what' at $at naming $hint:"
           echo "$out" | sed 's/^/        /'
           fails=$((fails + 1)) ;;
    esac
}

# The ask's repro: a parsed JSON node given to a string parameter.
cat > "$TMP/json_node.ae" <<'AE'
import std.json
takes(text: string) -> int { return 7 }
main() {
    v, _e = json.parse("{\"a\":1}")
    println("${takes(v)}")
}
AE
refused json_node 5:22 "Argument 1 'text' of 'takes'" "string.from_ptr(p)"

# A map's value, read untyped, given to a later parameter.
cat > "$TMP/map_value.ae" <<'AE'
import std.map
greet(who: int, name: string) -> int { return who }
main() {
    m = map.new()
    map.put(m, "k", "v")
    raw, _ = map.get(m, "k")
    println("${greet(1, raw)}")
    map.free(m)
}
AE
refused map_value 7:25 "Argument 2 'name' of 'greet'" "string.from_ptr(p)"

# A module function's string parameter, called qualified (std.string's).
cat > "$TMP/std_param.ae" <<'AE'
import std.list
import std.string
main() {
    l = list.new()
    list.add(l, "12")
    n, _ = string.to_int(list.at(l, 0))
    println("${n}")
    list.free(l)
}
AE
refused std_param 6:30 "Argument 1 's' of 'string.to_int'" "list.get_string(l, index)"

cat > "$TMP/quiet.ae" <<'AE'
import std.list
import std.map
import std.string

extern strlen(s: string) -> int

takes(text: string) -> int { return string.length(text) }

main() {
    m = map.new()
    map.put(m, "k", "four")
    l = list.new()
    list.add(l, "abc")
    raw = map_get_raw(m, "k")
    s, _ = map.get_string(m, "k")
    t, _ = list.get_string(l, 0)
    u, _ = string.from_ptr(raw)
    a = takes(s) + takes(t) + takes(u) + takes(null) + takes(string.from_int(12))
    b = strlen(raw)
    println("${a} ${b}")
    list.free(l)
    map.free(m)
}
AE
out=$(cd "$TMP" && AETHER_HOME="" "$AE" run quiet.ae 2>&1)
if [ "$out" != "13 4" ]; then
    echo "  [FAIL] quiet: the accepted forms should compile and print '13 4', got:"
    echo "$out" | sed 's/^/        /'
    fails=$((fails + 1))
fi

if [ "$fails" -ne 0 ]; then exit 1; fi
echo "  [PASS] ptr_string_argument: a ptr given to an Aether string parameter is refused (E0202); null, producers, accessors and extern string parameters are not"
