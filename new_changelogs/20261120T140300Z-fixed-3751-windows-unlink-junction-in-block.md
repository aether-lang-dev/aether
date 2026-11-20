- **`fs.unlink` of a junction works inside an enforce block on Windows
  (#3751).** The checked removal counted a junction as a directory, so
  unlinking one in a block failed with "is a directory", while outside a
  block (and `fs.is_symlink`) it is a link. Any name-surrogate reparse
  point is now a link to the checked removal too, and what it points at
  stays.
