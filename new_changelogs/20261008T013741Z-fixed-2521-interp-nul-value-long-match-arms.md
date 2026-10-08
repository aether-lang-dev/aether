- **Interpolating a string value that holds a NUL keeps the bytes after it
  (#2521).** A `${s}` segment was formatted with printf's `%s`, which stops
  at the first NUL although the value carries its length, so `"[${s}]"` with
  `s` = `"x\0y"` had length 3, and `print`/`println` of such a value wrote
  the same prefix. A string segment and a printed string value are now
  written by their length. Each `print`/`println` is one write under
  stdout's lock, its newline included, so lines printed by two threads
  never interleave. Numbers, floats, booleans and durations print as
  before.
- **Two string-literal match arms sharing their first 160 characters are no
  longer reported as duplicates (#2521).** The reachability check keyed each
  arm in a fixed 160-byte buffer, so long arms with a common prefix drew a
  W1004 "already handled" warning, which fails builds that treat warnings
  as errors. The key now covers the whole literal.
