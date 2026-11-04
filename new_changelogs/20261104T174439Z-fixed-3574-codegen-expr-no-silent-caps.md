- **Calls, operands, actor closures and messages past the old slot counts
  compile right (#3574).** The expression code generator held what a call
  or a construct has in fixed arrays: a call's fresh arguments past the
  16th were never freed; a call of more than 32 arguments, a struct or
  message literal of more than 32 fields or a list of more than 32
  elements was left in C's unspecified order, so `f(bump(), ..., count)`
  could read `count` before the bump; a closure in an actor handler
  writing a state field past the 64th was not refused, and wrote a stale
  local; a closure of more than 16 parameters called through a bare `fn`
  got its arguments uncoerced; and a message's array fields past the 16th
  were sent as compound literals the receiver read after the send's block
  had ended. Each now grows with the program.
