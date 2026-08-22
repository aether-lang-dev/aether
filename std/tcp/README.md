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

    werr = tcp.write(conn, "GET / HTTP/1.0\r\n\r\n")
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

## Exports

`connect`, `listen`, `accept`, `read`, `read_n`, `write`, `write_n`, `poll`,
`fd`, `server_fd`, `tcp_close`, `tcp_server_close`.
