- **A `for` loop that sends from outside an actor batches its sends, as a
  `while` loop does (#3826).** Only `while` loops opened a send batch (#3117),
  so `for i in 0..n { a ! M{} }` in main took the foreign send path for each
  message: an entry count, the inflight count, the foreign lock and a wake
  fence every time. A `for` loop now opens a batch under the same guard (no
  call, print, ask, actor field read, `return` or labeled exit in it) and
  sends it where the loop ends. `fanin_main_burst` (400k sends from main to
  one actor): 148 to 114 ns a message on Windows, where the `while` form also
  takes 114.
