- **A float comparison beside a NaN keeps IEEE's answer under GCC -O1 and
  up (#3848).** GCC 15.2 and 16.2 take the false edge of `a > n` for the
  relation `a <= n` and chain such relations, which a NaN breaks: from
  `a > n` and `n > b` both false, with n a NaN, they fold `a > b` to false
  for a = 1, b = 0. A scalar `pick_max` over floats read out of lanes
  returned what `pick_min` returns. The generated C now compares floats
  (`<`, `<=`, `>`, `>=`, and a float match range) through the quiet
  comparison builtins, which give the same values and which GCC records no
  relation for; Clang and other compilers keep C's operators. Under GCC a
  scalar `if a < b { a } else { b }` is now a compare and a move rather
  than one minss.
