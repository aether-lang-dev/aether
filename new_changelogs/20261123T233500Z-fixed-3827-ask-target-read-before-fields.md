- **An ask whose target panics no longer leaks its request.** An ask built
  its request (its strings copied, its shared fields published) and then
  read its target, so a target that panicked, an index past a fixed array's
  end or a call that panics, left the copies to no one. An ask now reads its
  target first, in source order, before any field is taken, as a send does,
  and a null target releases the request before its panic. A one-field send
  or ask reads its target first too, so a boxed closure in its field is not
  left behind (#3827).
