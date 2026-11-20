- **std.tar: reading an entry to its end reports its end, not an error
  (#3749).** `entry_read` answered "not positioned on an entry" once a file
  entry had been read whole, so a loop reading to length 0 failed after the
  last chunk of every file. It answers `("", 0, "")` there, and still refuses
  a read before any entry or past the archive's end. Found by the new tar
  fuzz target.
