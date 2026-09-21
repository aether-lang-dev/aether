# std.tcp

TCP sockets: connect, listen, accept, read, write.

Every call returns an error string rather than throwing. On a socket that is
the right shape — a peer closing mid-write is a normal Tuesday, not an
exceptional condition — but it does mean the error has to be checked at every
step, because a failed connect returns a handle you must not then use.

The example **compiles but is not run** in CI: it needs a peer on the network,
and a documentation example should not require one to be checked.

```aether
import std.tcp

main() {
    conn, err = tcp.connect("example.com", 80)
    if err != "" {
        println("connect failed: ${err}")
        return
    }

    _n, werr = tcp.write(conn, "GET / HTTP/1.0\r\n\r\n")
    if werr != "" {
        println("write failed: ${werr}")
        tcp.tcp_close(conn)
        return
    }

    body, rerr = tcp.read(conn, 1024)
    if rerr != "" {
        println("read failed: ${rerr}")
    } else {
        println(body)
    }

    tcp.tcp_close(conn)
}
```

`write` and `read` may transfer **fewer bytes than asked**, which is TCP
working as designed rather than an error. `write_n` and `read_n` loop until
the full count is transferred or the connection dies — usually what a caller
wants, and always what a length-prefixed protocol needs.

`poll` reports readability without blocking, which is how one thread services
several sockets. For an HTTP server rather than raw sockets, use
`std.http.server`, which handles keep-alive, parsing and connection parking
already.

## Serving on one address, without blocking in accept

`listen(port)` binds every interface. A debugging or control channel with no
authentication must bind the loopback address and nothing else:
`listen_on("127.0.0.1", port)`. Port `0` asks the OS for an ephemeral port;
`server_port(srv)` reads back the one it chose, so a tool that starts a
process can ask it which port it got.

`accept` blocks, and closing the listener from another thread does not wake it
on Linux. A serving loop that has to stop polls the listener instead:
`server_poll(srv, 50)` returns `1` when a connection is waiting (so `accept`
will not block), `0` on timeout, `-1` on a null handle — check the stop flag
between polls. `set_nodelay(sock, true)` turns on `TCP_NODELAY` for
request/response lines (the runtime's headers already declare `setsockopt`
with the platform's signature, so it cannot be declared from Aether).

```aether,fragment
srv, err = tcp.listen_on("127.0.0.1", 0)
port = tcp.server_port(srv)
while !stopping {
    if tcp.server_poll(srv, 50) == 1 {
        sock, aerr = tcp.accept(srv)
        _ = tcp.set_nodelay(sock, true)
        // ... read a line, answer, close
    }
}
```

## Exports

`connect`, `listen`, `listen_on`, `accept`, `read`, `read_n`, `write`,
`write_n`, `poll`, `poll2`, `server_poll`, `server_port`, `set_nodelay`,
`fd`, `server_fd`, `tcp_close`, `tcp_server_close`.
