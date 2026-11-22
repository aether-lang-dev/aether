- **A test that a worker's own `serve_exports(vm, h)` in a `child.run(h)`
  callback is still its own.** The combined tree has both the receiver rule
  (the program's name comes first) and `std.guest.child.serve_exports`; the
  test in `tests/integration/dsl_receiver_program_shadows/guest_child.ae` runs
  the real module over a socket pair and holds that the program's function is
  the one called.
