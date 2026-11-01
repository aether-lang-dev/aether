- **Three reference statements now say what the runtime does (#3480).** A
  handler that takes an ask and returns without a `reply` ends the ask at
  once with "the handler returned without replying"; the language
  reference said the caller timed out. std.cryptography's note on
  constant-time comparison now points to `ct.bytes_eq` and
  `ct.bytes_eq_slice` instead of telling callers to write their own. And
  `aether_set_call_deadline(ms)` sets one instant per thread, `ms` from
  when it is called, which every later call is cut at until a fresh
  deadline, not a budget per call (emit-lib.md and libaether.h).
