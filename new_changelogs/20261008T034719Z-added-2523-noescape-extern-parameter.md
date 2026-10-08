- **`@noescape` on an extern parameter says the C function uses the
  argument only during the call (#2523).** `extern string_seq_each(s:
  *StringSeq, f: @noescape ptr)` declares that the callee neither stores,
  frees nor hands the argument to another thread, so the compiler treats a
  closure passed there as it does for an Aether callee that keeps nothing:
  a literal's environment is released right after the call, a local's at
  scope end, and the `ptr` slot's box is built on the caller's stack. An
  unannotated extern keeps the environment alive, since the callee may have
  stored it. The attribute is valid on `ptr` and `fn` parameters only. The
  string seq combinators (`seq_each`, `seq_map`, `seq_filter`, `seq_reduce`,
  `seq_zip_each`), `fs.walk`, `string_list_sort` and the `std.mem`
  function-pointer shims carry it, and their C sides no longer free the box
  or the environment: before, a capturing closure passed to them leaked its
  captured cells and strings on every call (the C side freed the environment
  without running its destructor), and a closure local passed to `seq_each`
  twice used freed memory.
