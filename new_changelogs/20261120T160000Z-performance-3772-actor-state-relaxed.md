- **An actor's `int`, `long` and `Duration` state is no longer a seq_cst
  atomic in its handlers.** The fields are atomic so another thread's read
  is never torn, and handlers used them with plain C operators, which on an
  atomic is seq_cst: every write to state was a locked exchange on x86, and
  no value stayed in a register. Handlers now read a field relaxed and store
  it with a release, a plain store on x86, so a thread outside the actor
  that reads one field still sees every write the handler made before it.
  A handler loop that adds into a state field went from 6.00 to 0.33 ns per
  iteration (`actor_state_loop`, a new row of `make bench-core`). `x op= y`
  works on a copy with every rule it has for a local (a float right side
  saturates, `%=` by a float is fmod), and `x++` is a load and a store, no
  longer an atomic read-modify-write: the actor's step is the field's one
  writer (#3772).
