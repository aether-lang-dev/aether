- **Raw byte I/O takes `byte[]` slices: the length travels with the data
  (#2301).** `fs.write_binary(path, data)`, `fs.write_atomic(path, data)`,
  `fs.pwrite(file, data, offset)`, `tcp.write_n(sock, data)`,
  `udp.send_to(sock, host, port, data)` and `udp.send_to_addr(sock, addr,
  data)` drop their separate length argument; `io.fd_read_into(fd, buf)`,
  `udp.recv_from(sock, buf)` and `udp.recv_from_into(sock, buf, addr)` read
  at most `buf.len` bytes, the received bytes being `buf[0..n]`.
  `io.fd_write_n(fd, data, length)` is now `io.fd_write(fd, data)`: a module
  function's C name is `<module>_<name>`, so `io.fd_write_n` was the same
  symbol as the C function `io_fd_write_n` and every call bound the extern
  directly. Pass a string as `string.bytes(s)` (a prefix as
  `string.bytes(s)[0..n]`) and a `std.bytes` buffer as `bytes.view(buf)` or,
  for a read, `bytes.capacity_view(buf)`. An empty slice writes nothing and
  succeeds; a slice with no bound panics. Backward compatibility was not a
  goal: every caller in `std/`, `contrib/` and `tests/` was moved, and
  `fs.pread_into`, which fills a `std.bytes` handle and sets its length, is
  unchanged. The raw externs behind these wrappers (`io_fd_write_n`,
  `fs_write_binary_raw`, `fs_write_atomic_raw`, `fs_pwrite_raw`,
  `tcp_send_n_raw`) now take `ptr` data and are no longer exported, and
  `std.net` no longer declares `tcp_send_n_raw`; a heap string passed to a
  `ptr` parameter would send its header bytes, so a caller of the old raw
  form now gets a compile error instead. The TLS 1.3 client and server send
  each sealed record as a borrowed view instead of copying it into a string
  first.
