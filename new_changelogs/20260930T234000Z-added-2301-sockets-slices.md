- **`std.tcp` and `std.udp` send and receive `byte[]` slices.**
  `tcp.write_slice(sock, s)` sends a slice's bytes as they are, and
  `tcp.read_into(sock, buf)` receives into a caller-owned slice without
  allocating and returns the count, the data being `buf[0..n]`; both keep
  TCP's short transfers and `read_n`'s "timeout" sentinel.
  `udp.send_slice_to`, `send_slice_to_addr`, `recv_slice_from` and
  `recv_slice_from_into` do the same for datagrams. A receive never writes
  past the slice (the pointer forms trusted a separate capacity), a datagram
  longer than the slice is truncated to it with the rest discarded, an empty
  receive slice is refused without consuming anything, and a slice with no
  bound panics (#2301).
