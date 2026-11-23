- **A message dropped instead of handled no longer leaks a closure its send
  boxed.** A closure stored into a `ptr` field of a message is boxed at the
  send, and the box was the receiver's handler's to give back: a message
  sent to an actor that had ended, queued to one when it ended or was
  released, refused by a full bounded mailbox or by `try_send`, or of a type
  its receiver has no handler for was dropped through its release, which
  left a `ptr` alone, and the box with its reference to the closure's
  environment leaked. The send now marks each box it makes in the message,
  the handler's binding of the field takes it, and the message's release
  gives back every box still marked, on every path where no handler took
  it, a send to a null reference among them. A one-field message that
  boxes a closure travels whole, still in the message's own bytes (#3847).
