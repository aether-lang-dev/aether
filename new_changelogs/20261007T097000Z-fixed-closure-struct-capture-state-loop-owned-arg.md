- **A closure that captures a struct reads its own copy of the strings, a
  state field summed in a loop is the field, and a returned closure passed
  straight to a call is freed (#2504, #2505, #2506).** A closure capturing a
  struct that owns strings copied the struct's bytes only, and the declaring
  scope's destroy freed the strings while a returned or stored closure still
  read them; it now captures a copy with strings of its own (`<Name>_dup`),
  destroyed with the closure. In a receive arm, a loop that adds to a state
  field (`while i < n { kept = kept + 1 ... }`) was rewritten into a closed
  form on a bare `kept` and a local of that name was hoisted, so the build
  failed; the rewrite now writes the field, a promoted capture's cell or a
  closure's capture as the loop would, with errors on the loop's own lines,
  and a state field is never hoisted as a local. A closure a function hands
  over and the caller passes straight to a call (`run(make_counter())`) is
  freed after the call when the callee does not keep it, and a local whose
  value was handed on still frees the closures it is bound to afterwards.
