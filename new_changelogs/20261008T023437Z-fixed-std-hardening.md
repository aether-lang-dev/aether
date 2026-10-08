- **`zip.extract` and `tar.extract` cannot be steered outside the
  destination.** `zip.extract` checks every parent of an entry for a symlink
  or junction (a junction needs no privilege on Windows, and
  `dest/link/pwned.txt` was written through one), both modules write each
  entry at its cleaned name (`missing/../link/x` passed the check before
  `missing` existed), and an existing file or symlink at an entry's path is
  removed instead of written through. `tar.extract` measures a symlink's
  depth on its cleaned name (`./l -> ../escape` and `a//l -> ../../escape`
  passed with `allow_symlinks`) and refuses `..` after a name segment in a
  target (`r -> .` makes `r/..` the parent). On Windows `fs_is_symlink`
  counts a junction and `readlink` reads one, and `fs.remove_tree` removes a
  link instead of descending into it (through a junction it deleted files
  outside the tree).
- **A zip archive cannot misstate its entries.** A stored entry whose two
  recorded sizes differ is an error (a 100-byte entry claiming 1 byte got
  past `max_entry_bytes`), ZIP64 sizes and offsets at or past 2^63 or past
  the buffer are refused, and a comment holding `PK\5\6` no longer hides the
  entries: the end record whose comment ends the file is preferred, then the
  latest one that fits, as Python's zipfile reads appended archives.
  `std.zip` and `std.tar` can be imported together (one `ExtractOptions`),
  and `zip.extract` refuses `preserve_mode` and `preserve_mtime` instead of
  ignoring them.
- **`std.cbor` and `std.msgpack` decode hostile input safely and exactly
  (#2470, #2510).** Both stop at 256 levels of nesting (deep input overflowed
  the stack), check each declared length and count whole against the bytes
  left before reading or allocating (lengths from 2^31 were narrowed to a
  negative int, so truncated input decoded as success), and refuse bytes
  after the item (`0102` decoded as 1). `cbor.parse` refuses every malformed
  case of RFC 8949 Appendix F and invalid UTF-8, and accepts every example of
  Appendix A. Integers past the signed 64-bit range decode instead of
  flipping sign: `get_long` and `get_int` give 0 for them, `get_int64`
  returns the value with an error when it does not fit, `get_uint` (and
  CBOR's `get_nint`) read the bit pattern, and `from_uint` / `from_nint`
  build them. A parsed value keeps its encoding, so `encode` writes it back
  byte for byte; a float built with `num` takes its narrowest exact width;
  `cbor.diagnose` writes the shortest decimal that reads back the same float,
  JSON escapes and the §8.1 encoding indicators (`[_ 1, 2]`, `1.5_3`).
  `cbor.set` replaces an existing key instead of adding a duplicate,
  `map_get` matches an array or map key by content, and `msgpack.map_set` no
  longer frees the value it is setting.
- **`std.xml` enforces XML 1.0 well-formedness (#2471, #2510).** A
  mismatched, unopened or nameless end tag, a document ending inside an
  element, attributes not separated by whitespace, no root element or a
  second one, text or CDATA outside the root, an attribute named twice, a
  raw control character other than tab, LF and CR, a `&` that starts no
  reference, a reference to an entity other than the five predefined ones,
  and a character reference to no legal XML Char (`&#0;`, a surrogate, past
  U+10FFFF) all read without an error; each is now `EVENT_ERROR`, and
  `xml.error` gives its line, column and byte offset. A UTF-8 byte order mark
  before the root is accepted.
- **`std.schema` validates what it promises.** The error getters read `""`
  past the list (an access violation before); `INT` is 64-bit and a value
  outside it is an error (`{"age": 5000000000}` passed `max(120)` and
  vanished from the values); `default_to` refuses a default its type cannot
  read; `email()` accepts `first.last@example.com` and refuses `a@.com`; and
  `one_of` compares whole options (`"admin,user"` matched
  `one_of("admin,user,guest")`).
- **`std.cryptography.mlkem` runs the FIPS 203 input checks (#2482,
  #2508).** Encaps checks the encapsulation key's length and that every
  coefficient is below q (§7.2), decaps checks the ciphertext length, the
  decapsulation key length and its H(ek) (§7.3), and a failure returns an
  error and null outputs; a padded, short or out-of-range input used to be
  taken as is, padded with 0xFF or panic. The API returns errors:
  `mlkemN_keygen() -> (ek, dk, err)` and `mlkemN_encaps(ek) -> (ct, key,
  err)` draw from the OS CSPRNG, `mlkemN_decaps(dk, ct) -> (key, err)`, and
  the seeded `_derand` forms are for known-answer tests. `s16` is
  branch-free, and the size helpers return 0 for a `k` other than 2, 3 or 4
  instead of reading uninitialised memory.
- **`string.to_double` and `to_float` read the same text on every platform,
  and `to_int_radix` only what it documents (#2472, #2508).** `inf`,
  `infinity` and `nan` (any case, signed, `nan(...)`) and C99 hexadecimal
  constants such as `0x1.8p3` (correctly rounded, subnormals included) are
  read before the platform parser, so `from_double`'s `Infinity` and `NaN`
  read back on Windows, whose msvcrt reads neither. `to_int_radix("0x10",
  16)` returned 16 and took a leading space or `+`; it now takes an optional
  `-`, digits of the radix and trailing whitespace, with exact int64 bounds.
- **Numbers at the edge of their range are exact or an error.**
  `number.bytes` keeps 3 significant figures across a rounding carry
  (`bytes_si(999999)` was `1000 KB`) and renders `INT64_MIN`; `std.number`
  refuses an exponent beyond ±1,000,000 (`1e2147483648` formatted as `1`);
  `decimal.multiply` and `shift` return `(value, err)` and every decimal
  operation reports an exponent that leaves the `int` range; `decimal.pow`
  and the bignum shifts take `INT_MIN` without overflowing the stack;
  JSONPath slices keep bounds and steps past 32 bits; `lzf.max_compressed_size`
  no longer wraps past 130 MB; `json_get_long` clamps to int64 (`1e300` read
  as INT64_MIN), and `json.stringify` reports nesting past 256 levels instead
  of writing `null`.
- **`language`, `message` and the base32 and base64 decoders follow their
  standards.** `language.match_strings` never serves another script
  (`zh-Hant` got `zh-Hans`), keeps the client's order among equal `q` values
  and drops a `q=0` range. A `plural` argument in `message.format` selects by
  its number as written (`1.5`, `3000000000` and `many` all took `=0`).
  `encoding.base32_decode` and `base64_decode` reject a final group of an
  impossible length, padding that does not complete the last group, non-zero
  leftover bits and (base64) `=` mid-input; unpadded or lower-case base32 and
  line breaks in base64 are still accepted.
- **Smaller fixes.** `regex.replace` succeeds when the result outgrows the
  subject (it returned "" with "out of memory"). `time.parse_iso8601`
  refuses an offset or trailing text (an offset was dropped, five hours out)
  and `to_iso8601` writes years outside 0..9999 in full. `url.query_get`
  matches a key exactly (`a%3Db=1` answered for `a`). `unicode`'s case
  folding and normalisation keep invalid UTF-8 bytes instead of returning ""
  (so `equals_ignore_case` called any two such strings equal), and
  `grapheme_substring` stops at a malformed byte. `path.rel` has no answer
  when the base climbs above the target's root. `math.random_int` covers its
  whole range without bias or a division by zero (splitmix64, one sequence
  per seed on every platform), `random_float` is in `[0, 1)`,
  `abs_int(INT_MIN)` is defined and `min_float` / `max_float` ignore a NaN.
  `zlib.gzip_inflate` inflates every member, inflate refuses trailing bytes
  other than zero padding, and a compression level outside -1..9 is an
  error. `json.set` and `json.push` copy a node from a parsed document (they
  corrupted the heap). The HTTP proxy's trace and span ids and retry jitter
  come from a per-thread generator seeded per process, not an unseeded
  `rand()`.
- **Every extern that returns a fresh string hands it to the caller.**
  Fifteen in std and contrib were declared a plain `-> string`, so the
  compiler took the result for borrowed and leaked it at every call:
  `string.from_double`, `tcp.read` (a buffer per read),
  `proxy.pool_metrics_text` (the whole text per scrape), `sqlite.exec`'s
  error, tinyweb's `ws_generate_accept_key`, `ws_base64_encode` and
  `ws_unmask`, and the Factor, Racket, Rhombus and Aether host bridges'
  evaluate, get and capture functions. Each is declared `-> string @heap`;
  every other `-> string` extern was checked against its C body and returns
  storage that its handle, a static or a thread-local slot owns.
