- **A struct literal's field is no longer renamed after a module `var` of
  the same name.** A module with `var size` and a struct field `size` had
  every `Font { size: 2 }` (in its own functions, and in a file that
  glob-imports it) lowered to a field `<module>_size` the struct does not
  have: the merge took the field initialiser for a write to the var. The
  field of a struct literal or a struct pattern is never a variable.
