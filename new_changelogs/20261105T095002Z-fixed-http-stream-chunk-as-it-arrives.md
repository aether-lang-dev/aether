- **A streamed chunked body is read as it arrives.** `client.response_read`
  on a `Transfer-Encoding: chunked` response ran its decoder until `max`
  bytes were in or the body ended, so a chunk that had arrived waited for
  more: a followed log or an event feed (Docker's `logs?follow=1`, `/events`)
  held its first line until enough others came or the receive timed out. It
  now returns the payload it has once the next step would wait on the
  connection, as a `Content-Length` stream already did. The bytes past the
  head of a streamed response with no body (a `101`) are kept for
  `response_take_conn` instead of dropped.
