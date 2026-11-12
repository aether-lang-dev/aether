# REPLY: std.http.client reads past interim 1xx responses

**Re:** servirtium-vcr `asks/aether-http-client-interim-1xx.md`

**RESOLVED** in branch `fix/batch-2026-11-12b` (the first release after
0.909.0 carries it).

As the ask proposed: the response reader skips every interim head (a 1xx
status other than 101) and frames the response from the one after it.
One scan, `http_interim_scan` in `std/net/aether_http.c`, is used by every
reader of a response:

- the buffered reader (`http_response_is_complete` for where the response
  ends, `http_response_fill_from_bytes` for status, headers and body), so
  the proxy's event loop, which shares both, skips them too;
- the streaming reader (`set_stream` / `send_stream`), which drops the
  interim heads before it hands the transport to the stream;
- `http_response_head_keeps_conn`, so whether the connection is pooled is
  read from the final head.

101 Switching Protocols is final and comes back as it is (a
`response_take_conn` caller depends on that). The client still sends the
body straight away rather than waiting for the 100, which RFC 9110 allows;
waiting (with a timeout, as curl and .NET do) would be a separate change.

Test: `tests/integration/http_client_interim_1xx/` against a raw-socket
upstream: 100, a pause, then 201 + body (buffered and streamed), the same
connection reused for a second request, 100 and 103 Early Hints and 200
in one write, and 101 kept. Fails on 0.909.0 with "status 100 body ''".

The record-mode workaround (not forwarding `Expect`) can go once
servirtium-vcr is on the release carrying this.
