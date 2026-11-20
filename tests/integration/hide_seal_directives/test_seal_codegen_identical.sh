#!/bin/sh
# `hide` and `seal except` are compile-time only: a body that compiles both
# with and without one must compile to the same C (docs/hide-and-seal.md).
#
# One program, written with `/*SEAL a, b*/` and `/*HIDE a*/` marks on the
# line that opens a function's body, is built twice: once with every mark
# dropped, once with each mark made the directive it spells (on the same
# line, so line numbers agree). The two C files must be identical, and the
# sealed program must run right.
#
# Each shape here once compiled differently under a seal (0.923.0):
#   - a struct the list does not name lost its field types: a string field
#     compared by pointer, interpolated with %d, copied into an `int`
#     local the function then did not own; a closure stored in a field
#     unretained; a `using` field and a sum variant's field unresolved;
#   - a function on the list was counted as one taken as a value, which
#     marked its string returns (aether_fnptr_give);
#   - a local on the list was counted as used again, so its last use
#     copied the string instead of moving it;
#   - a constant or slice parameter on the list read as a binding of it, and
#     a slice parameter on the list as a use before the body moved it
#     (E0906 at the body's own move), an `Isolated` one as moved by the
#     list (E0200);
#   - a local bound from a call returning an unlisted struct was "of a type
#     the program does not define" (E0302);
#   - an arm's local on the list read as a use outside the arm, so it was
#     hoisted to the function's top.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/same_bytes.sh"
AE_TMP="$(mktemp -d)"
trap 'rm -rf "$AE_TMP"' EXIT

pass=0
fail=0

cat > "$AE_TMP/shapes.ae" << 'EOF'
import std.string

struct Browser {
    app_dir: string
    title: string
    hits: int
}

struct Holder { cb: ptr }

struct Entity { x: int  y: int }
struct Frog { using entity: Entity  hops: int }

struct Circle { r: float }
struct Rect { w: float  h: float }
type Shape = Circle | Rect

const LIMIT = 3

new_browser(dir: string) -> *Browser {
    b = heap.new(Browser)
    b.app_dir = dir
    b.title = "t"
    return b
}

greet(n: string) -> string { return "hi ${n}" }

is_home(b: *Browser) -> int { /*SEAL b*/
    if b.app_dir == "" { return 0 }
    if b.app_dir == "/home" { return 1 }
    return 2
}
hidden_type(b: *Browser) -> int { /*HIDE Browser*/
    if b.app_dir == "/home" { return 1 }
    return 0
}
label(b: *Browser) -> string { /*SEAL b*/
    return "[${b.hits}] ${b.title} (${b.app_dir})"
}
dir_of(p: string) -> string { /*SEAL p, new_browser*/
    b = new_browser(p)
    d = b.app_dir
    heap.free(b)
    return d
}
joined(b: *Browser) -> string { /*SEAL b, `string`*/
    s = string.concat(b.app_dir, "/x")
    return s
}
first_title(b: *Browser) -> string { /*SEAL b*/
    t = b.title
    if t == "" { return "none" }
    return t
}
set_cb(h: *Holder, c: @keep fn) { /*SEAL h, c*/
    h.cb = c
}
sum_xy(f: Frog) -> int { /*SEAL f*/
    return f.x + f.y
}
area(s: Shape) -> float { /*SEAL s*/
    let a: float = match s {
        Circle -> 3.0 * s.r * s.r
        Rect -> s.w * s.h
    }
    return a
}
hello(n: string) -> string { /*SEAL n, greet, msg, LIMIT*/
    msg = greet(n)
    if LIMIT > 5 { return "" }
    return msg
}
fresh(k: int) -> byte[] { return string.bytes("w${k}") }
arm_local(k: int, fill: bool) -> int { /*SEAL k, fill, fresh, extra*/
    if fill {
        extra = fresh(k)
        return extra.len
    }
    return k
}
new_title(t: string) -> Browser { return Browser { app_dir: "/", title: t, hits: 0 } }
title_len(t: string) -> int { /*SEAL t, new_title, `string`*/
    b = new_title(t)
    return string.length(b.title)
}
take_int(iso: Isolated[int]) -> int { /*SEAL iso, consume*/
    return consume(iso)
}
grown(xs: @keep int[]) -> int[] { /*SEAL xs, append*/
    return append(xs, 1)
}
total(xs: int[]) -> int { /*SEAL xs, i, t*/
    t = 0
    i = 0
    while i < xs.len { t = t + xs[i]; i = i + 1 }
    return t
}

expect(got: string, want: string) -> int {
    if got == want { return 0 }
    println("FAIL: got '${got}', want '${want}'")
    return 1
}

main() {
    b = new_browser("/home")
    var bad = 0
    bad = bad + expect("${is_home(b)} ${hidden_type(b)}", "1 1")
    bad = bad + expect(label(b), "[0] t (/home)")
    bad = bad + expect(dir_of("/opt"), "/opt")
    bad = bad + expect(joined(b), "/home/x")
    bad = bad + expect(first_title(b), "t")
    // set_cb is here for its C: a Holder that kept the closure would
    // need the program to release it.
    f = Frog { entity: Entity { x: 2, y: 3 }, hops: 1 }
    bad = bad + expect("${sum_xy(f)}", "5")
    let c: Shape = Circle { r: 2.0 }
    bad = bad + expect("${area(c) == 12.0}", "true")
    bad = bad + expect(hello("x"), "hi x")
    bad = bad + expect("${total([1, 2, 3])}", "6")
    bad = bad + expect("${arm_local(7, true)}", "2")
    bad = bad + expect("${title_len("abc")}", "3")
    let iso = isolate(41)
    bad = bad + expect("${take_int(iso)}", "41")
    heap.free(b)
    if bad == 0 { println("ok") }
}
EOF

mkdir -p "$AE_TMP/plain" "$AE_TMP/sealed"
sed -e 's|/\*SEAL [^*]*\*/||' -e 's|/\*HIDE [^*]*\*/||' \
    "$AE_TMP/shapes.ae" > "$AE_TMP/plain/shapes.ae"
sed -e 's|/\*SEAL \([^*]*\)\*/|seal except \1|' -e 's|/\*HIDE \([^*]*\)\*/|hide \1|' \
    "$AE_TMP/shapes.ae" > "$AE_TMP/sealed/shapes.ae"

marks=$(grep -c 'seal except\|{ hide ' "$AE_TMP/sealed/shapes.ae")
if [ "$marks" -ne 15 ]; then
    echo "  [FAIL] the sealed copy has $marks directives, not 15"
    fail=$((fail + 1))
fi

built=1
for v in plain sealed; do
    if ! (cd "$AE_TMP/$v" && "$ROOT/build/aetherc" shapes.ae shapes.c > build.log 2>&1); then
        echo "  [FAIL] the $v copy did not compile:"
        sed 's/^/      /' "$AE_TMP/$v/build.log" | head -8
        fail=$((fail + 1))
        built=0
    fi
done

if [ "$built" = 1 ]; then
    if same_bytes "$AE_TMP/plain/shapes.c" "$AE_TMP/sealed/shapes.c"; then
        echo "  [PASS] sealed and unsealed bodies compile to the same C"
        pass=$((pass + 1))
    else
        echo "  [FAIL] a seal or hide changed the generated C:"
        if command -v diff >/dev/null 2>&1; then
            diff "$AE_TMP/plain/shapes.c" "$AE_TMP/sealed/shapes.c" | cut -c1-200 | head -30 | sed 's/^/      /'
        fi
        fail=$((fail + 1))
    fi
fi

if (cd "$AE_TMP/sealed" && AETHER_HOME="" "$ROOT/build/ae" build shapes.ae -o shapes > run.log 2>&1) &&
   [ "$("$AE_TMP/sealed/shapes" | tr -d '\r')" = "ok" ]; then
    echo "  [PASS] the sealed program runs right"
    pass=$((pass + 1))
else
    echo "  [FAIL] the sealed program did not build or run right:"
    cat "$AE_TMP/sealed/run.log" | head -8 | sed 's/^/      /'
    "$AE_TMP/sealed/shapes" 2>&1 | head -8 | sed 's/^/      /'
    fail=$((fail + 1))
fi

echo ""
echo "Seal/hide codegen identity: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then exit 1; fi
