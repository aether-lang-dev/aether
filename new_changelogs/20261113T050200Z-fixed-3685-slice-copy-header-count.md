- **Slices: growing a shared slice copies its header without reading the
  count (#3685).** An `append` to a slice whose buffer another place holds
  copies the buffer, and it copied the header with `memcpy`, the count
  included, while other holders changed the count atomically: one more
  plain read of a shared count, which ThreadSanitizer reported in
  `test_module_slice_var_threads` (`make test-tsan`, #3542). The header is
  copied field by field now, the new buffer's count set to 1 as before.
