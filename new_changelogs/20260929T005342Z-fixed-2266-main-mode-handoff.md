- **A one-actor program no longer corrupts its heap when another thread
  first sends to it (#2266).** With one actor the runtime steps it inline
  on the main thread, and a send from a std.worker or std.http thread
  switches it to the scheduler (#2083). The inline send checked that the
  mode was on and then wrote the mailbox, and a switch landing between the
  two let a scheduler thread deliver into that mailbox as well. The inline
  step could then take a delivered message and leave the inline one, whose
  payload lives on the sender's stack, for a scheduler thread to process
  and `free()` after the frame was gone (`free(): invalid pointer`). The
  check and the enqueue are now one step with respect to every switch out
  of the mode (a foreign send, a handler's send to itself, a second
  spawn). The lock covers only the enqueue, never a step, so a thread
  leaving the mode does not wait on a handler.
