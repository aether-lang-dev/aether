# Portable absolute deadlines and cancellation for TLS clients

2026-11-22. Downstream evidence: esp32 native phone controller; SDK inspected:
Aether 0.935.0. This is a request to remove native transport glue, not a report
that all existing timeout support is absent.

## Current workaround

/Users/paul/scm/esp32/clients/native/ios/ControllerTransport.m supplies a native
controller_tls_deadlines hook to its pure-Aether TLS adapter. It extracts the
socket descriptor, installs SO_RCVTIMEO/SO_SNDTIMEO, duplicates the descriptor,
and uses a synchronized ten-second timer to shutdown the connection. Completion
and expiry arbitrate ownership so an expired timer cannot close a reused fd.

The duplicate-fd design prevents a lifetime race but is POSIX-specific plumbing
that the host should not have to maintain for each platform. A per-read timeout
alone does not bound a peer that continually sends small amounts of data.

## Existing support and the remaining gap

0.935.0 exports tcp.connect_timeout, fd/poll/wait_writable and adopted sockets.
The current tls13_client source also documents the #3703 fix: a socket receive
timeout now ends a blocking TLS wait instead of being retried 50 times. That fix
must be retained; the historical retry behaviour is not being reported as a
current 0.935 bug.

The request is an Aether-level end-to-end deadline/cancellation contract for
TCP+TLS, including adopted sockets and external-PSK connections. I did not find
that contract in the inspected tcp/tls13_client exports. If it already exists
elsewhere, please point to the supported replacement and tests.

## Requested behaviour

- One monotonic absolute deadline can cover connect, handshake, writes and reads.
  Slow progress must not silently reset it.
- A host can cancel a blocked connection safely, without exporting raw handles
  or authority into the guest language.
- Ownership is explicit when expiry, cancellation and ordinary close race.
  Neither double close nor closing an unrelated reused descriptor is possible.
- Timeout/cancellation are distinguishable from peer EOF, authentication failure
  and malformed TLS/HTTP. Secret buffers are reclaimed on every path.
- Define supported behaviour on macOS/iOS, Linux, Windows and FreeBSD. Document
  any ESP-IDF/cooperative restrictions rather than inheriting POSIX assumptions.

An options/deadline token or another design consistent with the existing socket
API is fine; the downstream does not require a particular spelling. Cancellation
must not make an unacknowledged command safe to retry: the application still owns
request idempotency and outcome reconciliation.

## Acceptance tests

Independent peers should stall before handshake, stall after handshake, trickle
records within each per-I/O timeout, and stop reading during a client write.
Verify the total deadline, prompt cancellation, ordinary successful traffic,
cleanup and expiry-versus-close/descriptor-reuse races. Include the adopted
socket path used by the external-PSK controller.

The downstream native test_pure_transport.py already includes a stalled-peer
case with an absolute timeout. Its current pass relies on the native workaround;
it is not evidence of a portable stdlib deadline API.
