- **contrib.quickjs reads and makes typed arrays.** `bytes_of(q, h)` and
  `arg_bytes(q, args, i)` give a `Uint8Array`'s (or `Uint8ClampedArray`'s)
  bytes, its own window of its buffer, as `(pointer, count)` with no copy,
  `(null, -1)` for anything else or a detached buffer; `new_uint8array(q,
  data, n)` hands a script a copy of the host's bytes. A 128x128 raster was
  65 536 handle allocations through `get_index`, and sae mirrored the
  runtime's private struct to reach the engine instead (sae
  asks/quickjs-typed-array-bytes.md).
