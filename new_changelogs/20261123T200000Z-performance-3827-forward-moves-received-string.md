- **A handler that forwards a string it received moves it on instead of
  copying it.** `R(s) -> { peer ! M { s: s } }` copied `s` on every hop: the
  binding views the received message's field, which the message released
  after the arm. When the send is the binding's last use, exact by
  construction (the one read of the name in the arm, under no loop, closure
  or `defer`, and nothing in the arm binds the name), the send takes the
  reference out of the received message, which then releases nothing of it.
  Any other use still copies: a read after the send, a send in a loop, a
  closure that captures the binding, two sends of it.
  `send_string_forwarded` went from 75 to 40 ns per message (best of nine,
  Windows) (#3827).
