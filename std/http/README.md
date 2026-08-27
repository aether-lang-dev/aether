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

**`ws://` only for now.** A `wss://` URL returns `null` rather than silently
connecting in the clear; TLS is a separate change.

One asymmetry is invisible but required: RFC 6455 §5.3 says a client must mask
every frame it sends and a server must not. The client handle does; the server
handle does not.

## Exports

Server: `server_create`, `server_get`, `server_post`, `server_put`,
`server_delete`, `server_start`, `server_set_keepalive`, `server_set_host`,
`response_set_status`, `response_set_header`, `response_set_body`, and the
request accessors. Client: `get`, `post`, `http_response_status`,
`http_response_body`, `http_response_headers`, `http_response_free`.
