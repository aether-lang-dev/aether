- **A local lasts to the end of its function, and the checker says so
  (#3872).** A local first bound in a loop or `if` body hid the function of
  its name to the end of the function in the generated C, but the checker
  let a call of the function after that body through, and it failed in the
  C compiler on an undeclared `ae_local_<name>`. Such a call is now E0301,
  as one after a binding in the function's own body was. A closure's own
  local no longer hides the function after the closure: codegen renamed the
  call there too, the same C failure.
