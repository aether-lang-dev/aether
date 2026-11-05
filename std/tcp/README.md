# std.tcp

TCP sockets: connect, listen, accept, read, write.

Every call returns an error string rather than throwing. On a socket that is
the right shape (a peer closing mid-write is a normal Tuesday, not an
exceptional condition), but it does mean the error has to be checked at every
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
        tcp.close(conn)
        return
    }

    body, rerr = tcp.read(conn, 1024)
    if rerr != "" {
        println("read failed: ${rerr}")
    } else {
        println(body)
    }

    tcp.close(conn)
}
```

Every write and read may transfer **fewer bytes than asked**, which is TCP
working as designed rather than an error: each is one `send(2)` or `recv(2)`,
and returns the count it moved. A send comes back short when the socket's
30 s send timeout expires while the peer reads slowly, or a signal cuts it
short. `write_all(sock, s)` sends a whole `byte[]` slice, as many sends as it
takes, and returns `(s.len, "")` or the bytes sent with `"send failed"` (#3108);
a read loops on the rest until the full count is in or the connection dies.

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
sent, werr = tcp.write_all(sock, frame)
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
will not block), `0` on timeout, `-1` on a null handle: check the stop flag
between polls. `set_nodelay(sock, true)` turns on `TCP_NODELAY` for
request/response lines (the runtime's headers already declare `setsockopt`
with the platform's signature, so it cannot be declared from Aether).
`set_read_buffer(sock, bytes)` sets the socket's receive buffer
(`SO_RCVBUF`), which asks the kernel not to grow it by itself while data
arrives, read or not. On a connected socket the window advertised when the
connection opened still stands, so a client that has to take little while
it does not read (a test of a server's send timeout, say) connects with
`connect_buffered(host, port, bytes)` instead, which sets the buffer before
the connection is made (#3338). The size is a request a kernel may round
up: macOS holds about 300 KB for a 16 KB request on loopback.
`available(sock)` says how many bytes are waiting in the socket's receive
buffer, readable without blocking (`FIONREAD`).
`close_drained(sock)` closes once what the peer already sent is read and
dropped (at most 256 KiB, 5 ms for each more to arrive). A socket closed with
bytes unread ends with a reset, and a peer that gets one may discard what it
had not read yet, such as an alert just sent (#3465). A plain close is
`tcp_close`.

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

## Unix-domain sockets

`connect_unix(path)` connects to the stream socket file at `path`, a local
daemon's API socket such as Docker's `/var/run/docker.sock` or Podman's, and
returns the same handle `connect` does: every read, write, poll and close
above works on it (`set_nodelay` does not; there is no TCP). Errors name the
path: `no such socket`, `connection refused: nothing is listening on the
socket`, `permission denied`. A path longer than `sockaddr_un` holds (103
bytes on macOS and the BSDs, 107 on Linux and Windows) is refused rather
than truncated, since a truncated path names another file.

`listen_unix(path)` creates the socket file and listens on it: `accept`,
`server_poll` and `server_fd` work as for a TCP listener, `server_port` is
-1, and `tcp_server_close` removes the file. A file already at the path is
`address in use`; removing a stale one is the caller's call, never this
module's.

In a `sandbox.enforce` block, a connect needs `fs_write` on the path (the
kernel's own rule: connecting to a Unix socket takes write permission on its
file; the path is resolved, symlinks followed, as every fs check is), and a
listen needs `tcp_listen` as well. The `LD_PRELOAD` layer does not check
`AF_UNIX` connects, which libc makes on its own (nscd, syslog).

The descriptor inside is an ordinary connected `AF_UNIX` stream socket, so
`std.ipc`'s `send_fd` / `recv_fd` / `write_frame` work on `tcp.fd(sock)` to
pass open files to or from a daemon; `os.socketpair` is the unnamed twin, for
two ends in one process tree with no path at all.

Windows has `AF_UNIX` from Windows 10 1803; on an older one (and under Wine)
both calls say `Unix-domain sockets are not supported on this Windows`.

For HTTP over a Unix socket, `std.http.client`'s `set_unix_socket` sends a
request there instead of dialling the URL's host:

```aether,fragment
req = client.request("GET", "http://localhost/v1.41/containers/c1/logs?follow=1&stdout=1")
client.set_unix_socket(req, "/var/run/docker.sock")
client.set_timeout(req, 0ns)                 // a followed log can go quiet
resp, err = client.send_stream(req)
client.request_free(req)
if err != "" { println("engine: ${err}"); return }
going = 1
while going == 1 {
    chunk = client.response_read(resp, 65536)    // each chunk as it arrives
    if string.length(chunk) == 0 { going = 0 } else { print(chunk) }
}
client.response_free(resp)
```

## Exports

`connect`, `connect_buffered`, `connect_unix`, `listen`, `listen_on`, `listen_unix`, `accept`, `available`, `read`, `read_n`, `read_into`,
`write`, `write_n`, `write_slice`, `write_all`, `poll`, `poll2`, `server_poll`, `server_port`, `set_nodelay`,
`set_read_buffer`, `fd`, `server_fd`, `close_drained`, `tcp_close`, `tcp_server_close`.
