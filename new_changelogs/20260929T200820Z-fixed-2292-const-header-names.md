- **A constant may be named like anything a C header declares (#2292).**
  The program's constants were file-scope statics under their own names, so
  on Windows `const ACCEL = 4.0` collided with winuser.h's `ACCEL` type and
  `const NEAR = 20.0` with a windef.h macro, and the generated C did not
  compile. `SIZE`, `POINT`, `RECT` and `MAX_PATH` failed the same way; the
  last one silently, where a reference compiled against the header's value.
  The program's constants are now emitted as `ae_const_<NAME>` with their
  references, so no header can collide with them on any platform. A
  parameter, local, closure parameter or actor state named like a constant
  is still that binding. The symbol catalog of an `--emit=lib` build still
  names constants as written. The program's own C translation unit also
  leaves USER and GDI out of `windows.h`, since it calls only a few kernel32
  functions. That removes `ACCEL`, `MSG` and every `CreateWindow` /
  `SendMessage` / `GetObject` macro from its scope.
