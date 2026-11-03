- **A `std.mutation` run no longer leaks its argument lists and counters.**
  Each mutant's oracle built three argument lists for `os.run_full`, which
  borrows them, and nothing freed them; `run` kept its counts in three ref
  cells and the survivors in a list, freed on no return. A mutation run
  leaked all of them, and more with every mutant.
