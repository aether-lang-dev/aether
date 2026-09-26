- **A `string` passed through a typed function pointer reaches C as its
  bytes (#2210).** A call to an extern wraps a `string` argument in
  `aether_string_data`, so a heap string (interpolated, concatenated) arrives
  as its characters. A call through a `fn(..., string) -> R` pointer passed
  the value as it was, so the callee read the `AetherString` header instead;
  it compiled without a warning. ae3d hit it calling OpenGL through cached
  function pointers: `glGetUniformLocation(program, name)` returned -1 for
  every uniform name built at run time (`lights[${i}].position`) while a
  literal worked, so the lights were never set and every frame came out
  black. The three typed-pointer call shapes now unwrap the way an extern
  call does: a cast local (`f = p as fn(uint32, string) -> int`), a
  `fn(...)` parameter, and a function-pointer struct field, by value or by
  pointer. A `ptr` parameter still passes the value as it is.
  `tests/regression/test_fnptr_string_arg_bytes.ae` measures a heap string
  through each shape with libc's `strlen`.
