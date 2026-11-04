- **A send that its protocol allows in no state is refused in a loop too
  (#3575).** `w ! Write {}` on every turn of a loop, with `w` in `start`
  where only `Open` is taken, compiled, while the same send outside a loop
  was refused: the protocol check took a reference sent a message wrong
  from every state to unknown, the loop's head joined to unknown, and the
  walk that reports, from the head, refused nothing. Such a reference is
  now in no state, which the head's join leaves out, so the send is refused
  where it is; the next send through it leads where the protocol says, as
  before.
- **Migrating:** a loop that sends a message its protocol never allows in
  the state the reference is in is refused, as the same send outside the
  loop always was. Send what the state takes first (`w ! Open {}` before
  the loop), or send through a bare `actor_ref`, which follows no protocol.
