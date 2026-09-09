# `strbuilder.finish()` returns a header-less `char*`, so scanning its result char-by-char is O(n²)

**From:** the LangArena port (2026-09-09) · **Where it bit:** porting all 50
LangArena benchmarks to Aether. Every checksum was correct on the small
`test`-tier inputs, then three benchmarks (Calculator, Words, the Template
pair) *timed out* on the production-tier inputs. Root cause was the same in
all three.

## Symptom

A benchmark builds a large text with `std.strbuilder`, finishes it to a
`string`, then scans it left-to-right with `string.char_at` / `string.substring`
(a hand-written tokenizer / interpreter / template renderer). At small n it's
instant; at large n it's quadratic. Measured, Calculator's parser over a
generated program (`string.char_at(text, pos)` per advance +
`string.substring(text, start, pos)` per token):

```
operations=1000  0.21s
operations=2000  0.87s     (~4x)
operations=4000  2.07s
operations=8000  7.85s     (~4x per doubling  ⇒  O(n²))
```

At the production input (operations≈11000 × 100 iterations) it never finished
inside 280s. Switching *only* the accessors to the length-carrying variants
(`string.string_char_at_n(text, len, i)` and
`string.string_substring_n(text, len, a, b)`, with `len` cached once) made it
linear:

```
operations=4000  0.03s
operations=8000  0.065s
operations=16000 0.077s     (linear)
```

and the full run dropped from >280s (timeout) to <1s.

## Why it happens

`aether_strbuilder_finish()` deliberately returns a **plain `char*` with no
`AetherString` header** (the comment in `std/strbuilder/aether_strbuilder.c`
explains why: the heap-tracker's reassignment shim emits a libc `free` on the
previous value, which on a real `AetherString*` would free the 24-byte header
and dangle the data buffer). Correct for the free contract — but it means
`str_len()` (`std/string/aether_string.c`) takes its `strlen()` fallback
branch, not its O(1) `->length` branch, on that string:

```c
static inline size_t str_len(const void* s) {
    if (is_aether_string(s)) return ((const AetherString*)s)->length;
    return strlen((const char*)s);              // finish()'d strings land here
}
```

`string_char_at`, `string_substring`, `string_length`, and anything else that
calls `str_len` therefore pays a full `strlen` **every call**. A char-by-char
scan of the finished string is `O(n)` calls × `O(n)` strlen = `O(n²)`. This is
exactly the `__strlen_avx2`-dominated profile the `string_char_at_n` header
comment already mentions from avn — but it reproduces trivially from
`strbuilder.finish` + a scan, with no binary/NUL content involved.

## What's actually wrong

Not the free contract, and not the accessors — it's that **the footgun is
invisible and undocumented as a *performance* cliff.** The `finish()` comment
documents the NUL-truncation consequence ("downstream operations that use
strlen will truncate at the first NUL") and names `string_length_n` /
`string_substring_n` as the escape for *binary* content. It says nothing about
the fact that an all-ASCII builder output — "JSON, log lines, templates,
paths", the exact cases the comment calls the "overwhelming majority" — is
`O(n²)` to scan through the ordinary `string.char_at` / `substring` API. A
porter reaches for `char_at`/`substring` (the documented, obvious API), gets
correct results on their test inputs, and only discovers the cliff when a
production-sized input hangs.

## Repro

```aether
import std.strbuilder
import std.string
main() {
    b = strbuilder.new(0)
    i = 0
    while i < 400000 { strbuilder.append(b, "x") i = i + 1 }   // ~400 KB, no NULs
    s = strbuilder.finish(b)
    n = string.length(s)
    sum = 0
    j = 0
    while j < n { sum = sum + string.char_at(s, j) j = j + 1 } // O(n²): strlen per char
    println(sum)
}
```

That loop is quadratic; replacing `string.char_at(s, j)` with
`string.string_char_at_n(s, n, j)` makes it linear. (`n` is even wrong-ish to
pass to a function that will re-derive it — the point is char_at doesn't take
it.)

## Suggestions (any one helps; roughly increasing effort)

1. **Docs, cheapest:** add the performance consequence to the `finish()`
   comment and to `std/strbuilder/README.md` / the string module docs — "the
   returned buffer has no length header; scanning it with `char_at` /
   `substring` is O(n²), use the `_n` accessors with a cached length." Right
   now the only warning is about NUL truncation.
2. **A length-returning finish as the ergonomic default:**
   `finish_with_length` already exists and returns `(ptr, int)` — but the
   idiomatic `finish` → `string` is the one people reach for. Consider making
   the docs steer scanning-heavy code to `finish_with_length` + the `_n`
   accessors as the *normal* path, not an escape hatch.
3. **Bigger, if worth it:** let `finish()` hand back a real length-bearing
   `AetherString*` that the heap-tracker frees correctly (the comment says the
   uniform-heap return shim can't currently piggy-back its struct-aware path
   for heap-flagged returns — if that shim could carry `is_struct`, `finish`
   could return an O(1)-length string and the footgun disappears at the
   source). This is the "make the fast path the default" fix.

## Impact / workaround

Fully worked around in the port (cached `len` + `string.string_char_at_n` /
`string_substring_n` in every hot scan, and `checksum_str` in the harness got
the same treatment). Nothing is blocked. Filing because it's a silent
correctness-preserving perf trap: the natural API on the natural output of the
natural builder is quadratic, and the docs only flag the NUL angle. A one-line
perf note on `finish()` would have saved the whole investigation.
