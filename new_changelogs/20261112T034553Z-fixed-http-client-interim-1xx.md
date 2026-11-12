- **std.http.client reads past interim 1xx responses to the real one.** A
  request sent with `Expect: 100-continue` to a server that honours it
  (Node's http server, Azurite) got `100 Continue` back as its response,
  status 100 with an empty body, and the final response was never read;
  RFC 9110 15.2 has a client parse any number of 1xx responses before the
  final one. The buffered reader, the streaming reader (`set_stream`) and
  the proxy's event loop now skip every interim head (1xx other than 101,
  which is final) before framing the response, and a connection reused
  after one is framed from the final head. Found by servirtium-vcr's Azure
  SDK harness.
