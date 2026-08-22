# std.net

Low-level networking primitives, shared by `std.tcp` and `std.http`.

Mostly plumbing: raw descriptor writes (`fd_write`, `fd_close`) and the HTTP
response accessors that both the client and the server build on. There is no
`fd_read` here — reading is `std.tcp`'s job, and the one consumer of this
layer that only needs to write is `std.ipc`. Application code usually wants `std.tcp` for sockets or
`std.http` for HTTP — reach here when you hold a descriptor from somewhere
else and need to move bytes over it.

`std.ipc` is built on `fd_write` for exactly that reason: the back-channel is
an inherited descriptor, not a socket it opened.

The example **compiles but is not run** in CI: it needs a live descriptor.

```aether
import std.net

fn report(fd: int) {
    n = net.fd_write(fd, "status: ok\n")
    if n < 0 {
        println("write failed")
        return
    }
    net.fd_close(fd)
}

main() {
    println("std.net is used through std.tcp, std.http and std.ipc")
}
```

`fd_write` returns the byte count, or negative on failure — the C convention
rather than the `(value, err)` one used above this layer, because this is the
layer where the wrapping happens.

The `http_response_*` accessors read a response object: `status_code`,
`body_str`, `headers_str` and their non-string variants. They live here rather
than in `std.http` so the client and the server share one representation.

## Exports

`fd_write`, `fd_close`, `pipe_write_fd`, `tcp_close`, `tcp_server_close`,
`http_response_status`, `http_response_status_code`, `http_response_body`,
`http_response_body_str`, `http_response_headers`, `http_response_headers_str`,
`http_response_error`, `http_response_ok`, `http_response_free`,
`http_server_create`.
