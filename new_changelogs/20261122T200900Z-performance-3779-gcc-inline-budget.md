- **GCC gets an inlining budget sized for a whole program (#3779).** aetherc
  emits a program as one C translation unit, and GCC refuses every inline
  past 40% growth of a unit, a cap sized for an ordinary C file: in a large
  program the small hot helpers stayed real calls, `inline-unit-growth limit
  reached` on 2,671 call sites of aephysics. `ae` now passes GCC
  `--param inline-unit-growth=100` on optimising builds (not at -O0, not with
  `--size`, not to Clang), ahead of the project's cflags so a budget of its
  own wins. aephysics without its own inlining parameters: joint grid 15.18
  to 11.02 ms a step, large pyramid 7.06 to 6.37, ragdoll rain 5.62 to 4.67,
  the speed its hand-set parameters gave, in 1.41 MB against their 1.59 MB.
  See docs/build-system.md, "The inlining budget and GCC".
