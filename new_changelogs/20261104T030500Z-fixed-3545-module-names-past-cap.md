- **A module with more than 4096 functions or constants merges every one of
  them (#3545).** The merge read a module's names into fixed lists of 4096
  and silently stopped there, so a merged body's reference to a name past
  the cap stayed bare: reading the 4097th `var` failed "Undefined variable",
  and a bare call of a later function the program also defines reached the
  program's function instead of the module's. The lists are now as long as
  the module, which generated binding modules need.
