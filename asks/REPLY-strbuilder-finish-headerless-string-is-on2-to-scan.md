# REPLY: `finish` now returns a length-carrying string; the scan is linear

**Re:** `asks/strbuilder-finish-headerless-string-is-on2-to-scan.md`

Option 3, not 1 or 2: the footgun is removed rather than documented.

The builder writes into one block laid out as an inline `AetherString` —
`[header][payload]` — from its first reserve, and `finish` fills in the
header and returns the block. No copy, no second allocation, and the
result carries its length, so `string.length` / `char_at` / `substring` are
O(1) on it and the ask's repro is O(n):

```
scan of a 400 000-byte finished string, string.char_at per byte
  before   4818 ms
  after      42 ms
```

The ownership reason the old comment gave for returning a header-less
`char*` no longer holds: the codegen's heap tracker frees through
`aether_heap_str_free`, which dispatches on the magic header, and
`string_release` recognises the inline layout by position and frees the
block whole. `std.bytes.finish` has handed off an `AetherString` the same
way for some time.

`finish_with_length` keeps its contract (raw bytes, no terminator, caller
frees with `free()`): it shifts the payload down over the header with one
`memmove`, no allocation.

A side effect worth having: content with embedded NUL bytes appended via
`append_n` now survives `finish` in full, since every string-aware
consumer reads `length` rather than scanning for the terminator.

Test: `tests/regression/test_strbuilder_finish_length.ae`.
