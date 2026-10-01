- **`std.bytes.cursor`, `std.mem` and `std.strbuilder` take `byte[]`
  slices.** `cursor.from_slice(s)` walks any bounded byte slice, and
  `cursor.read_view(c, n)` returns the next bytes as a borrowed sub-slice
  without copying. `mem.read_u16_le` through `mem.write_u64_be`, `copy_slice`
  and `fill_slice` check that the whole field lies inside the slice (a panic
  naming offset, width and length, where the pointer forms read past the
  buffer). `strbuilder.append_slice` appends a slice's bytes as they are
  (#2301).
