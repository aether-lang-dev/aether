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

## Exports

Server: `server_create`, `server_get`, `server_post`, `server_put`,
`server_delete`, `server_start`, `server_set_keepalive`, `server_set_host`,
`response_set_status`, `response_set_header`, `response_set_body`, and the
request accessors. Client: `get`, `post`, `http_response_status`,
`http_response_body`, `http_response_headers`, `http_response_free`.
