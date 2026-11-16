- **A send from another core no longer costs the receiving core's next
  step the lines it writes (#3540).** A sender reads the target's
  `assigned_core`, `dead` and mailbox bound and counts its message in
  `inflight`; the first two shared cache lines with the mailbox count and
  the step lock, which the core running the actor writes at every step, so
  each such send made that step fetch the lines back. The old scan of
  every actor hid part of this by reading them on each idle pass; with the
  ready list an ask from one core's actor to another's took 40 ns more from
  the dequeue to the reply. `ActorBase`'s fields after the mailbox are now
  in three cache lines: the stepping core's, what a sender touches, and
  what changes at spawn or death, at the same size. Dequeue to reply went
  from 117 ns to 66 (75 on the merge base), and `make bench-core`'s
  ask_from_actor from +9.6% against the merge base to -3.8%,
  ask_from_main from +11.0% to -1.3%.
