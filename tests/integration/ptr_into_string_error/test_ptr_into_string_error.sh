#!/bin/sh
# A bare `ptr` given to a `string` local (`s = map_get_raw(m, k)`, or a tuple
# slot, `s, err = map.get(m, k)`) would be read as text whatever it points at:
# a list, a struct, bytes with no end. It is refused, error[E0202] (W1005, a
# warning, in 0.830.0 and 0.831.0), naming the typed accessor that says the
# value is a string and checks it is one (`map.get_string`, `list.get_string`,
# `string.from_ptr`). `ae check` and `ae build` both refuse it.
#
# What is not that still compiles and runs: `null`, a `string.*` producer whose
# result is typed `ptr` (a counted string), the accessors themselves, a local
# whose first binding is the `ptr`, and a typed pointer.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] ptr_into_string_warn: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# refused <name> <line:col> <hint>: `ae check` refuses <name>.ae with exactly
# one E0202, at that position, naming <hint>.
refused() {
    name="$1"; at="$2"; hint="$3"
    out=$(cd "$TMP" && "$AE" check "$name.ae" 2>&1)
    if [ $? -eq 0 ]; then
        echo "  [FAIL] $name: accepted"; echo "$out" | sed 's/^/        /'
        fails=$((fails + 1)); return
    fi
    n=$(printf '%s\n' "$out" | grep -c "error\[E0202\]")
    case "$out" in
        *"error[E0202]: "*"a \`ptr\` given to it would be read as text whatever it points at"*"$name.ae:$at"*"$hint"*)
            if [ "$n" != "1" ]; then
                echo "  [FAIL] $name: $n E0202 errors, want 1"
                fails=$((fails + 1))
            fi ;;
        *) echo "  [FAIL] $name: no E0202 at $at naming $hint:"
           echo "$out" | sed 's/^/        /'
           fails=$((fails + 1)) ;;
    esac
    if printf '%s' "$out" | grep -q "W1005"; then
        echo "  [FAIL] $name: still says W1005"
        fails=$((fails + 1))
    fi
}

cat > "$TMP/plain_map.ae" <<'AE'
import std.map

main() {
    m = map.new()
    map.put(m, "k", "v")
    s = "x"
    s = map_get_raw(m, "k")
    println(s)
    map.free(m)
}
AE
refused plain_map 7:5 "map.get_string(m, key)"

cat > "$TMP/tuple_map.ae" <<'AE'
import std.map

main() {
    m = map.new()
    map.put(m, "k", "v")
    name = ""
    name, err = map.get(m, "k")
    println("${name}${err}")
    map.free(m)
}
AE
refused tuple_map 7:5 "map.get_string(m, key)"

cat > "$TMP/plain_list.ae" <<'AE'
import std.list

main() {
    l = list.new()
    list.add(l, "a")
    s = ""
    s = list_get_raw(l, 0)
    println(s)
    list.free(l)
}
AE
refused plain_list 7:5 "list.get_string(l, index)"

cat > "$TMP/tuple_list.ae" <<'AE'
import std.list

main() {
    l = list.new()
    list.add(l, "a")
    s = ""
    s, err = list.get(l, 0)
    println("${s}${err}")
    list.free(l)
}
AE
refused tuple_list 7:5 "list.get_string(l, index)"

cat > "$TMP/plain_ptr.ae" <<'AE'
show(p: ptr) {
    s = ""
    s = p
    println(s)
}

main() { show(null) }
AE
refused plain_ptr 3:5 "string.from_ptr(p)"

cat > "$TMP/quiet.ae" <<'AE'
import std.map
import std.list
import std.string

struct Node { v: int }

main() {
    m = map.new()
    map.put(m, "k", "v")
    l = list.new()
    list.add(l, "a")
    list.add(l, string.concat("b", "c"))
    inner = list.new()
    list.add(l, inner)
    s = "x"
    s = null
    s = string.from_int(7)
    s, err = map.get_string(m, "k")
    t = ""
    t, err2 = list.get_string(l, 0)
    p = map_get_raw(m, "k")
    p = list_get_raw(l, 1)
    u = ""
    u, err3 = string.from_ptr(p)
    w, err4 = list.get_string(l, 2)
    z, err5 = string.from_ptr(list_get_raw(l, 2))
    n = heap.new(Node)
    q = n
    q = null as *Node
    println("${s} ${t} ${u} [${err}${err2}${err3}] ${w == null} ${err4} ${z == null} ${err5} ${q == null}")
    heap.free(n)
    map.free(m)
    list.free(inner)
    list.free(l)
}
AE
# `ae build` refuses it too, and writes no program.
out=$(cd "$TMP" && "$AE" build tuple_map.ae -o tuple_map_bin 2>&1)
if [ $? -eq 0 ] || [ -e "$TMP/tuple_map_bin" ] || ! printf '%s' "$out" | grep -q "error\[E0202\]"; then
    echo "  [FAIL] tuple_map: ae build did not refuse it with E0202"
    echo "$out" | sed 's/^/        /'
    fails=$((fails + 1))
fi

# The typed forms compile with no E0202 and run: each gives the string back,
# and says "not a string" for a list pointer.
compiles_and_runs() {
    name="$1"; want="$2"
    out=$(cd "$TMP" && "$AE" check "$name.ae" 2>&1)
    if [ $? -ne 0 ] || printf '%s' "$out" | grep -q -E "E0202|W1005"; then
        echo "  [FAIL] $name: refused, or reported E0202/W1005"
        echo "$out" | sed 's/^/        /'
        fails=$((fails + 1)); return
    fi
    got=$(cd "$TMP" && "$AE" run "$name.ae" 2>&1)
    if [ "$got" != "$want" ]; then
        echo "  [FAIL] $name: ran as"; echo "$got" | sed 's/^/        /'
        echo "      want"; echo "$want" | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}
compiles_and_runs quiet "v a bc [] true not a string true not a string true"

if [ "$fails" -ne 0 ]; then exit 1; fi
echo "  [PASS] ptr_into_string_error: a ptr given to a string local is refused (E0202) naming the typed accessor; null, string producers and the accessors compile and run"
