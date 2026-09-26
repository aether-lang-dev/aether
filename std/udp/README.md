# std.udp

Datagram sockets: bind, send_to, recv_from, poll. The other half of
`std.tcp`, for game networking.

A game's state runs on datagrams because a snapshot that arrives late is worth
less than none: it must not wait behind a lost one the way TCP's in-order
stream makes it wait. So every socket here is **non-blocking from bind**. A
game loop drains the socket once a tick with `recv_from` until it reports
`"would block"`, and never waits on it. `poll` is for the program that does
want to wait, and `fd` for a poll across several sockets.

Every call returns an error string rather than throwing, as in `std.tcp`.
`"would block"` is the one error that is not a failure: the socket is fine and
there is simply nothing waiting.

The example runs over loopback in CI: a server on an ephemeral port, a client
that sends to it by name, and the server answering through the sender's
address without resolving anything.

```aether,run
import std.udp
import std.bytes

main() {
    server, err = udp.bind("127.0.0.1", 0)     // port 0: the OS picks one
    if err != "" { println("bind failed: ${err}"); return }
    port = udp.local_port(server)

    client, cerr = udp.bind("127.0.0.1", 0)
    if cerr != "" { println("bind failed: ${cerr}"); return }

    // Nothing waiting yet: "would block", not a wait and not a failure.
    buf = bytes.new(1200)
    _, _, idle = udp.recv_from(server, bytes.data(buf), 1200)
    println("idle server: ${idle}")

    _, serr = udp.send_to(client, "127.0.0.1", port, "snapshot 1", 10)
    if serr != "" { println("send failed: ${serr}"); return }

    ready, _ = udp.poll(server, 1000)
    n, sender, rerr = udp.recv_from(server, bytes.data(buf), 1200)
    println("ready=${ready} got ${n} bytes: ${bytes.to_string(buf, n)}")

    // The sender is an address value: answer it without resolving.
    _, aerr = udp.send_to_addr(server, sender, "ack", 3)
    if aerr != "" { println("answer failed: ${aerr}"); return }
    m, from, _ = udp.recv_from(client, bytes.data(buf), 1200)
    println("client got ${bytes.to_string(buf, m)} from the server: ${udp.addr_port(from) == port}")

    udp.addr_free(from)
    udp.addr_free(sender)
    bytes.free(buf)
    udp.close(client)
    udp.close(server)
}
```
```output
idle server: would block
ready=true got 10 bytes: snapshot 1
client got ack from the server: true
```

## Addresses are values

A server answering 64 peers a tick must not resolve 64 names a tick.
`resolve(host, port)` does the lookup once and returns an address; the sender
of every received packet is one too. An address can be compared
(`addr_equal`), hashed (`addr_hash`, non-negative, stable within a process),
printed (`addr_host`, `addr_port`, and `addr_string` as `host:port`, or
`[host]:port` for IPv6, the natural key for a peer table), copied
(`addr_clone`) and sent to (`send_to_addr`). Free each one with `addr_free`.

A receive loop that must allocate nothing per packet keeps one address from
`addr_new` and fills it with `recv_from_into`:

```aether,fragment
peer = udp.addr_new()
while true {
    n, err = udp.recv_from_into(sock, bytes.data(buf), 1200, peer)
    if err != "" { break }          // "would block": the tick's packets are drained
    handle(peer, buf, n)            // compare with addr_equal, key on addr_string
}
```

## Sending

`send_to(sock, host, port, data, n)` sends one datagram of `n` bytes from
`data`, which may be a string or a bytes buffer's `bytes.data(b)`; it resolves
the name each call. `send_to_addr` takes a resolved address instead. A send
the OS could not queue reports an error and the packet is gone: a datagram
protocol expects loss and resends on its own schedule, so the module does not
retry.

`set_buffer_sizes(sock, send, recv)` sets `SO_SNDBUF` / `SO_RCVBUF` in bytes
(0 leaves that side alone); a server fanning snapshots out wants a larger
send buffer than the default. `set_broadcast(sock, true)` allows sends to a
broadcast address for LAN discovery.

## IPv4 and IPv6

`bind("0.0.0.0", port)` (or `""`) takes every IPv4 interface, `bind("::", port)`
IPv6 and, where the OS allows, IPv4 too on the same socket; `"127.0.0.1"` and
`"::1"` bind loopback only. A socket sends to peers of its own family: an
IPv4 socket cannot reach an IPv6 address, and `addr_equal` never equates a
v4 and a v6 address.

## Exports

`bind`, `local_port`, `close`, `fd`, `poll`, `set_buffer_sizes`,
`set_broadcast`, `resolve`, `addr_new`, `addr_clone`, `addr_free`,
`addr_host`, `addr_port`, `addr_string`, `addr_equal`, `addr_hash`,
`send_to`, `send_to_addr`, `recv_from`, `recv_from_into`.
