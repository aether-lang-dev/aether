- **The reverse proxy answers a malformed upstream head the same way on
  both of its paths (#3749).** The event loop's pass-through ended the
  status line at the first LF and stopped reading fields at a bare CR,
  while the copying path reads through the client, which turns a bare CR
  into a space and ends lines only at CR LF. On a status line with a bare
  CR and a bare LF the pass-through kept a Content-Length the copying path
  dropped. The pass-through now takes only heads whose lines all end in CR
  LF, with no bare CR, bare LF, NUL or folded line, as the upstream sent
  them: the client's framing replaces a NUL or a bare CR with a space
  before the pass-through sees the head (#3572), and it now records that it
  did, so a repaired head goes the copying path too. Before, the
  pass-through relayed "Content-Length: 1\rX: y" as one field with the
  value "1 X: y". Found by the proxy-response fuzz target.
  On an answer to HEAD neither path forwards a Content-Length the
  upstream stated badly: each value must be digits only and every one the
  same (RFC 9110 8.6, RFC 9112 6.3). The pass-through kept the first of
  "2" and "0 OK", the copying path the last.
  The proxy cache stores an answer's fields under the same rules (#3782):
  a hit sends what the miss sent, where it replayed a length the miss had
  dropped and a value's trailing whitespace. And an answer to HEAD whose
  lengths were dropped ends the client's connection after its head on the
  pass-through, as one whose upstream stated no length does (#3710).
