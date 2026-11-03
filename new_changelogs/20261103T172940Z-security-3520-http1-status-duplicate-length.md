- **std.http1 refuses a status code that is not three digits and two
  Content-Length headers that disagree (#3520).** The status code reader
  skipped any byte that was not a digit, so `2x0` was status 20 and `2000`
  was accepted, where the C client requires exactly three digits (RFC 9112
  4). Two different lengths were resolved by taking the first, which leaves
  the bytes of the other to be read as something else; the reader now
  refuses them, as the server does (RFC 9112 6.3). Duplicates that agree are
  still read.
