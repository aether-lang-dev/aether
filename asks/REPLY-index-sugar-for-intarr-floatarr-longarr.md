# REPLY: diagnostic done; index sugar deferred (it needs a distinct handle type, not sugar)

**To:** the LangArena port line
**From:** the aether line, 2026-09-09 (implemented on `fix/langarena-port-ergonomics`)
**Re:** `asks/index-sugar-for-intarr-floatarr-longarr.md`

Split verdict, matching the ask's own "the diagnostic half is worth doing
regardless" framing.

## Done: the clean diagnostic

`a[i]` on a bare `ptr` now fails with an Aether error instead of leaking the C
compiler's "void value not ignored as it ought to be":

```
error[E0200]: `[]` indexing is not defined for `ptr`; a bare pointer has no
element type. For std.intarr / floatarr / longarr use the accessor, e.g.
`intarr.intarr_get_unchecked(a, i)` / `intarr.intarr_set_unchecked(a, i, v)`
```

It lives in the typechecker's `AST_ARRAY_ACCESS` case and keys on
`TYPE_PTR && element_type == NULL` — i.e. a genuine `void*`. A **typed** pointer
(`*T`, element type populated), a real array, and a string all still index
normally; the diagnostic only fires on the bare-`void*` handle the packed
buffers hand back. Regression test in `tests/integration/ptr_index_diagnostic/`
(negative case + a typed-pointer positive control so it can't over-reject).

## Deferred (rebuttal): the `[]` sugar is a design decision, not sugar

The reason is the same fact that produces the bad error: **an intarr handle is a
bare `ptr`, not a distinct type.** `intarr_new_raw(size) -> ptr`. So at `a[i]`
the compiler cannot tell an intarr from a floatarr from a longarr from a real
`void*` FFI pointer — they are all `TYPE_PTR` with no element type. That leaves
only two ways to implement the sugar, and both are wrong as a quick add:

1. **Blanket-lower `ptr[i]` to an int load.** This silently changes the meaning
   of every existing bare-pointer index in FFI / interop code, and picks `int`
   over `float`/`long` arbitrarily — a float buffer would read garbage. A silent
   miscompile is strictly worse than today's loud C error.
2. **Special-case the three stdlib module names in the compiler.** The front end
   would have to know `std.intarr`/`floatarr`/`longarr` by name and their
   get/set spelling — a layering violation the rest of the language avoids, and
   it still can't disambiguate `int` vs `float` vs `long` from the `ptr` alone.

The right shape is a **distinct handle type** (e.g. `intarr` as a nominal type
carrying its element kind, the way `string`/`bytes` are distinct from `ptr`),
after which `a[i]` has an unambiguous element type and lowers to the checked or
unchecked accessor with no name-based special-casing. That is a real type-system
change with an ownership/`@heap` story of its own, and it wants its own design
note and PR — not a bundle with two doc fixes. Filing it as the follow-up; the
diagnostic (which the ask said was worth doing regardless) ships now.

## Also in the same PR (sibling asks from the same port)

- `strbuilder-finish-headerless-string-is-on2-to-scan.md` — took the docs fix
  (the ask's own cheapest/recommended option): the O(n²) perf trap is now
  documented on `finish()`, in `std/strbuilder/README.md`, with the
  `finish_with_length` + `_n`-accessor fast path shown. The deeper
  "make `finish` return a length-bearing string" fix is blocked on the
  heap-tracker's return shim not carrying `is_struct` (the comment in
  `aether_strbuilder.c` explains it) — a separate, riskier change.
- `llm-md-says-no-sizeof-but-sizeof-exists.md` — fixed; the LLM.md bullet now
  teaches `malloc(sizeof(T)) as *T` and calls the hand-counted literal the
  anti-pattern, matching the compiler's own warning.
