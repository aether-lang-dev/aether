- **`std.guest.child` gives a Host back (#3547).** `child.open` and
  `child.attach` allocate the `Host` a worker talks to its host through,
  with its list of mounts, and nothing freed either, so every Host leaked,
  six in the module's own tests. `child.free(h)` gives back the Host, its
  mounts and its strings, and leaves the channel open: the socket given to
  `attach` is its caller's, the inherited one the process's.
