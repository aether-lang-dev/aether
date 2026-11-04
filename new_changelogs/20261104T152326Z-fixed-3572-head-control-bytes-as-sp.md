- **A response whose header section holds a NUL or a bare CR is read again,
  and a redirect whose `Location` holds a control byte is followed (#3572).**
  #3556 closed a header injection by refusing such a response ("malformed
  response header section") and such a redirect (`redirect_error`), so a
  response that read before failed. A recipient may instead replace each
  NUL and bare CR with SP (RFC 9110 5.5, RFC 9112 2.2), and both response
  readers, the CONNECT reply's included, now do: a CR no longer ends
  anything, so a field cannot carry another behind it, and the response is
  read. A `Location` is read as a browser reads it (WHATWG URL): a TAB, an
  LF or a CR is removed, and any other control byte in its path, query or
  fragment is percent-encoded, as a space is, so the hop goes on and the
  request line it writes carries no injected field. A control byte in a
  `Location`'s host still stops the hop.
- **Migrating:** v0.881.0's note on the reverse proxy no longer applies: an
  upstream response with a bare CR in a header is forwarded again, that
  field's value with SP for the CR, where v0.881.0 answered 502.
