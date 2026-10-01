- **The hashes, encoders and compressors take `byte[]` input (#2301).**
  `hash.fnv32`/`fnv64`/`murmur3_32`/`siphash24`/`crc32`/`crc32_update`,
  `encoding.hex_encode`/`base64_encode`/`base64_encode_padded`/
  `base32_encode`, `zlib.deflate`/`inflate`/`gzip_deflate`/`gzip_inflate`/
  `deflate_raw`/`inflate_raw`/`stream_write`, `zstd.compress`/`stream_write`,
  `brotli.compress`/`stream_write` and `lzf.compress`/`decompress` drop their
  separate length argument: the input is a slice whose length travels with
  it. Pass a string as `string.bytes(s)`, a prefix as `string.bytes(s)[0..n]`
  and a `std.bytes` buffer as `bytes.view(buf)`; a sub-slice hashes or
  compresses only its own bytes. Results are unchanged (owned, length-aware
  strings and `(bytes, n, err)` tuples), decoders still take text, and
  `lzf.decompress` still takes the original length. A slice with no bound
  panics. Every caller in `std/`, `contrib/`, `tests/` and the docs was moved;
  the compressors' input externs (`zlib_try_deflate` and the rest of the
  `*_try_*` calls that take data) now take `ptr` and are no longer exported.
