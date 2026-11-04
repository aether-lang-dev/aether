- **The code generator no longer cuts or drops what does not fit (#3574).**
  A builder call nested more than 64 deep (a builder recursing inside its
  own block) had its block's calls work on an outer block's config; a
  builder whose factory has a long name left its block by a `return` into
  a C call of a cut name; a C function pointer type (`cfn`) whose spelling
  passed 1024 bytes did not compile; an `--emit=lib` export of more than
  32 parameters lost its alias, with a warning that a parameter type is
  not representable; a library id longer than 127 characters was cut in
  the symbols named after it; a library whose exports pass a struct of a
  name longer than 199 characters, or reach a string more than 64 structs
  down, did not name its own runtime among an importer's duties; and in a
  module imported at `@target`, a call of a function of a name longer than
  511 characters, a struct holding lanes more than 24 structs down, or a
  copy using more than 512 lane structs passed lanes between the copies
  unrefused or unremapped. Each now grows with the program.
