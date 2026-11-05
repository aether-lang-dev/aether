- **A name of any length resolves (#3569).** The parser and the checker
  joined the names they build into fixed buffers of 64 to 512 bytes, and a
  longer name was cut to another one, which then named nothing: a call
  `mod.<name>()` whose name ran past about 250 bytes was "Undefined
  function", and so were a qualified constant, a function bound by a
  selective or aliased import, a tuple result destructured from a qualified
  call, an enum's member, a function of a DSL block's receiver, an
  `@extern("sym")` or `@c_callback` symbol, a `select_alias`, a module path
  and the drop function of an owned type past their buffers; the effect
  check did not follow a call into a module whose namespace ran past 64
  bytes. Every name they build is now as long as it is.
