- **An actor's `int`, `long` and `Duration` state is read and written relaxed
  in its handlers.** The fields are atomic so another thread's read is never
  torn, and handlers used them with plain C operators, which on an atomic is
  seq_cst: every write to state was a locked exchange on x86, and no value
  stayed in a register. A handler loop that adds into a state field went
  from 5.86 to 0.32 ns per iteration (`actor_state_loop`, a new row of
  `make bench-core`). `x op= y` and `x++` on such a field are a relaxed load
  and store, no longer an atomic read-modify-write: the actor's step is the
  field's one writer. A read from outside the actor is unchanged (#3772).
