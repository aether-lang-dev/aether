- **The HTTP client's receive buffer travels with its pooled connection
  (#2160).** The blocking client path read every response into a buffer
  that started at nothing and grew to at least 16 KiB, then freed it when
  the request ended — so a pooled connection serving thousands of requests
  allocated and freed 16 KiB each time, even for a 200-byte response. In
  #1739's census that was 5.3 MiB over 300 proxied requests, the largest
  byte figure by an order of magnitude. The event-loop proxy driver already
  kept its buffer per connection; this was the blocking driver's gap.

  The buffer now lives on the `Transport`, which is exactly what the pool
  stores and returns whole and exactly what every disposal path hands to
  `transport_close` — so it rides with the connection into and out of the
  pool and is freed on every path that retires one, with no new ownership
  rule to get wrong. A buffer that grew past 64 KiB is not kept, so the idle
  pool can pin at most `max_idle × 64 KiB` rather than whatever the largest
  body was.

  Measured, not assumed: `http.client_rx_buffer_allocs()` counts buffers
  allocated from nothing. Ten requests over one pooled connection allocate
  **one**; with pooling disabled the same ten allocate **ten**, which is
  what every request did before. `tests/integration/http_client_keepalive`
  asserts that three requests over its provably-single connection needed
  one buffer.
