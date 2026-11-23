- **`tcp.listen` queues connections up to the kernel's limit (#3832).** It
  listened with a backlog of 5, so a burst of more connects than that before
  the server accepted filled the queue: Linux and macOS dropped the SYN and
  the client resent it a second later, with nothing in any log, and Windows
  refused the connect after two seconds of retries. `tcp.listen`,
  `tcp.listen_on` and the Unix socket listener (16 before) now listen with
  `SOMAXCONN`, as Go and nginx do. 64 connects with none accepted: the 6th
  failed after 2030 ms on Windows; now the slowest takes 20 ms.
