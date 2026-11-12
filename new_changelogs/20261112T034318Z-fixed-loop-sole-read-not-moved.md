- **A local read once, inside a loop, is no longer moved out on the first
  run.** A local's only read counted as its last use, so it was moved (left
  empty) there; in a loop body that read runs again and found nothing.
  `blank = mk(); for i in 0..n { cells[i] = blank }` filled only the first
  element, `x = "x"` stored into a `string[]` slice the same way, and
  `pattern = lastpattern` in a loop emptied `lastpattern` for the next
  time round. The same held for a struct literal's field, a field store
  and an inner loop. Such a read is now taken (retained or copied), unless
  the loop body binds the local unconditionally ahead of it, which still
  moves. Found by nib-code's `ae/term` port and the std.posix.regex AT&T
  conformance test.
