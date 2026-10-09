- **`contrib.host.tinygo` can take a `C.CString` result over instead of
  leaking it.** Every string-returning wrapper (`call_str_str`, `call_s_v`,
  `call_s_s`, `call_s_i`, `call_s_s_s`, `call_s_s_s_s`) has an `_owned` twin
  that copies the result into a string Aether owns and frees the library's
  pointer, for a Go function returning `C.CString(...)`, which cgo allocates
  with `malloc` and nothing freed: every call through the borrowing wrappers
  leaked one string, the module's own example included. The plain wrappers
  still borrow, for a static or long-lived result. The example and the README
  use the owned form, and a new test runs every `_owned` wrapper against a C
  stand-in for a c-shared library, with no Go toolchain needed, checking that
  owned calls leave the heap where it was; CI's contrib/host job now builds
  the bridge and runs it with the Go end-to-end test (#2569).
