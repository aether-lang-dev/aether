- **A caught panic frees a message built at run time.** Before this fix,
  every caught `panic("bad value ${n}")` leaked its message, because nothing
  owned the pointer. A heap-tracked local passed to `panic(msg)` was worse:
  the unwind journal freed it before the catch body read it. The catcher
  now owns a built message. The `catch e` binding is freed when its handler
  ends, unless the handler keeps it in an outer variable, returns it (alone
  or in a tuple), stores it, or raises it again. An actor that dies on such a
  panic releases the message after its death hook. The runtime gains
  `aether_panic_owned(reason, release)` for this, and a death hook's
  `reason` is now valid only during the call (#2333).
