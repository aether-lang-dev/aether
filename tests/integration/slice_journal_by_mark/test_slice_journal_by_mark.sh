#!/bin/sh
# #3787: a slice place is journaled for a caught panic by its value's own
# mark. Where the frame's body shows every write to a slice local (its
# binding, the stores slice ownership writes), the local is journaled when
# its value holds a reference (aether_slice_journal reads
# AETHER_SLICE_MADE and AETHER_SLICE_STRING), and the store that brings
# the first counted value journals it then (aether_slice_set_journal). A
# call's slice read in place, passed straight to a call, a loop's slice and
# a module `var`'s read are journaled the same way. Every exit forgets
# only what was journaled (aether_unwind_forget_tok, from the token
# aether_unwind_track returns), so a view of memory no count covers that a
# function returns costs no journal call at its caller, where it cost an
# aether_unwind_track and an aether_unwind_forget, and a `make` buffer
# outside every `try` costs one call that writes nothing.
#
# A local the frame may change unseen keeps its journal entry from its
# declaration on: one grown in place (`push`), one whose address is taken,
# and one stored into inside a `try` it is declared outside of, where a
# journal entry would belong to the `try`'s frame.
#
# What runs is tests/regression/test_slice_journal_by_mark.ae's.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

[ -x "$AETHERC" ] || [ -x "$AETHERC.exe" ] ||
    { echo "  [SKIP] slice_journal_by_mark: aetherc not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

cat > "$TMP/views.ae" <<'AE'
struct Hull {
    data: ptr
    count: int
}

var gs: int[] = []

faces(h: *Hull) -> int[] {
    return h.data as int[]
}

fresh(n: int) -> int[] {
    return make([]int, n)
}

same(xs: int[]) -> int[] {
    return xs
}

total(xs: int[]) -> int {
    return xs.len
}

use_bound(h: *Hull) -> int {
    f = faces(h)
    return f[0]
}

use_fresh(n: int) -> int {
    f = fresh(n)
    return f.len
}

use_copy(xs: int[]) -> int {
    c = same(xs)
    return c.len
}

use_replaced(h: *Hull, k: int) -> int {
    v = faces(h)
    if k > 0 {
        v = fresh(3)
    }
    return v.len
}

use_in_place(h: *Hull) -> int {
    return faces(h)[0] + fresh(2).len
}

use_passed(h: *Hull) -> int {
    return total(faces(h)) + total(fresh(3))
}

use_loop(h: *Hull) -> int {
    t = 0
    for x in faces(h)[0..2] {
        t = t + x
    }
    return t
}

use_module_var() -> int {
    return gs.len + 1
}

kept_pushed() -> int {
    int[] xs = []
    xs.push(1)
    return xs.len
}

kept_stored_in_try(h: *Hull) -> int {
    v = faces(h)
    try {
        v = fresh(2)
    } catch e {
        println(e)
    }
    return v.len
}

main() {
}
AE

if ! (cd "$TMP" && "$AETHERC" views.ae views.c > out.txt 2>&1); then
    echo "  [FAIL] slice_journal_by_mark: the program did not compile"
    sed 's/^/        /' "$TMP/out.txt"
    exit 1
fi

# body <name>: the generated definition of function <name>.
body() {
    awk -v name="$1" '
        $0 ~ "^[A-Za-z_][A-Za-z0-9_ *]* " name "\\(.*\\) \\{$" { on = 1 }
        on { print }
        on && /^}/ { exit }
    ' "$TMP/views.c"
}

# by_mark <name> <form>...: every slice place of <name> is journaled by its
# value's mark (each <form> is in the body), and nothing is journaled or
# forgotten unconditionally.
by_mark() {
    name="$1"
    shift
    b=$(body "$name")
    if [ -z "$b" ]; then
        echo "  [FAIL] $name: no definition in the generated C"
        fails=$((fails + 1))
        return
    fi
    for form in "$@"; do
        case "$b" in
            *"$form"*) ;;
            *) echo "  [FAIL] $name: no $form"
               fails=$((fails + 1)) ;;
        esac
    done
    if printf '%s\n' "$b" | grep -q -E "aether_unwind_track\(|aether_unwind_forget\("; then
        echo "  [FAIL] $name: a slice place is journaled or forgotten whatever its value"
        printf '%s\n' "$b" | grep -n -E "aether_unwind_track\(|aether_unwind_forget\(" | sed 's/^/        /'
        fails=$((fails + 1))
    fi
}

# from_declaration <name>: the local is journaled from its declaration on,
# and its exits forget it by the token the track returned.
from_declaration() {
    b=$(body "$1")
    case "$b" in
        *"= aether_unwind_track((const void*)_ae_dfc"*) ;;
        *) echo "  [FAIL] $1: a place the frame may change unseen is not journaled from its declaration"
           fails=$((fails + 1)) ;;
    esac
    case "$b" in
        *aether_slice_set_journal*)
            echo "  [FAIL] $1: a store journals a place already journaled"
            fails=$((fails + 1)) ;;
    esac
}

by_mark use_bound "aether_slice_journal(" "aether_unwind_forget_tok("
by_mark use_fresh "aether_slice_journal(" "aether_unwind_forget_tok("
by_mark use_copy "aether_slice_journal(" "aether_unwind_forget_tok("
by_mark use_replaced "aether_slice_journal(" "aether_slice_set_journal(" "aether_unwind_forget_tok("
by_mark use_in_place "aether_slice_journal(" "aether_unwind_forget_tok("
by_mark use_passed "aether_slice_journal(" "aether_unwind_forget_tok("
by_mark use_loop "aether_slice_journal(" "aether_unwind_forget_tok("
by_mark use_module_var "aether_slice_journal(" "aether_unwind_forget_tok("
from_declaration kept_pushed
from_declaration kept_stored_in_try

# No defer's exit forgets an entry its registration may not have written.
if grep -n "aether_unwind_forget((const void\*)_ae_dfc" "$TMP/views.c" > "$TMP/bare.txt"; then
    echo "  [FAIL] a defer's exit forgets without its token"
    sed 's/^/        /' "$TMP/bare.txt"
    fails=$((fails + 1))
fi

if [ "$fails" -eq 0 ]; then
    echo "  [PASS] slice_journal_by_mark: slice places are journaled by their value's mark, and forgotten only when journaled"
fi
[ "$fails" -eq 0 ]
