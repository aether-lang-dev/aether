- **Functions of many locals and deeply nested blocks compile right (#3574).**
  The statement code generator and the generator's own stacks held what a
  function has in fixed arrays: a function of more than 256 string locals,
  or of more than 256 `string?` locals, leaked what the rest held (past 256
  pending releases they were dropped with only a warning); a function of
  more than 64 owned-pointer locals left the rest unreleased; a `defer`
  reading more than 64 locals stopped the compile; a `match` of more than
  256 variant arms with a second arm for one past them did not compile;
  string locals bound more than 64 blocks deep were released in the wrong
  blocks; a `break` in a switch nested more than 64 switches deep left
  an outer switch; and a statement of more than 64 values, or loops nested
  more than 64 deep, lost track of what they hold. Each now grows with the
  program.
