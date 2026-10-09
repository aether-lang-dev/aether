- **A tuple function that fills a position from its own recursive call no
  longer leaks a string per level.** The position was classified while the
  function was still being analysed, so the recursive call counted as
  borrowed: the base case handed its parameter back, every caller kept its
  argument alive, and the outermost caller never freed the result. A
  position the function fills from its own call (destructured, or its
  whole tuple returned) now counts as one it hands over, so the base case
  returns a copy and callers free their arguments and the result (#2641).
