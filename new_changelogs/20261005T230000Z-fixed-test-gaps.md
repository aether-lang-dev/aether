- **`std.intmap`'s iteration test checks that every key is visited once.**
  It compared only the number of slots visited against `size()`, so a walk
  that visited one key twice and skipped another passed. It now marks each
  visited key and fails on a repeat. The old test passed against an iterator
  mutated that way; the new one fails on it.
- **The YAML conformance suite fails when its corpus walk fails.** With the
  corpus checked out, an `fs.walk` error was dropped, so a walk that stopped
  part-way passed on whatever it had read, and one that found no cases passed
  on zero. CI does not carry the corpus, so this affects local runs only.
- **`std.list`'s test checks the error for a negative index, as well as the
  value.** The pinned out-of-range behaviour (#2439) was half-asserted for
  negative indices.
- **Correction to 0.781.0's #2432 entry.** It said fixed-size arrays were
  handled "nested ones included". The type grammar takes a single `[N]`
  suffix, so a nested fixed array such as `Cell[2][3]` does not parse. The fix
  covers fixed-size array fields of one dimension, the only kind that can be
  written.
