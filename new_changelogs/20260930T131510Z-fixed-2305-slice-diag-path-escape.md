- **A slice bounds check names a Windows source path correctly (#2305).**
  The check writes its source file into the generated C as a string
  literal, and the path went in unescaped. A Windows path's backslashes
  became escape sequences: gcc warned "unknown escape sequence" once per
  checked access (about 1,200 warnings building ae3d, which fails a
  warnings-as-errors build), and a `\a` in the path silently became a BEL
  character in the panic message. The path is now escaped like any other
  string in the generated C.
