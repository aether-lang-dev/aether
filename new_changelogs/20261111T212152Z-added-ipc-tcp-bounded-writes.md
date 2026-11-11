- **std.ipc and std.tcp can write without waiting on the reader.** Every
  `ipc.write`, `ipc.write_frame` and `tcp.write` waited until the peer took
  the bytes, so a program writing to a pipe or socket whose reader had
  stopped reading froze until it read, exited or was killed; sae ran a
  writer thread per channel to keep its window alive. New in std.ipc:
  `wait_writable(fd, ms)` (`poll` for room; on Windows the pipe's free
  room, `WriteQuotaAvailable`), `write_some(fd, bytes, offset) -> (n, err)`
  (what fits now, never waiting, without changing the descriptor's flags),
  and a frame writer (`frame_writer(fd)`, `frame_writer_put`,
  `frame_writer_flush(w, ms)`, `frame_writer_pending`, `frame_writer_free`)
  that queues frames in a queue of its own and writes them as the reader
  takes them, resuming inside a frame, so a UI tick can flush with a
  timeout of 0. On Windows a pipe is written no more than its free room,
  and, when a reader waiting in a large read makes an empty pipe report
  none, in `PIPE_NOWAIT` mode for those writes and back after: no thread,
  and nothing left changed on a pipe someone else created. New in std.tcp: `wait_writable(sock, ms)`
  and `write_some(sock, byte[]) -> (n, err)`; macOS's `send` waits for
  room even with `MSG_DONTWAIT` once part of a send has gone, so there (as
  on NetBSD and Windows) the socket is non-blocking for the one send.
