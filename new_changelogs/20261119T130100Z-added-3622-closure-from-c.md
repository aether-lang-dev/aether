- **A C bridge builds an Aether closure with the runtime's constructor.**
  `include/aether_closure.h` declares `aether_closure_new(fn, env, release)`,
  `aether_closure_retain`, `aether_closure_release`, `aether_closure_env` and
  `aether_closure_self_env`. The closure is one Aether opens with
  `unbox_closure`, calls with `call` and gives back with `release_closure`,
  in the same reference count as a closure Aether made: `release(env)` runs
  once, when the last reference goes. Bridges such as html-sanitizer's
  copied codegen's private box and env header to do this, and broke at run
  time, with nothing at compile or link time, whenever that layout changed
  (#1439, #2494). The layout is now defined once, in
  `runtime/aether_closure_abi.h`, which generated code and the runtime both
  include, and callers of the constructor only ever hold opaque pointers
  (#3622).
