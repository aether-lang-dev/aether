- **Nested loops compile in time linear in their depth (#3574).** The free
  check, the slice ownership and escape checks, the isolation check and
  the protocol check walk a loop's body two or three times, and each walk
  walked the loops inside it again, so the time doubled with each level of
  nesting: 20 nested loops took seconds and 70 did not finish. A loop
  walked again from the same state now reuses the walk already made, with
  the joins it made into the loops around it, and 96 nested loops check in
  a few milliseconds. The protocol check's join is now a proper join: a
  reference that reaches a point under two protocols is followed on no
  path from there, however the paths are joined, where it was forgotten
  and could come back under one of them.
