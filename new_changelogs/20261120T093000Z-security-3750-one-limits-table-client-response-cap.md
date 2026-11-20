- **Every HTTP resource limit is in one table, and the HTTP client bounds
  what a server can make it hold (#3750).** The server's limits, added one
  incident at a time, are now one struct (`HttpServerLimits`) with one set
  of defaults, documented together in docs/http-server.md, "Resource
  limits"; every existing setter keeps working and writes into it. The
  chunked request body's 8 MiB bound no longer passes a lower
  `server_set_max_body`. The HTTP client grew a response's buffer for as
  long as the server kept sending, and read a streamed response's head the
  same way with no deadline for the whole of it. A response now holds at
  most 64 MiB, head and body, unless `client.set_max_response(req, n)` or
  `client.set_default_max_response(n)` says otherwise (`0` for no bound),
  and past it the request fails with an error naming the limit; a streamed
  head is bounded by that limit and by the request's timeout. A chunked
  body counts by its payload, not its chunk framing, which the client
  holds no more than twice the limit of. A program that fetches larger
  responses whole sets the limit to their size, or reads them with
  `set_stream`. The reverse proxy gives each upstream fetch its own body
  cap as the limit, on the worker path and on the proxy event loop, so a
  cap past 64 MiB keeps working and a response past it is no longer
  buffered whole before it is refused, nor retried on another upstream as
  if the transport had failed; `ae add` fetches package archives of up to
  1 GiB.

  Behaviour change: every `std.http.client` request that is not streamed
  now fails on a response above 64 MiB unless it sets a limit
  (`client.set_max_response`, or `client.set_default_max_response` for the
  program; `0` for none).
