- **A slice that holds no reference costs no unwind journal call (#3787).**
  A slice local bound to a call, a call's slice read in place or passed
  straight to a call, a loop's slice and a module `var`'s read were each
  journaled for a caught panic where they were set and forgotten at every
  exit, two out-of-line calls, even when the value was a view of memory no
  count covers (`p as T[]` returned from an accessor), for which a drain
  gives back nothing. Such a place is now journaled by its value's own mark
  (`AETHER_SLICE_MADE`, `AETHER_SLICE_STRING`): a view costs a flag test,
  and a store that brings a counted value journals the place then. A local
  the frame may change unseen (grown in place, its address taken, stored
  into inside a `try` it is declared outside of) is journaled from its
  declaration, as before. `aether_unwind_track` now returns whether it
  journaled, and every exit skips `aether_unwind_forget` when it did not,
  so a counted value outside every `try` costs one call, not two (runtime
  ABI revision 7).
