- **A tuple destructure rebinds a closure local.** `f = || { return 1 }`,
  then `f, n = pair(5)` where `pair` returns a closure and an int, then
  `call(f)` ran the first literal again: the call was emitted as a direct
  call of that literal's function, as if `f` had never been rebound. A
  destructure slot now counts as a binding of the variable wherever the
  direct-call shortcut is decided, so the call goes through the closure
  the variable holds (#2903).
