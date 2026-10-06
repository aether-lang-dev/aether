- **Interpreter dispatch loops lower to threaded dispatch (#2378).** A
  `while true { head; switch sel { case ...: { ... continue } } }` loop now
  compiles, under GCC and Clang, to a label table per loop: each `continue` that
  targets the loop repeats the head and jumps straight to the next arm with
  `goto *table[sel]`, the way actor message dispatch already did. No
  annotation; the shape is recognised (integer selector, constant cases 0..4095,
  a head of plain scalar statements, any code after the `switch`). A value with
  no arm still goes through the `switch`, so behaviour is unchanged, and MSVC or
  `-DAETHER_NO_THREADED_DISPATCH` gets the plain loop. Under `--emit=lib` every
  threaded dispatch checks the call deadline. `AETHER_EXPLAIN_THREADED=1` makes
  the compiler say why a loop of this shape was not threaded.
