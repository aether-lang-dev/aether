- **std.tar reads a regular-file entry whose name ends in '/' as a
  directory (#3749).** That is the convention of the tars before typeflag
  '5', which GNU tar, libarchive and Python's tarfile keep. The reader
  called such an entry a file named "d/", while extract, which cleans the
  name, wrote a file at "d": the two disagreed about the same entry, and an
  entry under "d/" could not be extracted. `entry_kind` now gives
  `KIND_DIRECTORY` for it and extract makes the directory. Found by the tar
  fuzz target, through a GNU long name with a NUL in it.
