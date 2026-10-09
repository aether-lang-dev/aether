- **A string returned through a typed function pointer is the caller's, and
  the caller frees it.** A call through a `fn(...) -> string` pointer took
  its result as borrowed, but since 0.792 a `string` function returning a
  field returns a copy of its own, so every call through the pointer leaked
  that copy: ae3d's component copy (`setter(dst, getter(src))`) and JSON save
  leaked one string per text field. The call now decides at run time: a
  function used as a pointer that hands over owned strings marks the one it
  returns, and the call takes a marked result as it is and copies any other.
  A C function's string is never freed, however its pointer arrived (a cast
  from a raw `ptr`, a raw `ptr` passed for a typed parameter, an extern's
  result, a struct laid over C memory, an extern named as a value, a pointer
  C passes to a callback), and an Aether function handed to C is the
  function itself. An owned string argument to such a call is freed after
  it, position by position, under the convention closures follow (#2586).
