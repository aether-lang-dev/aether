- **`string.index_of`, `split` and `replace` look for a needle with
  memchr instead of comparing at every byte (#3531).** They called memcmp
  at each offset of the text, a libc call per byte for a one byte
  separator. One search now finds each place the needle's first byte
  occurs and compares the rest only there, and the `_from` and `_n` forms
  share it. Best of 5 on this machine: `index_of` over 256 KiB 438 to 57
  us, splitting 2000 fields 130 to 104 us. A match past 2 GiB panics, as
  `string.length` does, instead of coming back as a negative position.
