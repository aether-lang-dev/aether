# Feature request: `[]` index sugar for `std.intarr` / `floatarr` / `longarr` (and a clean diagnostic when it's missing)

**From:** the LangArena port (2026-09-09) · **Where it bit:** the packed
numeric buffers (`std.intarr` etc.) are the workhorse of every array-heavy
benchmark — sieve, sort, matmul, graph, BWT, the AST arena. Every element
access is a function call.

## The ask

Allow `a[i]` / `a[i] = v` on an `intarr` / `floatarr` / `longarr` handle as
sugar for `intarr.intarr_get_unchecked(a, i)` / `intarr.intarr_set_unchecked(a, i, v)`
(pick the checked or unchecked lowering — either is a win over the status quo).
These types are used *exactly* like arrays; the call-site verbosity is
substantial:

```aether
// today
intarr.intarr_set_unchecked(sa, pos, intarr.intarr_get_unchecked(input, i))
intarr.intarr_set_unchecked(perm, a, intarr.intarr_get_unchecked(perm, b))

// wished
sa[pos] = input[i]
perm[a] = perm[b]
```

A tight inner loop (matmul, the BWT suffix sort, the arithmetic coder) reads
far better with brackets, and it's the shape porters expect coming from C /
Go / Rust where these are `arr[i]`.

## Secondary bug: the current failure is a raw C error, not an Aether diagnostic

When you *do* write `a[i]` on an `intarr` handle today, you don't get a clean
"indexing not supported on this type" — you get a C compiler error leaking
through:

```
$ ae run t.ae
    5 |     x = a[0]
      |          ^
/tmp/t.ae:5:9: error: void value not ignored as it ought to be
    5 |     x = a[0]
      |         ^
Build failed.
```

Repro:

```aether
import std.intarr
main() {
    a = intarr.intarr_new_raw(4)
    intarr.intarr_set_unchecked(a, 0, 7)
    x = a[0]            // <- C error "void value not ignored as it ought to be"
    println("${x}")
}
```

`a` is a `ptr`, so `a[0]` presumably lowers to a C pointer-index on a `void*`
and the type checker never catches it. Even if index sugar for these types
isn't wanted, a front-end diagnostic ("`[]` indexing is not defined for `ptr`;
use `intarr.intarr_get_unchecked(a, i)`") would beat a leaked C error — this is
the kind of thing a first-time porter hits and can't decode.

## Impact / workaround

No blockage — the function forms work fine and the port uses them throughout.
This is pure ergonomics + a diagnostic-quality nit. Filed together because they
share a root (index syntax on the packed-buffer handle type); the diagnostic
half is worth doing regardless of whether the sugar lands.
