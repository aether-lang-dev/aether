- **A call that returns several values, passed where a parameter takes one,
  is the checker's error.** `takes(list.get(xs, 0))` or
  `spec.expect_str(pair())`, with the multi-value call written straight into
  the argument, passed `ae check` and failed in the C compiler ("passing
  '_tuple_ptr_string' to parameter of incompatible type 'const char *'").
  The same mistake through a name bound to the tuple was already refused; the
  call form, including a UFCS receiver (`pair().takes()`), now is too (E0200),
  naming the call and the fix: destructure it first.
