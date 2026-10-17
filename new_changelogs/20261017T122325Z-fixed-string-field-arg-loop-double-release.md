- **A string field read in a loop as an argument beside a call is released
  once a pass.** `add(b, p.name, copy(p))` in a loop, the field read ahead of
  the call into a temporary that borrows it, was released at the end of the
  loop's body and again at the start of the next pass, so every string but
  the last lost a reference too many: a struct that had kept a copy of the
  name (a field set from the parameter) held freed memory once the
  originals went. The body's exit release now clears the temporary's flag.
  Found writing ae3d's shader evaluator (a struct value copied field by
  field).
