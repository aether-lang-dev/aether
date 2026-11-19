- **A variable named after a libc function can be bound to a call with a
  trailing block.** `open = sandbox.new("open") { grant_env("X") }`
  declared `open` and pushed `ae_open` as the block's context: the push
  spelt the variable as a function named after libc is spelt (#1366), and
  the generated C did not compile. The same for a reassignment and a store
  to a module variable, and for every name on that list (`abs`, `remove`,
  `index`, `read`, `time`, ...). The push now names the variable as its
  declaration does.
