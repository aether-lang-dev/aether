- **`zip.extract` no longer writes through a symlink or junction in the
  destination.** It promised to refuse a parent directory that already exists
  as a symlink but never checked, so with `dest/link` a junction (`mklink /J`,
  no privilege needed) or symlink to another directory, the entry
  `link/pwned.txt` was written there. It now checks every parent of the entry
  with the same helper `tar.extract` uses, and both modules write each entry
  at its cleaned name. A raw name such as `missing/../link/x` could otherwise
  pass the check before `mkdir_p` created `missing`, and then be written
  through `link`. An existing file or symlink at an entry's path is removed
  before an overwrite instead of being written through.
- **`tar.extract` measures a symlink's depth on its cleaned name.** The escape
  check counted every `/` in the entry name as one level, `.`, `..` and the
  empty segments of `//` included. So with `allow_symlinks` on, `./l ->
  ../escape`, `a/../l -> ../../escape` and `a//l -> ../../escape` all passed.
  A target may also no longer use `..` after a name segment (`r/..`). That
  name can be another link from the same archive, and the OS climbs from
  where the link points, not lexically: `r -> .` makes `r/..` the
  destination's parent.
- **A zip entry can no longer be bigger than `entry_size` says.** A stored
  entry's central-directory size was never compared with its data, so a
  100-byte entry claiming 1 byte was read whole and extracted past
  `max_entry_bytes = 10`. A stored entry whose two sizes differ is now an
  error. ZIP64 sizes and offsets at or past 2^63 refuse the archive, and ones
  past the buffer are an error rather than being narrowed to a wrong in-range
  offset.
- **An archive comment containing `PK\5\6` no longer hides a zip's entries.**
  `zip.open` took the last end-of-central-directory signature in the file
  without checking that the record's comment reached the end of the buffer.
  So a comment holding the signature opened as an empty archive with no
  error. The record whose comment ends the file is preferred now; if bytes
  were appended after the record and none does, the latest one that fits is
  used, as Python's zipfile reads such archives.
- **`std.zip` and `std.tar` can be imported together.** Their
  `ExtractOptions` structs differed, which is a compile error once both are
  imported. zip's now has tar's fields, so the two are one type and either
  module's options work with the other's `extract`. `zip.extract` refuses
  `preserve_mode` and `preserve_mtime` rather than ignore them, and never
  creates symlinks, so `allow_symlinks` has nothing to allow there.
