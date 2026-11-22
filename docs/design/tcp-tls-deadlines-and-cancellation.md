# Deadlines and cancellation for TCP and TLS connections

Answers `asks/tls-client-absolute-deadlines-and-cancellation.md`. Shipped in
`std.tcp` (`std/net/aether_net.c`) and used by
`std.cryptography.tls13_client`.

## What it is

A connection can carry two things, both bound to it alone and passed
explicitly; there is no process-wide setting:

- **A deadline**: one absolute time on the monotonic clock
  (`os.now_monotonic_ms`, CLOCK_MONOTONIC or QueryPerformanceCounter), in ms,
  0 for none. It covers the dial, the TLS handshake, and every read and write
  after, however many system calls they take: a peer that trickles a byte
  inside every per-read timeout still meets it, because nothing restarts it.
- **A cancel handle**: `tcp.cancel_new()`, tripped with `tcp.cancel(c)` from
  any thread or actor (on POSIX from a signal handler too). Tripping wakes
  every wait on every socket bound to it at once.

```aether,fragment
c, _ = tcp.cancel_new()
due = os.now_monotonic_ms() + 10000
conn, err = tls13_client.connect_psk(addr, port, id, key, due_ms: due, cancel: c)
// ... conn_send / conn_recv: all held to `due`; another thread may tcp.cancel(c)
tls13_client.close_conn(conn)      // the owner closes, on every path
tcp.cancel_free(c)                 // after the last thread that may trip it
```

An operation a deadline stops ends with `tcp.DEADLINE_EXCEEDED`
(`"deadline exceeded"`), one a cancel stops with `tcp.CANCELLED`
(`"cancelled"`): two exact strings, compared with `==`, distinct from
`"timeout"` (the socket's own per-call `SO_RCVTIMEO`/`SO_SNDTIMEO`, kept as
it was, #3703), from a peer's close, from a TLS authentication failure and
from malformed input. Both stops are sticky: once the deadline has passed or
the handle is tripped, every bounded call on the socket ends before reading
or sending a byte. `tcp.set_deadline` rebinds and clears the stop for a plain
socket; a TLS connection that was stopped stays failed (a write may have been
cut inside a record), so it is closed.

The API, all additive:

| std.tcp | |
|---|---|
| `connect_until(host, port, due_ms, cancel)` | the dial bounded; the socket comes back with both bound |
| `set_deadline(sock, due_ms, cancel)` | bind both to any socket, an adopted one included |
| `deadline(sock)`, `stopped(sock)` | what is bound; what stopped the last call |
| `cancel_new()`, `cancel(c)`, `cancelled(c)`, `cancel_free(c)` | the handle |

| tls13_client | |
|---|---|
| `connect`, `connect_mtls`, `connect_psk`, `connect_resume`, `connect_resume_0rtt` | take `due_ms: long = 0, cancel: ptr = null` |
| `connect_socket*`, `connect_socket_psk` | honour what the caller bound with `tcp.set_deadline` |
| `conn_set_deadline(conn, due_ms, cancel)` | rebind after the handshake, per request |

The names follow what the stdlib already says: `due_ms` on the monotonic ms
clock is tls13_server's read deadline (#3737); a cancel the host trips and
the guest never sees is contrib.quickjs's cancel cell and std.guest's
`cancel`.

## Ownership: who closes what

The rule is one closer. A socket's descriptor is closed by `tcp.close`
(or `close_conn`, or a TLS handshake that failed, which closes the socket it
was given before returning) and by nothing else. A deadline does not close
anything: it is a bound on how long the owner's own call waits, enforced in
the owner's thread. A cancel does not close anything either: tripping writes
to the handle's own descriptor, which every wait polls beside the socket, and
the woken call returns to its owner, who closes.

So the race the esp32 workaround guarded against with a duplicated descriptor
(a timer closing a descriptor number that the owner had closed and the
system had handed to something else) cannot happen: there is no second
closer. The cancel handle is reference-counted (the creator's reference,
plus one per bound socket), so its own descriptors outlive every wait on
them whichever is freed first; the one misuse left is tripping a handle after
`cancel_free`, which the host avoids by freeing it after the last thread
that may trip it has finished. `tests/integration/tcp_deadline_cancel` races
expiry and cancel against close for hundreds of rounds, opening decoy
connections in the gap (some take the number just closed) and checking each
still carries its own traffic; with a canceller that closes by number, the
old shape, the same check fails.

## Per platform

| | wake handle | wait | connect | send |
|---|---|---|---|---|
| Linux | eventfd | `poll` on socket + eventfd | non-blocking `connect`, `poll` | `MSG_DONTWAIT` sends on a non-blocking socket |
| macOS, iOS | pipe (both ends non-blocking, close-on-exec) | `poll` | the same | the socket made non-blocking for the send (macOS's `MSG_DONTWAIT` still waits for room) |
| FreeBSD, NetBSD, OpenBSD | pipe | `poll` | the same | non-blocking socket, as above |
| Windows | a connected loopback TCP pair (the accepted end checked to be the one this process connected) | `select` on socket + wake socket (`WSAPoll` cannot wait on an event, and a pair works under Wine too) | non-blocking `connect`, `select` on write, except and the wake | `FIONBIO` for the send, put back blocking |

A cancel handle costs one descriptor on Linux and two elsewhere. A bounded
socket's per-call timeouts (`SO_RCVTIMEO`, `SO_SNDTIMEO`) are read each call
and still end a call that saw nothing for that long with `"timeout"`, as an
unbound call would.

Not bounded: **name resolution**. `getaddrinfo` cannot be interrupted, so
`connect_until` checks the deadline and the cancel before and after it; a
numeric address (what the esp32 controller dials) is the wholly bounded
dial. `tcp.poll`, `poll2`, `wait_writable` and `server_poll` take a timeout
of their own and do not consult the deadline; `write_some` never waits.

## Builds without threads or sockets

- **`AETHER_NO_THREADING`** (the cooperative scheduler: wasm, `ci-coop`,
  ESP-IDF): the deadline works as everywhere, since it is only the owner's
  own waits being bounded. A cancel can only come from somewhere that runs
  while the owner is blocked, and on one thread nothing does, except a POSIX
  signal handler (`tcp.cancel` is async-signal-safe there). A handle tripped
  before the call still refuses it at once.
- **`AETHER_NO_NETWORKING`** (wasm32-wasi, Emscripten, and the ESP-IDF
  component build as `tests/esp32/project` sets it today): std.tcp is the
  stub, so `connect_until`, `set_deadline` and `cancel_new` refuse with
  "networking disabled at build time" and `cancel` does nothing. When the
  ESP-IDF build gains lwIP sockets, lwIP's `select` and its eventfd
  (`esp_vfs_eventfd`) or a socket pair are the pieces the POSIX branch needs;
  it is not built or tested there now.

## What cancellation does not promise

A cancel or a deadline that ends a write says nothing about what the peer
received: some of the request may have gone. The application still owns
idempotency and working out the outcome of a command that was not
acknowledged; the transport only stops waiting.
