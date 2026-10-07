- **`language.match_strings` no longer serves one script for another.** The
  base-language fallback compared only the language subtag, so a request for
  `zh-Hant` against `en,zh-Hans` returned `zh-Hans`, which the README says it
  must not. A fallback match now requires the two tags not to name different
  scripts; `zh-TW` is still served `zh-Hans`, and `zh-Hant` gets the default.
- **`language.match_strings` keeps the client's order among equal `q`
  values, and never picks a `q=0` range.** The weight sort was an unstable
  swap sort, so `en;q=0.5, fr;q=0.5` chose `fr`; it is now a stable insertion
  sort. A range with `q=0` means "not acceptable" (RFC 7231) and was tried
  last instead of dropped, so `fr, en;q=0` returned `en`; it is now ignored.
- **A `plural` argument in `message.format` selects by its number.** Any
  value that was not a 32-bit integer (`1.5`, `3000000000`, `many`) silently
  counted as 0 and took the `=0` branch. The value is now read as a decimal
  numeral: `=N` matches only an equal number, otherwise the locale's CLDR
  category for the digits as written applies (`1.5 items` in English), and a
  value that is not a numeral takes `other`.
- **`encoding.base32_decode` rejects malformed input.** A final group of 1, 3
  or 6 characters, padding that does not complete the last group to 8, and
  non-zero bits after the last byte all decoded as success (`M` as no bytes,
  `MZXW6Y` as three). Each is now an error; unpadded and lower-case input are
  still accepted.
