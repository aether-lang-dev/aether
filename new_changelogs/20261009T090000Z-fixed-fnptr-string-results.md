- **A string returned through a typed function pointer is the caller's, and
  the caller frees it.** A call through a `fn(...) -> string` pointer took
  its result as borrowed, but since 0.792 a `string` function returning a
  field returns a copy of its own, so every call through the pointer leaked
  that copy: ae3d's component copy (`setter(dst, getter(src))`) and JSON save
  leaked one string per text field. A named function used as such a pointer
  now goes through an adapter that hands its result over owned (copied when
  it returns a literal or a borrow), the caller frees what it gets, and an
  owned string argument to such a call is freed after it, under the same
  convention closures follow (#2586).
