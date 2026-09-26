- **New `std.unicode`: Unicode-correct normalization, case/accent folding and
  grapheme-aware length/substring.** `std.string` is byte-indexed, so its
  `length`/`substring` cut multi-byte characters; `std.unicode` is codepoint-
  and grapheme-correct. `nfc`/`nfd` normalize; `fold_accents` strips combining
  marks (`"café"` -> `"cafe"`); `casefold` folds case for caseless matching
  (`ß` -> `ss`); `grapheme_len`/`grapheme_substring` count and slice
  user-perceived characters without splitting a combining sequence;
  `equals_ignore_case` and `equals_fold_accents` are the caseless /
  accent-insensitive compares. Backed by utf8proc (v2.9.0, MIT), which was
  already vendored for `contrib.i18n.collate` and is now moved (via `git mv`,
  history preserved) to `std/unicode/utf8proc/` and compiled into `libaether`,
  so `std.unicode` needs no `--extra` link; `contrib.i18n.collate` shares the
  same copy. Requested by the OpenDisk-ae port, which hand-wrote `od_text.c`
  for accent-folded search and case-insensitive compare.
