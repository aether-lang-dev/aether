- **Type checking a module with thousands of externs or vars no longer
  slows with the square of its size (#3537).** Each call in a module body
  that resolved to an extern walked the module's top level for the file's
  own declaration and searched a list of the symbols made for them, each
  read of a var walked the program's vars, and each question of what a
  module declares walked the module. They are indexes now, built once per
  check: on a module of 4000 externs, vars and functions the type check
  takes 119 ms instead of 1025 ms.
