- **`xs.push(f())` keeps the strings of the struct `f` returns.** The
  statement collected the call's result as a temporary of its own and
  destroyed it once done, though the new element had taken it, so the element
  read freed strings (`[x hi]` printed `[]`). Binding the result to a local
  first, or pushing a struct literal, was unaffected (nib-code ask
  push-call-result-string).
