- **A send or an ask to a null actor reference no longer leaks a closure
  it boxed.** A closure stored into a `ptr` field of a message is boxed at
  the send, and the box is the receiver's to give back: the message's
  release leaves a `ptr` alone. A send to a null reference panics once the
  message is built, and the box, with its reference to the closure's
  environment, was left to no one, on a one-field message (the payload
  carries the box), on any other message and on an ask. Everything the
  message took is now given back before that panic: the fields it owns, by
  its release, and each box the send made (#3847).
