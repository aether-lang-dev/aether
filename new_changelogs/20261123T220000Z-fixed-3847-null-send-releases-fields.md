- **A send to a null actor no longer leaks what its message owns.** A send
  or a `try_send` to a null actor reference panics (#3172), but the message
  was built by then: a string field held its own copy, or the fresh string
  the send moved in, and the panic left them to no one, one leak per send.
  The message is now released before the panic, as the receiver's handler
  end releases it, for `!` and `try_send` alike. A dead actor or a full
  mailbox was released already (#3847).
