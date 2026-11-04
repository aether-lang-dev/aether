- **A slice local a closure assigns owns the buffer it holds (#3549).** The
  local is a cell the frame and the closure share, and it was left to the
  program: a store into it from the closure dropped nothing it replaced, and
  the cell's release freed the cell and not the buffer, so a `make` buffer
  held there leaked on every call. Now a store into the cell, in the frame or
  the closure, drops what it held, the cell's last release drops what it
  holds then, and a `return` of the local gives the buffer to the caller. A
  local whose buffer a view may outlive (`w = v; return w`) is left to the
  program as before, so no view reads freed memory.
