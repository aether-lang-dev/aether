- **Using a slice after `append` or `free` consumed it is E0906 again
  (#3568).** Once a slice local had been given to a function or a view of it
  taken, `ys = append(xs, 5)` and `free(xs)` still emptied `xs`, but the
  check took it for copied and let `xs.len` through, which then read 0. The
  check follows the consume again, as it did before slices were counted.
