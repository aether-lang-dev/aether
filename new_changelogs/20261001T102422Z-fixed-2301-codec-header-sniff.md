- **base64 and the compressors no longer misread input that begins with the
  string-header magic.** The C side of `encoding.base64_encode`, `std.zlib`,
  `std.zstd`, `std.brotli` and `std.lzf` checked its input for an
  AetherString header, so input starting `DE C0 57 AE` was taken for one and
  the call read from a `data` pointer pulled out of the bytes that follow,
  crashing or encoding/compressing whatever memory that named. The HTTP
  server reaches the same base64 code with a SHA-1 digest of the client's
  `Sec-WebSocket-Key`, so a client could grind a key whose digest starts that
  way. Input now reaches C as raw bytes with its length and is never checked;
  `tests/regression/test_issue2301_codec_slices.ae` round-trips such a
  payload through every codec (it segfaults against the old C).
