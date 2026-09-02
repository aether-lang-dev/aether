# std.http

HTTP client and server.

The server handles keep-alive, connection parking, TLS termination, routing,
middleware and HTTP/2; the client does requests, connection reuse and TLS
verification. This is the largest module in `std`, and
`docs/http-server.md` is the reference for the server side — this page is the
orientation.

The example **compiles but is not run** in CI: a server binds a port and does
not return.

```aether
import std.http

fn handle(req: ptr, res: ptr, ud: ptr) {
    http.response_set_status(res, 200)
    http.response_set_header(res, "Content-Type", "text/plain")
    http.response_set_body(res, "hello\n")
}

main() {
    server = http.server_create(8080)
    if server == 0 {
        println("could not bind 8080")
        return
    }

    http.server_get(server, "/", handle, 0)
    http.server_start(server)
}
```

A handler takes `(request, response, user_data)` and fills the response; the
`user_data` pointer is whatever was passed at registration, which is how a
handler reaches application state without a global.

**Keep-alive is on by default**, and an idle connection is parked in a poller
rather than holding a worker thread — so concurrency bounds on file
descriptors, not on the `cores * 2` worker count. `AETHER_HTTP_PARKING=0`
turns that off if a regression needs bisecting.

For the client, `http.get` / `http.post` return a response object that must be
freed with `http_response_free`. Connection reuse is on by default.

## TLS

`server_set_tls(server, cert_path, key_path)` before `server_start` turns a
server into an HTTPS one. It returns an error string, `""` on success — check
it, because a bad path or an unreadable key is the common failure and the
server otherwise starts happily in plaintext.

```aether,fragment
err = http.server_set_tls(server, "/etc/ssl/cert.pem", "/etc/ssl/key.pem")
if err != "" {
    println("TLS setup failed: ${err}")
    return
}
```

**Two backends.** OpenSSL is the default wherever it is compiled in. The other
is a pure-Aether TLS 1.3 implementation with no OpenSSL dependency at all,
selected with `AETHER_PURE_TLS=1` — and used automatically when the build has
no OpenSSL, which is what lets a cross-compiled binary (`ae build --target=…`,
which links none) serve HTTPS at all.

The pure backend is **opt-in at the application**, because it reads the
certificate and key itself and so needs filesystem capability that a plain
`std.http` user should not inherit:

```aether,fragment
import std.http
import std.cryptography.tls13_server   // pure TLS server; needs --with=fs
```

The pure backend has constraints the OpenSSL one does not, and each fails as a
**dropped connection rather than a message**, so they are worth knowing before
debugging one:

- the server key must be **ECDSA P-256** — there is no RSA signing path, so an
  RSA certificate produces a server that fails at the signature step
  (`openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1`)
- key exchange is **X25519 only**
- **HelloRetryRequest is not implemented**, so a client offering only other
  groups is refused rather than renegotiated

The client side is the mirror, and is now wired the same way (#1849):
`std.http.client` does HTTPS through OpenSSL where it is compiled in, and
falls back to `std.cryptography.tls13_client` where it is not — which is every
`ae build --target=` cross-build, since zig bundles no TLS. Add

```aether,fragment
import std.cryptography.tls13_client
```

to the program and `https://` works in a cross-built binary that links no TLS
library at all. `set_insecure` and `set_cafile` are honoured on the pure path
too, and an https request through a forward proxy works because the CONNECT
tunnel is established before the handshake either way.

Without that import the pure client is not linked, and an https request in a
no-OpenSSL build fails with an error naming the missing import rather than
returning an empty body.

**Performance.** A pure handshake against a real CDN (a P-256 leaf over
secp384r1 intermediates) completes in about 1.3 seconds, and a full cross-built
HTTPS request in under a second. That is slower than OpenSSL but well inside any
server's handshake deadline. An earlier version of this note measured 12–22
seconds and warned that the default request timeout could expire mid-handshake;
that cost was `std.bignum`'s bit-serial division inside every P-256 and P-384
field reduction, and it is gone — both curves now reduce mod their prime with
shifts and adds. No timeout adjustment is needed.

See also `std.http.server.lb` for load balancing, `std.tcp` for raw sockets,
and `docs/http-server.md` for routing, middleware, TLS and the HTTP/2 path.

## WebSockets

The server side has shipped since v0.96 — `server_websocket(srv, path, handler,
user_data)` registers a route whose handler gets a live `ws` handle and uses
`ws_recv` / `ws_message` / `ws_send_text` / `ws_close`.

`ws_connect` is the other end: it dials a `ws://` URL, completes the
client-side upgrade, and returns a handle that takes **the same verbs**,
because the frame codec underneath is shared.

```aether,fragment
w = http.ws_connect("ws://127.0.0.1:9515/session/abc")
if w == null { return }

_ = http.ws_send_text(w, "{\"id\":1,\"method\":\"session.status\"}")
if http.ws_recv(w) == 1 {
    println(http.ws_message(w))
}

http.ws_close(w, 1000, "done")
http.ws_client_free(w)
```

`ws_recv` returns `1` for a text message, `2` for binary, `-1` once the peer
has closed; ping/pong is handled inside it on both ends. Free a **client**
handle with `ws_client_free` — server-side handles belong to the request loop
and must not be passed to it.

`ws_connect` returns `null` rather than raising: on a malformed URL, an
unreachable host, a response that is not `101`, or a `Sec-WebSocket-Accept`
that does not match the key it sent. That last check is what stops any `101`
being accepted, including one from a server that never saw the handshake.

**`wss://` dials over TLS**, reusing the HTTP client's trust store — the same
system CA discovery and TLS floor as `https`. The peer certificate is verified
and pinned to the host in the URL, so a certificate with a valid chain but the
wrong host is refused. The default port follows the scheme: 80 for `ws`, 443
for `wss`.

One asymmetry is invisible but required: RFC 6455 §5.3 says a client must mask
every frame it sends and a server must not. The client handle does; the server
handle does not.

### Reading without blocking

`ws_recv` blocks until a frame arrives, which is fine for a request/response
exchange and wrong for a **multiplexed** one. WebDriver-BiDi is the motivating
case: many commands in flight at once, each awaited by `id`, plus unsolicited
events on the same socket. That needs one reader routing frames to an id-keyed
table or an event queue — and built on a blocking `ws_recv`, that reader has to
own a thread.

Three calls avoid it:

```aether,fragment
// Bounded: 1 text, 2 binary, 0 nothing arrived in time, -1 closed.
k = http.ws_recv_timeout(w, 50)
if k == 0 { return }          // nothing yet, come back later
if k < 0 { return }           // peer gone

// Readiness, consuming nothing: 1 readable, 0 timed out, -1 closed.
if http.ws_poll(w, 50) == 1 {
    _ = http.ws_recv(w)       // guaranteed not to block
}
```

`ws_recv_timeout(w, 0)` is a true poll and never blocks, which makes it the
building block for a pump: call it, route whatever comes back, return.

The timeout bounds the wait for the **start** of a frame. Once a header has
been read the frame is finished even if that blocks, because WebSocket framing
has no resynchronisation point — abandoning a half-read frame would leave the
next read treating payload as a header. Pings and continuation frames handled
during the wait do not restart the clock, so a chatty peer cannot extend the
call.

**`ws_fd(w)` exposes the underlying socket** for a native event loop
(`asyncio.add_reader`, epoll, kqueue), and it comes with a contract:

> The descriptor is a readiness **hint**, not an authority. A frame can be
> sitting in the connection's buffer, or decrypted inside the TLS layer, with
> nothing pending on the socket. Always drain with `ws_recv_timeout(w, 0)`
> until it returns `0`, then go back to waiting.

That is not a theoretical caveat. Over `wss://`, OpenSSL decrypts an entire
TLS record at once, so a peer that writes two frames together leaves the
second one decrypted and waiting while `poll(2)` on the descriptor reports
nothing — measured, not assumed. `ws_poll` checks the connection buffer, then
the TLS layer, then the socket, so it cannot answer "not ready" while a frame
is already in hand. Prefer it whenever you want one authoritative answer;
reach for `ws_fd` only when an event loop needs a real descriptor.

## Exports

Server: `server_create`, `server_get`, `server_post`, `server_put`,
`server_delete`, `server_start`, `server_set_keepalive`, `server_set_host`,
`response_set_status`, `response_set_header`, `response_set_body`, and the
request accessors. Client: `get`, `post`, `http_response_status`,
`http_response_body`, `http_response_headers`, `http_response_free`.
