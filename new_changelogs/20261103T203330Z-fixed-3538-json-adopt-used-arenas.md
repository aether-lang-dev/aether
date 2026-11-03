- **A JSON container no longer keeps the whole arena of a small parsed
  document given to it (#3538).** A parse gives every document a 16 KB
  first chunk, and `json.push` or `json.set` of a parsed document adopted
  all of it, so 2000 parsed `{"id":N,"ok":true}` pushed into one array held
  32 MB where copies take 0.5 MB, and setting one key to a parsed document
  again and again kept each arena. A container now adopts an arena only when
  a third of it or more is in use; a mostly empty one is copied, its own
  nodes only, with what it adopted handed over as it is. Building a document
  level by level stays fast: best of 5, a 300 deep document builds in 0.09
  ms (0.92 ms before #3538) and an object of 5000 keys in 0.72 ms (11.8 ms).
