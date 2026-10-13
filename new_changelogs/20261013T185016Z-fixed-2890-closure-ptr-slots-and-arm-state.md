- **A closure goes into a `ptr` slot, and an arm's closure reads state.**
  A closure stored straight into a `ptr` module variable, a `ptr` field of
  a struct literal or a `ptr` message field emitted C that did not compile:
  the slot got the closure itself, where a `ptr` parameter and a `ptr`
  field store box it. Every such store now boxes it the same way (a bare
  function through its adapter), and a function reads a `ptr` global it
  stored a closure into back as a `ptr`. A closure made in a receive arm
  that read the actor's state did not compile either (`'n' undeclared`):
  it now captures the state's value when it is made, as it captures a
  local; writing state from a closure is still refused (#2890).
