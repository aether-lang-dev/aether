- **A function of more than 16 parameters works as a `fn` value (#3574).**
  The adapter that lets a named function stand in a `fn` slot took at most
  16 of its parameters, so a wider function used as a value did not
  compile. A function indexing more than 8 slice parameters now picks its
  view copy on all of them, and every parameter in scope shadows a
  function of its name, past the old 256.
