- **A slice's release is inline again for a view (#3853).** The release a
  slice gets where it dies (a local's scope exit, a store, an unwind) is
  `static inline`, but it carried the string release, the buffer's free and
  its element drops, so the compiler left the whole release out of line: a
  call per place for what is, for a view (`ptr as T[]`, a slice of a
  slice), one test of its marks. The counted path that frees or touches a
  published count now sits in a cold helper; the uncounted test and the
  common counted case (a reference that is not the last) stay inline.
  bench-core on an Apple M5 Pro: `slice_view_returned` 0.86 to 0.22 ns,
  `slice_view_bound` 3.34 to 3.15 ns; the ownership rows unchanged.
