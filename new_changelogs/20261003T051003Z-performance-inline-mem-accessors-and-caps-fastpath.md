- **More `std.mem` accessors are lowered inline, and `--emit=lib` loop heads
  no longer call into the runtime when no deadline is armed.** The #1733
  inline lowering now also covers `ptr_to_long` / `long_to_ptr`, `get_ptr` /
  `set_ptr` (through memcpy, so alias-safe once inlined) and the
  int8/uint8/int16/uint16/uint32 accessors; each was an out-of-line libaether
  call. And every `--emit=lib` loop head called `aether_caps_deadline_tripped()`,
  which reads two thread-locals (each a `_tlv_get_addr` call on macOS); it now
  tests a plain global, `aether_caps_armed`, first. Together these were about
  half the run time of mquickjs-ae's VM: on Octane it is now 2.35× faster
  (0.15 → 0.36 of upstream C's score), and its microbench went from 7.25× to
  3.38× slower than C. `tests/integration/mem_inline_accessors` compares every
  new lowering against its extern, null paths included.
