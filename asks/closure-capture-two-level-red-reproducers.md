# Red reproducers for the closure-capture bug (#2189 family), two of them

**From:** the OpenDisk-ae port (macOS arm64, aether 0.717.0 @ e7f9562),
2026-09-25. **For:** whoever fixes #2189. This answers the ask for "a minimal
red .ae": there are two, both small, both red on 0.717.0, and the second one
is a silent **heap-use-after-free**, not a compile error.

## Where this came from

OpenDisk-ae's `opendisk.ae` hit `use of undeclared identifier 'nm'` for real
(an `each` render closure containing a button callback that bound `nm`, and
`main` binding its own `nm` later). Restoring that code still fails on
0.717.0; reducing it by knocking out one ingredient at a time gave the files
below. aether-ui is not needed: they are plain Aether.

## Red 1: compile failure — undeclared identifier (15 lines)

```aether
outer(render: fn) { call(render, "item") }
inner(on_click: fn) { call(on_click) }
use(s: string) { println(s) }

main() {
    start = "x"
    outer(|item: string| {
        inner(|| {
            nm = item
            use(nm)
        })
    })
    nm = start
    use(nm)
}
```

`aetherc` accepts it; the C fails: `error: use of undeclared identifier 'nm'`.
Same with the `outer() callback |item: string| { inner() callback { … } }`
spelling.

What decides red vs green (each verified on 0.717.0):

| Variant | Result |
|---|---|
| as above: closure **two** levels deep binds `nm`; `main` binds `nm` **after** it, **same block level** | RED |
| only one closure level (`nm = item` directly in the outer closure) | green |
| `main` binds `nm` **before** the closures | green (but see Red 2) |
| `main`'s later `nm` is inside an `if` block | green |
| closures wrapped in a trailing block / builder block / `frame() { … }` | still RED |
| no later `nm` in `main` at all | green |

That table is why the earlier reductions missed it: shape 1 there ("later
main-level `nm` in an `if`") is exactly the green row. The later binding has
to be at the same block level as the closure statement.

Emitted C (0.717.0): the outer closure's maker is
`_aether_make_closure_0(const char** nm)`, the call site is
`_aether_make_closure_0(nm)`, and `main`'s `nm` is promoted to a cell
(`const char** nm = _aether_cell_new(...)`) **six lines later**. The outer
closure's capture collection claims the inner closure's fresh local as a
capture of `main`'s later binding, and that claim is what promotes `main`'s
`nm` to a cell in the first place.

## Red 2: runtime — heap-use-after-free, no diagnostic (13 lines)

```aether
outer(render: fn) { call(render, "from-closure") }
inner(on_click: fn) { call(on_click) }

main() {
    nm = "from-main"
    outer(|item: string| {
        inner(|| {
            nm = item
            println("inner nm=${nm}")
        })
    })
    println("main nm=${nm}")
}
```

Expected (closures capture by reference; the one-level version prints this):

```
inner nm=from-closure
main nm=from-closure
```

Actual on 0.717.0: `main nm=` followed by garbage bytes. Under
`-fsanitize=address`:

```
ERROR: AddressSanitizer: heap-use-after-free ... READ of size 2
    #2 main m7.ae:12
freed by thread T0 here:
    #2 _closure_env_1_free
    #3 _closure_fn_0 m7.ae:13
    #4 outer m7.ae:2
allocated by:
    #2 string_new
    #3 aether_str_capture
```

So when the write-through to a captured variable crosses two closure levels,
the string stored into `main`'s cell is a copy owned by the **inner** closure's
environment (`aether_str_capture`), freed with that environment, and `main`
then reads freed memory. At one level (`nm = item` directly in the outer
closure) the same program is correct: `main nm=from-closure`, ASan clean.

Whether Red 2 and Red 1 are one bug or two: they share the ingredient (a
binding two closures deep, a same-named binding in `main`), and in Red 2 the
inner binding *should* be a write to `main`'s `nm`. So "is `nm` in the inner
closure a fresh local or the enclosing variable?" is decided by declaration
order, and both answers currently emit wrong C. That points at one place,
the two-level capture/promotion path in `codegen_expr.c`
(`first_occurrence_kind` and the outer closure's capture collection), but
fix and guard them as two tests.

## Suggested guards

- `tests/regression/test_closure_nested_fresh_local_later_outer.ae`: Red 1
  as is; must compile and print `item` (the inner closure's own `nm`) then
  `x` (`main`'s).
- `tests/regression/test_closure_nested_write_through_string.ae`: Red 2; must
  print `main nm=from-closure`. Worth a `make test-asan`-style run too, since
  without ASan the garbage can happen to look like a string.
- Keep the green rows of the table as passing cases in one of them, so a fix
  cannot trade one shape for another.

Workaround used in OpenDisk-ae meanwhile: don't reuse the name (it inlines
`d.name` instead of binding `nm`).
