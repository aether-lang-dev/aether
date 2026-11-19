- **`std.cryptography.asn1` reads an indefinite length in linear time
  (#3749).** Each TLV inside an indefinite-length TLV was appended to the
  content read so far by reallocating, copying and wiping all of it, so
  `30 80`, N empty TLVs and `00 00` cost about N * N bytes: 5 seconds of CPU
  for a 400 KB certificate a TLS peer may send. The content is now copied
  once from the input, where those TLVs lie side by side. BER indefinite
  lengths are read as before, and `last_tag` after one is now its own tag,
  where it was the tag of the last TLV inside it.
