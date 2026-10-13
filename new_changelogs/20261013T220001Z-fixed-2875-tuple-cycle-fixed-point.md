- **Tuple functions that call each other agree on what they hand over,
  whatever the shape of the cycle.** When one of them returned the other's
  call as it was (`return pb(n - 1, w)`), the return classifier answered
  "not heap" for the function still being classified, the pass-through
  vetoed the error position the other built its error at, and every caller
  leaked the error, in either definition order. A cycle is now solved as a
  fixed point that starts from heap and drops a position only when the walk
  shows it cannot be, over every return, pass-throughs included (#2875).
