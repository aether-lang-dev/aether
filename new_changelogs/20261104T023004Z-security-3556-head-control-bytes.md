- **A response whose header section holds a NUL or a bare CR is refused, and
  a redirect's `Location` with a control byte is not followed (#3556).** The
  client found the end of a header section with `strstr`, which stops at a
  NUL, and read its fields as C strings. A bare CR stayed inside a field
  value, so `Location: /x\rX-Injected: y` sent `GET /x\rX-Injected: y` to the
  next hop, a header of the server's choosing in the client's request; a NUL
  hid the section, and the whole response came back as the body of a
  status-0 response with no error (the streamed reader waited for an end it
  had already been sent). Both readers now find the section's end past any
  NUL and refuse a section holding either with "malformed response header
  section" (RFC 9110 5.5, RFC 9112 2.2), and a `Location` holding a TAB or
  any other control byte stops the hop with a `redirect_error`, the 3xx kept.
- **Migrating:** the reverse proxy reads its upstreams with the same reader,
  so an upstream response with a bare CR in a header, which it forwarded
  with that field dropped, is now answered 502 (`upstream_transport`), as an
  invalid response from an upstream is (RFC 9110 15.6.3).
