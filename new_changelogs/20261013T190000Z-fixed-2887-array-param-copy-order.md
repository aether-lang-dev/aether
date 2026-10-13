- **A call that writes its fixed-size array parameter no longer orders the
  caller's reads of the array.** A `T[N]` parameter is the function's own
  copy (as E0900 treats it), so a store into it, or into a slice of it by a
  function it calls, changes nothing the caller reads. The evaluation-order
  rule counted it as a write through the parameter, and
  `first(arr, 9) + arr[0]` held `arr[0]` in a temporary for nothing. The
  results were already right; the C is now what it was without the call's
  write (#2887).
