- **`free` of a string's bytes is the same panic on every target (#3568).**
  Where a heap pointer carries a tag in its top byte (Android on aarch64),
  `string.bytes(s)` gives a read-only copy of the characters instead of a view
  that holds the string, and `free` of that copy gave its buffer back where
  every other target refuses the `free` by name. The copy is refused the same
  way now.
