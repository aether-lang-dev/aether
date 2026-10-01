- **The rest of #2301's Foundations table: std.lanes, std.string and
  std.collections get slice forms, and std.mem/std.strbuilder's two
  remaining gaps close (#2301).** `std.lanes` gains `load4_slice`/
  `store4_slice`/`load2_slice`/`store2_slice`/`i32load_slice`/
  `i32store_slice`/`i16load_slice`/`i16store_slice`/`pack_i16_u8_slice` —
  typed-slice wrappers over the existing raw-pointer loads/stores that check
  the WHOLE vector width fits before decaying to a pointer, not just that an
  extern parameter is typed `T[]`. `std.string` gains `bytes(s)` (a bounded
  read-only `byte[]` view of a string's own bytes, for binary/ASCII scanning
  without `char_at_n`'s separate length), `array_view(arr)` (an owned
  `string[]` snapshot of a split result, sidestepping `array_get`'s borrowed-
  pointer lifetime hazard), and `seq_from_strings`/`seq_to_strings` (bridges
  between a plain `string[]` and a `*StringSeq`, for a runtime slice the
  compiler's literal-to-seq lowering doesn't reach). `std.collections` gains
  `intarr_view`, delegating to `std.intarr.array()`'s already-bounded view so
  a caller using only the compatibility facade doesn't need a second import.
  `std.mem` gains `compare_slice` (lexicographic, shorter-is-less on a shared
  prefix). `std.strbuilder.finish_with_length` now returns a bounded `byte[]`
  instead of a `(ptr, int)` tuple — `free()` on the result decays to the same
  raw pointer it always returned, so every call site becomes one line
  shorter and `.len` replaces the second return value. Backward compatibility
  was explicitly not a goal for this change: all four existing call sites
  were updated rather than kept working via an overload.
