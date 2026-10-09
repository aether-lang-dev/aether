- **An arrow body that ends in a local string returns it instead of freeing
  it first.** In `f(n) -> { msg = "count=${n}"; msg }` the trailing `msg` is
  the implicit return, but the parser wrapped the statement holding it rather
  than the expression, and the passes that decide whether a return hands over
  an owned string, and which local escapes through it, did not look inside a
  statement. So `msg` was freed at scope exit and the freed pointer returned,
  which printed as garbage, different at `-O0` and `-O2`. The implicit return
  now holds the expression, exactly as `return msg` does, and the caller frees
  the string (#2685; found by the -O0 against -O2 sweep of #2488).
