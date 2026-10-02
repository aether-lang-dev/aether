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

Every write and read may transfer **fewer bytes than asked**, which is TCP
working as designed rather than an error: each is one `send(2)` or `recv(2)`,
and returns the count it moved. A length-prefixed protocol loops on the
rest until the full count is transferred or the connection dies.

## Binary data in `byte[]` slices

`write_slice(sock, s)` sends a `byte[]` slice's bytes as they are, zero bytes
included: the length travels with the slice. `read_into(sock, buf)` receives
into a caller-owned slice, allocating nothing, and returns `(n, err)` with the
data in `buf[0..n]`; it never writes past `buf.len`. Both may be short, so
the loops take the rest as a sub-slice. A quiet peer is `"timeout"` (retry or
`poll`, as with `read_n`), a closed one `"connection closed or receive
failed"`. An empty receive slice returns `"receive buffer is empty"` without
reading, and a slice with no bound (`p as byte[]`, `.len` -1) panics.

```aether,fragment
// Send all of `frame`, then read exactly `want` bytes into `buf`.
sent = 0
while sent < frame.len {
    n, err = tcp.write_slice(sock, frame[sent..])
    if err != "" { break }
    sent = sent + n
}
got = 0
while got < want {
    n, err = tcp.read_into(sock, buf[got..want])
    if err != "" { break }          // "timeout": poll and retry; else closed
    got = got + n
}
```

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

`connect`, `listen`, `listen_on`, `accept`, `read`, `read_n`, `read_into`,
`write`, `write_n`, `write_slice`, `poll`, `poll2`, `server_poll`, `server_port`, `set_nodelay`,
`fd`, `server_fd`, `tcp_close`, `tcp_server_close`.
