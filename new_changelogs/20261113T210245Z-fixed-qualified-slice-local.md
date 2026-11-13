- **A typed local of a qualified type's slice or array parses.** `m.P[] xs =
  []` and `m.P[4] ps` stopped at the `[` with "Expected statement in block":
  the statement dispatcher took a qualified type name followed by `[` for an
  expression, though `P[] xs = []` and a `m.P[]` parameter or field were
  accepted (nib-code ask qualified-slice-local).
