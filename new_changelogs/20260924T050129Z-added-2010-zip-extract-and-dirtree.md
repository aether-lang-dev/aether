- **`std.zip` extracts to disk, safely by default, and packs a directory tree
  (#2010 follow-ups).** `extract(data, len, dest, ExtractOptions)` unpacks an
  archive buffer under a destination (`extract_file` reads the `.zip` off disk
  first); every entry is decompressed and CRC-checked by the reader before it
  is written. It reuses `std.tar`'s extraction guards, sharpened for ZIP's
  higher deflate ratios: absolute paths, `..` traversal, and backslash/colon
  tricks that become absolute or drive-qualified on Windows are refused, and
  `max_entries` / `max_entry_bytes` / `max_total_bytes` cap a zip bomb;
  `overwrite` defaults off. `add_dir_tree(w, fs_root, arc_prefix, method,
  level)` walks a directory and adds every file under it, named relative to
  `fs_root`. `std/zip/test_zip.ae` gains a filesystem section: a dir-tree ->
  extract round-trip that diffs identical, `extract_file`, and traversal /
  absolute-path rejection with nothing written outside the destination.
