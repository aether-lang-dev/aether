- **`std.ipc` shared buffers: memory two processes map, passed as a
  descriptor.** `ipc.shared_buffer(size)` makes a zeroed buffer and holds its
  close-on-exec descriptor (`shared_buffer_fd`) to pass with `ipc.send_fd`;
  the receiver maps it with `shared_buffer_map(fd, size)` (or
  `shared_buffer_map_readonly`), the size checked against the descriptor, and
  both sides read and write it through a bounded `byte[]`
  (`shared_buffer_view`, `shared_buffer_read_view`) while the channel carries
  only short messages. Each buffer is its own handle, freed with
  `shared_buffer_free`. Linux and FreeBSD make a memfd whose size is sealed,
  so no holder can shrink it under another's mapping; macOS and the other
  BSDs an `shm_open` object unlinked at once. No sandbox grant is needed: a
  child under `sandbox.spawn_socket` with none maps and makes buffers.
  Windows reports it unsupported, as it does descriptor passing.
