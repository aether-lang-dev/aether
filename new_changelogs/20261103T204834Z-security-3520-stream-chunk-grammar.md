- **The HTTP client's stream reader frames a chunked body by the same
  rules as its buffered reader (#3520).** A streamed response
  (`client.send_stream`) took a bare LF or a bare CR for the CRLF that ends
  a chunk's data, and after the last chunk read one CRLF and gave up on any
  trailer lines, while the buffered reader refuses the first two and reads
  the trailers: two readers of one body that disagree about where it ends
  are what request smuggling is built from. The stream reader now needs a
  CRLF after each chunk's data and reads trailer lines to the empty line,
  each ending in CRLF and at most 1024 bytes, as the buffered one does.
