- **`regex.replace` / `replace_all` succeed when the result outgrows the
  subject.** The first output buffer is subject + replacement + 64 bytes, and
  the retry relied on PCRE2 reporting the size it needed, which it does only
  under `PCRE2_SUBSTITUTE_OVERFLOW_LENGTH`. Replacing 100 `a`s with ten
  characters each returned "" with "regex: out of memory".
- **`time.parse_iso8601` refuses an offset or trailing text.** An offset
  (`+05:00`) was dropped, so the instant came back five hours out with no
  error, and trailing garbage was accepted. A fraction of a second (dropped)
  and a `Z` or `z` are still accepted, so JavaScript's `toISOString` output
  parses; offsets are `parse_iso8601_offset`'s to read.
- **`time.to_iso8601` writes years outside 0..9999 in full.** Both 10000 and
  -1 came out as "0000"; they are "10000-..." and "-0001-...", as ISO 8601's
  expanded years are.
- **`url.query_get` matches a key exactly.** A key that decoded to contain
  `=` (`a%3Db=1`) was stored as "a=b=1" and answered for key `a`. Within a
  stored entry a key's own `%` and `=` stay encoded, so the first `=` always
  ends the key.
- **`unicode` handles invalid UTF-8 without losing or conflating text.**
  `casefold`, `nfc`, `nfd` and `fold_accents` returned "" for any input with
  an invalid byte, so `equals_ignore_case` called any two such strings equal.
  Valid runs are now mapped and invalid bytes kept as they are.
  `grapheme_substring` stops at a malformed byte, as `grapheme_len` does,
  instead of running on to the end.
- **`encoding.base64_decode` rejects malformed input.** `=` was skipped
  anywhere ("Zm9v=Zm9v" decoded to "foofoo"), a 1-character final group
  decoded to nothing, and non-zero leftover bits were ignored, each with an
  empty error. Padding, when present, must now complete a multiple of 4 and
  the leftover bits must be zero. Line breaks are still skipped.
- **`path.rel` has no answer when base climbs above target's root.**
  `rel("../a", "b")` returned "../../b", which joined onto `../a` is not `b`.
  It now returns no path, as Go's `filepath.Rel` does.
- **`math.random_int` covers its whole range, without bias or a crash.** It
  used `rand() % range`: on Windows, where RAND_MAX is 32767, it never
  returned more than 32767, and over the full int range the span overflowed
  to 0 and divided by zero. It is splitmix64 now, the same sequence for a
  seed on every platform. `random_float` is uniform over `[0, 1)`, no longer
  `[0, 1]`. `abs_int(INT_MIN)` is defined (INT_MIN) and `min_float` /
  `max_float` ignore a NaN on either side.
- **`lzf.max_compressed_size` does not wrap past 130 MB.** The bound was
  computed in 32 bits (200000000 gave 72032273), and `compress` allocated
  that wrapped bound as its output buffer.
- **`zlib.gzip_inflate` inflates every member of a gzip file, and inflate
  rejects trailing bytes.** Only the first member came back, the rest dropped
  with no error. Bytes after a zlib stream, or after the last gzip member,
  were ignored; zero padding there is still skipped, as gzip -d does.
  Compression levels outside -1..9 are an error in `deflate`, `gzip_deflate`,
  `deflate_raw` and `stream_new`, which replaced them with the default.
- **`json.set` / `json.push` with a node from a parsed document copies it.**
  The node lives in its document's arena; it was deep-copied and then freed
  as a heap tree, which corrupted the heap. `json.free` on such a node is a
  no-op for the same reason.
- **`json_get_long` clamps to the int64 range, and `stringify` reports nesting
  past 256 levels.** A double past int64 (`18446744073709551615`, `1e300`)
  read as INT64_MIN. Past the depth `parse` enforces, stringify wrote `null`
  in place of the rest and reported success.
- **`fs.remove_tree` removes a link instead of descending into it.** On
  Windows `file_stat` follows links, so a directory junction inside the tree
  looked like a directory and remove_tree deleted the files of the directory
  it pointed at, outside the tree.
- **The HTTP proxy's trace and span ids differ between processes.** They came
  from an unseeded `rand()`, so every process sent the same id sequence. A
  per-thread generator seeded from the clock, the process id and a stack
  address replaces it, for retry jitter too.
- **On Windows, `fs_is_symlink` counts a directory junction, and `readlink`
  reads one.** Junctions (`mklink /J`, no privilege needed) redirect path
  resolution exactly as symlinks do, but looked like ordinary directories, so
  tar extraction's parent check let an entry through one to a directory
  outside the destination.
