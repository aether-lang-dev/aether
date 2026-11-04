#!/bin/sh
# #3549: a slice cell (a slice local a closure assigns) owns its buffer only
# where nothing else can reach that buffer. A view of it that outlives the
# cell keeps the local left to the program, as before, so the view never
# reads freed memory.
#
# viewed() binds `w = v` and returns `w`, a view of the cell's buffer: the
# cell's release must not drop that buffer. given() returns `v` itself, its
# buffer handed to the caller while the closure keeps a loan of it. After
# each call the heap is churned with buffers of the same size filled with
# -7: a freed buffer would come back holding that filler. (Like slice_own's
# kept.ae, the viewed buffer leaks by design, so this is no regression test
# under the leak gate.)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=slice_cell_owns

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/cell.ae" <<'AE'
viewed(n: int) -> int[] {
    let v: int[] = make([]int, n)
    set = || {
        v = make([]int, n)
    }
    set()
    v[0] = 11
    v[1] = 22
    w = v
    return w
}

given(n: int) -> int[] {
    let v: int[] = make([]int, n)
    set = || {
        v = make([]int, n)
    }
    set()
    v[0] = 33
    v[1] = 44
    return v
}

churn() -> int {
    t = 0
    for r in 0..64 {
        b = make([]int, 4)
        for i in 0..4 {
            b[i] = -7
        }
        t = t + b[0]
    }
    return t
}

main() {
    w = viewed(4)
    churn()
    g = given(4)
    churn()
    println("cell ${w[0]} ${w[1]} ${g[0]} ${g[1]}")
    free(g)
}
AE
out=$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" run cell.ae 2>&1)
case "$out" in
    *"cell 11 22 33 44"*)
        echo "  [PASS] $NAME: a view of a cell's buffer keeps it; a returned cell gives it away" ;;
    *)
        echo "  [FAIL] $NAME: not \"cell 11 22 33 44\":"
        echo "$out" | head -10 | sed 's/^/        /'
        exit 1 ;;
esac
exit 0
