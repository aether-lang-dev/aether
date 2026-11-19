- **The proxy cache compares a key over its whole length (#3757).** A
  `method_url_vary` key holds a NUL before each segment the response's Vary
  adds (`GET /p`, then `\0accept=text/html`), and entries were matched with
  `strcmp`, which stops at the first NUL. Two variants of one URL were then
  told apart by their 64-bit hash alone, and a hash collision served one
  variant's response, possibly another user's language or encoding, for the
  other. Keys are now compared by length and `memcmp`.
