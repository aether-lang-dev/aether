# Reply: absolute deadlines and cancellation for TLS clients

Answers `tls-client-absolute-deadlines-and-cancellation.md`. Shipped in
one PR with the external-PSK client and task execution contexts (branch
`batch/nov22-esp32-psk-deadlines-task-contexts`, not yet merged or
released). The design,
per platform, is `docs/design/tcp-tls-deadlines-and-cancellation.md`.

## The API

`std.tcp`, all additive:

```text
connect_until(host: string, port: int, due_ms: long, cancel: ptr) -> (ptr, string)
set_deadline(sock: ptr, due_ms: long, cancel: ptr) -> string
deadline(sock: ptr) -> long
stopped(sock: ptr) -> string            // "", DEADLINE_EXCEEDED or CANCELLED
cancel_new() -> (ptr, string)
cancel(c: ptr)
cancelled(c: ptr) -> bool
cancel_retain(c: ptr) -> string
cancel_free(c: ptr)
const DEADLINE_EXCEEDED = "deadline exceeded"
const CANCELLED = "cancelled"
```

`std.cryptography.tls13_client`: `connect`, `connect_mtls`, `connect_psk`,
`connect_resume` and `connect_resume_0rtt` take `due_ms: long = 0, cancel:
ptr = null`; `connect_socket*` and `connect_socket_psk` honour what the
caller bound with `tcp.set_deadline`; `conn_set_deadline(conn, due_ms,
cancel) -> string` rebinds after the handshake.

`due_ms` is an absolute time on `os.now_monotonic_ms`'s clock
(CLOCK_MONOTONIC, QueryPerformanceCounter), 0 for none: the spelling of
tls13_server's read deadline (#3737). Both are per connection and passed
explicitly; there is no process-wide setting.

## The contract, against the ask's list

- **One monotonic deadline over connect, handshake, writes and reads.** The
  dial, every handshake read and write, and every `conn_send`/`conn_recv`
  wait no later than it. Nothing restarts it: a peer trickling a byte every
  100 ms is cut off at the deadline.
- **Cancellation without raw handles or authority in the guest.** The host
  holds an opaque handle and calls `tcp.cancel(c)` from any thread or actor
  (on POSIX a signal handler too). It wakes a connection blocked in connect,
  handshake, read or write at once.
- **Ownership when expiry, cancellation and close race.** There is one
  closer: the connection's owner (`close_conn`, `tcp.close`, or a failed
  handshake, which closes the socket it was given before returning). A
  deadline is a bound on the owner's own waits, enforced in the owner's
  thread. A cancel writes to the handle's own descriptor (eventfd, pipe, or a
  loopback pair on Windows), which every wait polls beside the socket, and
  never touches the socket. So neither a double close nor the close of a
  reused descriptor is possible; no duplicated descriptor is needed. The
  handle is reference-counted (creator, each bound socket, and each other
  thread that may trip it, which takes its own reference with
  `cancel_retain` before it is handed the handle and drops it with
  `cancel_free`), so its descriptors outlive every wait and every trip.
- **Distinguishable errors.** A stop is exactly `tcp.DEADLINE_EXCEEDED` or
  `tcp.CANCELLED`, whichever handshake step or I/O it cut short, compared
  with `==`; never an EOF message, a TLS alert or an authentication error,
  and distinct from `"timeout"`, the socket's own per-call timeout, whose
  #3703 meaning is kept. Both are sticky.
- **Secrets reclaimed.** A stopped handshake goes down the same failure path
  as any other: every derived secret wiped, the socket closed. A stopped
  connection is failed (a write may have been cut inside a record);
  `close_conn` wipes its secrets and closes it at once.
- **Platforms.** Linux, macOS and iOS, FreeBSD/NetBSD/OpenBSD and Windows are
  implemented, not stubbed (table in the design doc). Builds without threads
  (`AETHER_NO_THREADING`) keep the deadline; a cancel there comes before the
  call, from a signal handler, or, with task contexts (the PR also answers
  `esp-idf-cooperative-task-local-execution-state.md`), from another task,
  a native one included. Builds without networking (the
  ESP-IDF component build today, wasm) refuse with "networking disabled at
  build time". Not bounded: name resolution (checked before and after, not
  interrupted), so the controller's numeric device address is the wholly
  bounded dial.

Cancellation stops waiting; it says nothing about what the peer received.
The application still owns idempotency and reconciling an unacknowledged
command.

## Tested against

- `tests/integration/tcp_deadline_cancel` (std.tcp, peers on threads of the
  test process using plain unbounded std.tcp): a connect that stalls
  (TEST-NET-1; skipped, saying so, where it fails at once) ended by the
  deadline and by a cancel from another thread; a dial refused when the
  deadline has passed or the handle is tripped; 256 KiB echoed under a
  deadline and a handle; a silent peer's read ended by the deadline and by a
  cancel; a byte every 100 ms ended by the total deadline; a 64 MiB write to
  a peer that does not read ended by the deadline and by a cancel; sticky
  stops, rebinding, freeing the handle while bound; and 200 rounds of expiry
  and cancel racing close with decoy connections opened in the gap (some
  taking the closed number), each decoy checked to carry its own traffic.
  A canceller that closes by number, the old shape (`BROKEN_CANCELLER=1`),
  fails that check.
- `tests/integration/crypto_tls13_external_psk` (TLS over the external-PSK
  paths against `pskpeer.ae` made to stall): handshake stalled (deadline;
  cancel; the adopted socket with `tcp.set_deadline`), stalled after the
  handshake (deadline, cancel), a record trickled a byte every 100 ms, a
  16 MiB write to a peer that does not read (deadline, cancel), ordinary
  traffic, a wrong key with a far deadline (an alert, not a stop), and a
  stalled dial. With the waits ignoring the deadline, the trickled read ran
  23.6 s and returned data instead of stopping at 1 s.

## Where it ran (2026-11-22)

- macOS arm64: both tests pass; the TLS/TCP/HTTP/crypto selection 294/0/5.
- cachyos (gcc 16): the selection 293/0/6; ASan with LeakSanitizer and UBSan
  over the new and core TLS/TCP tests, 38/0/1 each.
- ghostbsd (FreeBSD 15.1): the selection 295/0/4; the race check 200 rounds,
  130 decoys taking the closed number, all intact.
- ae-x64 (gcc 11 -Werror): `make ci` passes (2321/0/28 .ae, 765 C).
- Windows (cross-built, Wine 9): all 18 std.tcp checks, the race 200 rounds
  with 146 reused numbers, and all ten TLS stop cases.
- A stalled connect needs TEST-NET-1 to stall rather than fail at once; it
  did on every host here, and the case says it is skipped where not.

## What the esp32 controller changes

`clients/native/ios/ControllerTransport.m`'s `controller_tls_deadlines` hook
(SO_RCVTIMEO/SO_SNDTIMEO, the duplicated descriptor, the synchronized timer
that shuts the connection down, and the completion/expiry arbitration) goes.
The Aether adapter instead:

```aether,fragment
due = os.now_monotonic_ms() + 10000
sock, err = tcp.connect_until(device_ip, 443, due, cancel)    // or tcp.set_deadline on its own socket
conn, err = tls13_client.connect_socket_psk(sock, identity, key)
// per command: tls13_client.conn_set_deadline(conn, os.now_monotonic_ms() + 10000, cancel)
```

The native host keeps one `tcp.cancel_new()` handle per connection (or per
command) and calls `tcp.cancel` from its UI or lifecycle thread to abandon
one; it frees the handle after that thread can no longer trip it.
`test_pure_transport.py`'s stalled-peer case should then pass with the
native hook removed.

## Decisions left open

- `tcp.poll`, `poll2`, `wait_writable` and `server_poll` keep their own
  timeout and ignore the bound deadline and cancel. Making them honour the
  cancel would help relays; it changes their meaning, so it is left for a
  decision.
- (Settled.) `cancel_retain` was left open here and is now shipped: in
  GitHub CI's TSan job a canceller thread holding no reference raced the
  creator's `cancel_free` (the eventfd's write against its close, and two
  frees), in this branch's own tests. Each canceller now holds its own
  reference.
