- **`string.bytes(s)` no longer leaks the string it views.** It returns a
  borrowed `byte[]` view, but its body hands `s` to a `ptr` extern and
  returns a view of it, which the compiler's escape walk read as `s`
  escaping, so a scope-owned heap string passed through it was never freed.
  `fs.write_binary(p, string.bytes(buf)[0..n])` on a fresh read buffer leaked
  the whole buffer per call. A named string passed to `string.bytes` is now
  freed at scope exit like any other: a slice does not extend its owner's
  lifetime, which is the documented contract. A *temporary* passed straight
  in (`string.bytes(f())`) is still left unfreed rather than freed under the
  view; hold it in a local. `tests/regression/test_issue2301_string_bytes_reclaim.ae`
  copies a 10 KB file through a view 200 times and checks the heap does not
  grow.
