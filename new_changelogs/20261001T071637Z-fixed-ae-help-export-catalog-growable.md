- **`ae help`'s export catalog no longer silently truncates.** The
  missing-import and "Did you mean" suggestions draw on a catalog of every
  `exports (...)` name in `std/` and the `--lib` directories, which was a
  fixed 2048-entry array filled in `readdir` order. `std` alone is past
  2,300 exports, so whatever the filesystem listed last was dropped: on NTFS
  (alphabetical) that was the tail of `std.string` and all of `std.zstd`,
  while ext4's hash order happened to keep them — which is why adding five
  exports to modules sorted before `string` made `ae help` stop suggesting
  `import std.string` for an unimported `string_length` on Windows only. The
  catalog now grows by doubling and has no cap; the `ae_help` integration
  test gains a case for the alphabetically last std module, the earliest
  casualty of any such limit under any ordering.
