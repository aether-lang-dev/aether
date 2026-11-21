- **The shared map carries keys and values by their length, and its shared
  memory block is length-prefixed.** `aether_shared_map_*` stored C strings,
  so a NUL cut a key or a value, and the cross-process block used the NUL as
  its separator. Every key and value now carries its length
  (`aether_shared_map_put(map, key, key_len, value, value_len) -> int`, `get`
  with a `*value_len` out, `*_at` likewise; put returns -1 instead of
  dropping a new key in a full map), and the block is layout 2: a magic
  (`AESM`), a zero word that stops a layout-1 reader at an empty map, a
  version, then length-prefixed entries; a block in any other layout is
  refused whole when read back, and a child's block can no longer overwrite
  a frozen input. Aether code gets `std.sharedmap` (`new`, `put`, `get ->
  (string, int)`, `freeze`, `guest_put`, `free`, ...). Two older bugs went
  with it: `to_shm` wrote past its 64 KiB stack buffer for a large map (now
  refused with `E2BIG`), and on macOS its shm name was longer than the 31
  characters macOS allows, so a map never reached a child there.
