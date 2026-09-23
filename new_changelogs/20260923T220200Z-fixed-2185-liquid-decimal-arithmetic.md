- **`contrib.templating.liquid`: the arithmetic filters compute as Liquid
  does, with decimals and without overflow (#2185).** `plus`, `minus`,
  `times`, `divided_by`, `modulo`, `at_least` and `at_most` were 32-bit
  integer arithmetic, and a decimal operand was an error (before #1558 it
  was silently read as 0). They now work on exact decimals, as Liquid's
  Ruby numbers do:
  - A decimal on either side gives an exact decimal result
    (`0.1 | plus: 0.2` is `0.3`, `0.3 | divided_by: 0.1` is `3.0`), printed
    as Ruby prints the Float nearest to it (`10 | divided_by: 3.0` is
    `3.3333333333333335`, and `1.0e+17` and `1.0e-05` beyond the range Ruby
    writes in full).
  - Integers stay integers and no longer overflow
    (`99999999999 | times: 99999999999` is `9999999999800000000001`).
  - Integer division and modulo floor, as Ruby's do. They used to truncate,
    so `-7 | divided_by: 2` printed `-3` where Liquid prints `-4`, and
    `-7 | modulo: 3` printed `-1` where Liquid prints `2`.
  - `at_least` / `at_most` return the chosen operand in its own shape.

  `abs` and `round` print decimals through the same Float formatting.
