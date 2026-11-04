- **A typed message whose fields fit in the runtime's `Message` travels in
  it, with no heap copy (#3534).** Only a message of no field or one scalar
  travelled inline; every other send copied its struct to the heap and the
  receiver freed it, though a message of two ints, an int and a string, or a
  `Down` fits the 28 bytes (16 on 32-bit targets) `Message` carries anyway.
  Such a message is now copied into the message (`zerocopy.owned` is
  `AETHER_MSG_INLINE`), every path that handles, drops or records a queued
  message reads it there, and a send of one cannot fail for want of memory.
  A two-int send takes 25% less time (61 to 46 ns per message on the core
  send benchmark), a forwarded string 9% less (93 to 85 ns) and an ask 7%
  less (683 to 633 ns). C code that takes messages from a mailbox reads a
  typed payload with `aether_message_payload` and gives it back with
  `aether_message_free_payload`; the runtime ABI revision is 2.
- **Migrating: a hand-written C step function reads a typed message through
  `aether_message_payload` (#3534).** A message that fits now travels in
  `Message` itself (`zerocopy.owned == AETHER_MSG_INLINE`), so its
  `payload_ptr` holds the message's bytes, not a pointer to them. C code
  that read `msg.payload_ptr` as the payload, or freed it (an actor step
  handed to `http_server_set_actor_handler`, a host draining a mailbox),
  calls `aether_message_payload(&msg)` and
  `aether_message_free_payload(&msg)` instead. Generated code and every
  shim in the tree do; a library built for runtime ABI revision 1 is
  refused rather than misread.
