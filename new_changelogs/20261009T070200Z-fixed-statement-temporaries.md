- **A struct a call returns is destroyed after any statement that does not
  keep it.** Only an expression statement held such a struct in a temporary;
  in a declaration, an assignment, a tuple destructure or a `return`
  (`n = string.length(make_item(w).name)`, `x = count(make_item(w))`) its
  strings were owned by nobody and leaked on every call. Each of those
  statements now destroys it once done, and on a `return`, `break` or
  `continue` from inside its `or` handler, and so do an `if` or `while`
  condition, a `match` subject (once the match is done) and a `match` arm
  (#2582). A `string` a call may hand back from such a struct, as
  `first(make_item(w).name, 1)` does when `first` returns its parameter as
  it came, is copied where it is made, so the struct still goes with its
  statement (#2619). A tuple or pointer such a call returns, or anything a C
  function returns, keeps its argument alive instead.
- **A function or closure that hands its struct parameter back gives the
  caller strings of its own.** A struct parameter borrows its caller's
  strings, so returning it (directly, through an alias, in an `if` arm, in a
  tuple or inside another struct) handed back a view of the caller's
  argument, valid only while the argument lived. A parameter the body keeps
  as a whole value now takes its own references on entry (a counted string
  retained, anything else copied); one that is only read still borrows
  (#2582).
- **A struct local returned through an `if` is no longer freed under the
  caller, and one returned on one path is destroyed on the others.**
  `return if k > 0 { x } else { make_item("q") }` returned `x` and then
  destroyed it, so the caller read freed memory; the `if` now moves or copies
  the struct it picks. And the mark that holds back a returned local's
  destroy stayed for the rest of the function, so every later exit skipped
  the destroy and leaked the local's strings; it now belongs to its own
  `return` (#2612).
- **A closure returning a struct it captured hands back a copy.** It handed
  back its environment's struct as it was, and releasing the closure then
  freed those strings under the caller: heap corruption (#2617).
- **A struct assigned to a module-level `var` is taken, not shared.** The
  global held the strings its source freed at its exit; it now moves or
  copies them, as it does a string (#2616).
