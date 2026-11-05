- **The checker and the parser no longer drop what does not fit (#3574).**
  Each held a list of the program in a fixed array and compiled the rest
  wrong without a word: past 256 enums, distinct types, named C
  function-pointer types, sum types or `@c_struct` overlays, the rest did
  not resolve; past 64 owned pointer types the rest were plain pointers; a
  match on an enum or a sum of more than 64 members or variants was not
  checked for the ones past them, so a match missing one compiled; an actor
  arm past the 256th, a match key past the 256th and a comma-list value past
  the 256th were not checked; a block's defers past the 64th, isolated moves
  past the 128th and borrowed `fn` parameters past the 128th were not
  followed; a function's or a closure type's parameter modes past the 64th
  were dropped; a destructure into more than 64 places was refused; and the
  binding of a local past 256 pending nodes was not found. Each now grows
  with the program.
