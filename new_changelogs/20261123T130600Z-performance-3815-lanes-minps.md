- **`lanes.min4`, `max4`, `min8` and `max8` are one `minps` / `maxps` on x86
  (#3815).** Each was a compare and a blend, which GCC never turned into the
  one instruction, so a running minimum carried both on its loop chain. On
  SSE they now lower to `minps` / `maxps`, and an eight-lane value held in
  one AVX register to `vminps` / `vmaxps` on ymm, with the operand order
  that is exactly `a < b ? a : b` (NaN and signed zeros included), so every
  result keeps its bits. Other targets keep the compare and select. A
  running `min4` or `min8`: 1.5 to 0.76 ns a step. The horizontal
  reductions stay as they are: a shuffle tree was 2.2x faster on throughput
  but 3% slower on a dependent chain.
