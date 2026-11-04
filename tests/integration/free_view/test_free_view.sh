#!/bin/sh
# #3072: a view owns nothing (R1, R2), and `free` of one the checker can see
# is E0200: a read-only view of a string's bytes (the issue's program), a
# sub-slice of a `make` buffer at an offset or at its start, an array viewed
# as a slice, an array `T[N]`, each as the expression or as a local bound
# once to it. They gave the allocator a pointer it never handed out, or one
# the buffer's slice still holds. The slice that holds a `make` buffer, a
# view of a raw pointer the program owns from its start, a local bound
# again, and a `@keep` slice parameter compile; a view given to a function
# is the run-time check's (tests/regression/test_issue3072_free_view.ae). A
# struct parameter's slice field compiles with W1007: its caller owns it, so
# the `free` gives back nothing (#3568).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] free_view: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# refused <name> <text> <line:col>: `ae check` refuses the program on stdin
# with an E0200 whose message holds <text>, at that position.
refused() {
    cat > "$TMP/$1.ae"
    out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" check "$1.ae" 2>&1)
    if [ $? -eq 0 ]; then
        echo "  [FAIL] $1: accepted"
        fails=$((fails + 1))
        return
    fi
    case "$out" in
        *"error[E0200]"*"$2"*"$1.ae:$3"*) ;;
        *) echo "  [FAIL] $1: not E0200 \"$2\" at $3"
           echo "$out" | sed 's/^/        /'
           fails=$((fails + 1)) ;;
    esac
}

# accepted <name>: `ae check` takes the program on stdin.
accepted() {
    cat > "$TMP/$1.ae"
    out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" check "$1.ae" 2>&1)
    if [ $? -ne 0 ]; then
        echo "  [FAIL] $1: refused"
        echo "$out" | sed 's/^/        /' | head -8
        fails=$((fails + 1))
    fi
}

view="it is a view of elements another slice or an array holds, which owns nothing"

# The issue's program.
refused issue_bytes "cannot free 'v': it is a read-only view of a string's bytes (string.bytes), which owns nothing" 6:5 <<'AE'
import std.string

main() {
    s = "hello"
    v = string.bytes(s)
    free(v)
    println("${s}")
}
AE

refused bytes_call "cannot free this value: it is a read-only view" 4:5 <<'AE'
import std.string

main() {
    free(string.bytes("abc"))
}
AE

refused sub_local "cannot free 's': $view" 4:5 <<'AE'
main() {
    b = make([]byte, 8)
    s = b[2..4]
    free(s)
    free(b)
}
AE

# The same under a program's own `extern free(p: ptr)` (#3450).
refused sub_local_extern_free "cannot free 's': $view" 6:5 <<'AE'
extern free(p: ptr)

main() {
    b = make([]byte, 8)
    s = b[2..4]
    free(s)
    free(b)
}
AE

refused sub_at_start "cannot free this value: $view" 3:5 <<'AE'
main() {
    b = make([]byte, 8)
    free(b[0..4])
    free(b)
}
AE

refused sub_from "cannot free 't': $view" 4:5 <<'AE'
main() {
    b = make([]byte, 8)
    t = b[1..]
    free(t)
    free(b)
}
AE

refused array_view "cannot free 's': $view" 4:5 <<'AE'
main() {
    arr = [1, 2, 3]
    int[] s = arr
    free(s)
}
AE

refused array_value "cannot free 'arr': an array \`int[3]\` lives in its function or its struct" 3:5 <<'AE'
main() {
    arr = [1, 2, 3]
    free(arr)
}
AE

accepted owners <<'AE'
extern malloc(n: int) -> ptr

drop(b: @keep byte[]) {
    free(b)
}

main() {
    b = make([]byte, 8)
    free(b)
    p = malloc(4)
    owned = (p as byte[])[0..4]
    free(owned)
    d = make([]byte, 4)
    drop(d)
    e = make([]byte, 2)
    e = make([]byte, 3)
    free(e)
}
AE

# #3568: `free` of a struct parameter's slice field gives back nothing (the
# caller owns the field and releases it), with W1007 where it is written.
warned() {
    cat > "$TMP/$1.ae"
    out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" check "$1.ae" 2>&1)
    if [ $? -ne 0 ]; then
        echo "  [FAIL] $1: refused"
        echo "$out" | sed 's/^/        /' | head -8
        fails=$((fails + 1))
        return
    fi
    case "$out" in
        *"warning[W1007]"*"$2"*"$1.ae:$3"*) ;;
        *) echo "  [FAIL] $1: not W1007 \"$2\" at $3"
           echo "$out" | sed 's/^/        /'
           fails=$((fails + 1)) ;;
    esac
}

warned param_field "this \`free\` gives back nothing: the slice is a field of a struct parameter" 6:5 <<'AE'
struct Box {
    items: int[]
}

drop_items(b: Box) {
    free(b.items)
}

main() {
    b = Box { items: make([]int, 3) }
    drop_items(b)
}
AE

if [ "$fails" -ne 0 ]; then exit 1; fi
echo "  [PASS] free_view: free of a view the checker can see is refused; owners are freed"
