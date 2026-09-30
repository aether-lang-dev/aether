- **First-class slices: `T[]` is now a fat pointer `{ ptr, len }` (#1286).** A
  `T[N]` array flows into a `T[]` parameter, binding, field or return with
  its length; `s.len` reads it; `s[i]` is bounds-checked (a runtime panic
  naming the line, index and length, catchable with `try`); `s[lo..hi]`,
  `s[lo..]`, `s[..hi]` and `s[..]` sub-slice without copying;
  `make([]T, n)` returns a bounded slice. A slice still decays to its
  pointer for a `ptr` parameter, a C extern, `free`, `null` comparison
  and pointer arithmetic, so the extern ABI is unchanged. A view over a
  bare pointer (`p as T[]`, an extern's `T[]` return) stays unbounded:
  unchecked, `.len == -1`, until bounded with `v[0..n]`.
