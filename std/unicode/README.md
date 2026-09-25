# std.unicode

Unicode-correct text operations, over the vendored
[utf8proc](https://github.com/JuliaStrings/utf8proc) (v2.9.0, MIT + Unicode
data license), in `std/unicode/utf8proc/`.

`std.string` is **byte-indexed**: its `length` and `substring` count bytes, so
they cut multi-byte characters and combining sequences. `std.unicode` is
codepoint- and grapheme-correct, for the jobs that need it — accent-insensitive
search, caseless compare, and slicing user-perceived characters.

```aether,fragment
import std.unicode

unicode.fold_accents("café")           // "cafe"  (strip combining marks)
unicode.casefold("STRASSE")            // "strasse"  (ß folds to ss too)
unicode.nfc(s)  /  unicode.nfd(s)      // normalize (composed / decomposed)
unicode.grapheme_len("é")              // 1, even when é is e + U+0301
unicode.grapheme_substring("café", 3, 1)   // the é, whole — never split

// Caseless / accent-insensitive comparison:
unicode.equals_ignore_case("Straße", "STRASSE")   // 1
unicode.equals_fold_accents("Café", "cafe")       // 1
```

## What it covers

- **Normalization** — `nfc` (composed) and `nfd` (decomposed), stable across
  utf8proc versions.
- **Accent folding** — `fold_accents` decomposes and strips combining marks, so
  `"café"` → `"cafe"` (result is in decomposed form; for accent-free text that
  is the same string).
- **Case folding** — `casefold` for caseless matching. Not the same as
  lowercasing: it maps e.g. German `ß` → `"ss"`.
- **Grapheme clusters** — `grapheme_len` and `grapheme_substring` count and
  slice *user-perceived characters*, never splitting a combining sequence or an
  emoji ZWJ sequence.
- **Comparisons** — `equals_ignore_case` (fold case) and `equals_fold_accents`
  (fold accents *and* case), the common "does this search term match" needs.

All string returns are owned by the caller.

## Vendored dependency

utf8proc lives in `std/unicode/utf8proc/` and is compiled into `libaether`, so
`std.unicode` is always available — no `--extra` link needed. `utf8proc.c`
`#include`s `utf8proc_data.c` (the generated Unicode tables), so only
`utf8proc.c` is a translation unit. `contrib.i18n.collate` shares this same
vendored copy.

## Exports

`nfc`, `nfd`, `fold_accents`, `casefold`, `grapheme_len`, `grapheme_substring`,
`equals_ignore_case`, `equals_fold_accents`.
