- **A library function that ends in a loop nothing leaves returns its zero
  value when a deadline or a cancel ends it.** Such a function needs no
  `return`, but under `--emit=lib` the safe point at each loop head leaves
  the loop, and the C then fell off the end of a function that returns a
  value: clang warned (`-Wreturn-type`, into the output of
  `examples/04-interop/07-limits`), and the value the export's wrapper read
  was undefined. The function now returns its result's zero value there,
  as the wrapper does.
