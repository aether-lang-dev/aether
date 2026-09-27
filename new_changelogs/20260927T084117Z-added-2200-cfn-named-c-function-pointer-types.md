- **Named C function-pointer types, `type Name = fn(T1, T2) -> R` and `cfn Name(a: T1, b: T2) -> R`.** OpenGL
  past 1.1 and all of Vulkan are reached through pointers fetched at run time
  (`wglGetProcAddress`, `vkGetDeviceProcAddr`, `dlsym`). Either spelling names one
  such signature once (the `type` alias as a use site writes it, `cfn` as the
  C prototype reads): `var gen_buffers: GenBuffers = null`, loaded with
  `get_proc_address("glGenBuffers") as GenBuffers`, is then called with C's
  calling convention from anywhere, with struct pointers, `float` and `f32` in
  the signature. It is the typedef form of `fn(T1, T2) -> R` and goes wherever
  that goes: a local, a parameter, a struct field, a return type, another
  `cfn`'s parameter, and across an import (`exports (GenBuffers, ...)`). Two
  gaps in the underlying machinery closed with it: a call through a
  module-level `var` holding a function pointer was emitted bare (C rejected
  it) and failed the checker inside an imported module ("Undefined function");
  and `f as fn(...)` rejected the address of a `-> float` or `-> string`
  function as "must be a ptr value". A third: a call through any C function
  pointer returning `bool` read the full `int` return register, but a C
  `bool` return defines only its low byte, so on Windows x64 `is_even(7)`
  came back true; the result is now read from that byte. (#2200)
