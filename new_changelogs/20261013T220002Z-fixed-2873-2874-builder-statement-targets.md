- **A builder call with a trailing block runs its block whatever its result
  is stored into.** Each statement form ran the block itself, and the ones
  that stored into a module global or an actor's state did not: the builder
  ran with a null config and its block was dropped (#2873). A declaration of
  a struct a builder returns did not compile, since the declaration was
  emitted with ` = 0` to be assigned after the block (#2874). The block and
  the arguments now run ahead of the statement, once, for every target, and
  the statement is emitted as for any value, the call made with the filled
  config.
