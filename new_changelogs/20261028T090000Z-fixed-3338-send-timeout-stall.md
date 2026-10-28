- **http_proxy_send_timeout tests the server, and std.tcp can size a
  receive buffer before connecting.** The test's worker-path case received
  every byte now and then. Its client set its receive buffer after
  connecting, so the window the connection opened with still stood; on
  Linux it took that much through the stall and drained it for sixteen
  seconds after the cut. And macOS's loopback sometimes queued all 96 MB for
  a client with a 16 KB buffer that read nothing; the server then rightly
  went on, its sends being taken. New `tcp.connect_buffered(host, port,
  read_buffer)` sets the buffer before the connection is made, and
  `tcp.available(sock)` says how many bytes are waiting to be read. The test
  connects with the first, measures with the second, and runs a try again
  when the kernel took over 1 MB while the client was not reading; a try
  that stalled is judged as strictly as before (#3338).
