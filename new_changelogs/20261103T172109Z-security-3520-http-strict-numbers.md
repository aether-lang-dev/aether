- **std.net reads every framing number and URL port whole and strictly
  (#3520).** The server parsed a Content-Length from the first 255 bytes of
  its value, so 300 zeros and a 5 meant an empty body here and five bytes to
  a front end; it now refuses a value with more digits than any length it
  takes. Chunk sizes had two grammars: the framing check used `strtoll`
  (a sign, spaces, `0x`) and the decoder a loop that wrapped past 16 digits,
  so `0x5` framed as five bytes and decoded as the last chunk. Framing,
  decoding and the streaming client now walk one grammar, `chunk-size
  [ chunk-ext ] CRLF` (RFC 9112 7.1), and refuse anything else with 400. The
  client reads a response's Content-Length from the whole value too. A URL
  port, a proxy port and a WebSocket port are digits only in 1..65535:
  `host:80abc` dialled port 80, and `host:65616` reached a resolver that may
  keep only its low 16 bits.
