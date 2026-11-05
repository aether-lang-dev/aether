- **A closure's push into a slice it captures is seen by its frame
  (#3568).** `add = |v: int| { xs.push(v) }` grew a copy of `xs` that
  nothing released, a leak on every call that grew with the slice. A push
  is a write now, as an assignment is, so `xs` is shared with the closure
  the way an assigned capture is: each push grows it, and the frame sees
  what was pushed.
