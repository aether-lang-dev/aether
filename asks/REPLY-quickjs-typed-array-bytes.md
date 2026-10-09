# REPLY: contrib.quickjs reads and makes Uint8Arrays

**Re:** sae `asks/quickjs-typed-array-bytes.md`

**RESOLVED** in branch `fix/win-main-closure-drain-contrib-hooks` (the
first release after 0.799.0 carries it).

As proposed:

```
bytes_of(q: ptr, h: int) -> (ptr, int)
arg_bytes(q: ptr, args: int, i: int) -> (ptr, int)
new_uint8array(q: ptr, data: ptr, n: int) -> int
```

- `bytes_of` takes a `Uint8Array` or `Uint8ClampedArray` only (another
  typed array's bytes are not what a byte reader expects), and gives the
  view's own window: `new Uint8Array([9, 1, 2, 3]).subarray(1)` reads as
  `1, 2, 3`, your self-check, now a spec. `(null, -1)` for anything else,
  and for a view whose buffer is detached; an empty array is a non-null
  pointer and 0. No copy, and no exception left pending when it refuses
  (a spec checks a host function that then fails without throwing is
  reported as exactly that).
- `arg_bytes` is the one-call form for a host function's argument; the
  pointer is valid while the host function runs.
- `new_uint8array` copies `n` bytes from a raw pointer (`bytes.data(b)`
  for a `std.bytes` buffer, not the handle).

Tests: four new specs in `contrib/quickjs/test_quickjs.ae`, including a
65 536-byte array summed through one `arg_bytes` call, and the handle count
back to 0 at the end.

## What sae can drop

`src/sae_raster.c` (the `SaeQjsHead` mirror of `AeQjs` and its self-check)
and the `quickjs.h` include path in `.build.ae`; `arg_bytes_` in
`src/sae_host.ae` becomes `quickjs.arg_bytes(ctx, argv, i)`.
