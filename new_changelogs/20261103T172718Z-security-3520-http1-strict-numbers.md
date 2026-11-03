- **std.http1 refuses a Content-Length or chunk size that is not a plain
  count (#3520).** The pure-Aether response reader skipped every byte of a
  Content-Length that was not a digit, so `5abc9` framed 59 bytes, and read
  a chunk size up to the first byte that was not hex, so `0x5` was the last
  chunk; both wrapped past the largest int. They now follow std.net's rules:
  digits of the base only, no more digits than the largest value has, and
  chunk-size lines are `chunk-size [ chunk-ext ]` with the extensions read to
  their end. Chunk data must also be followed by its CRLF.
