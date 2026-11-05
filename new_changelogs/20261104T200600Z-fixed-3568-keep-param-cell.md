- **A `@keep` slice parameter a closure assigns keeps its buffer for the
  closure (#3568).** The cell the closure shares the parameter through was
  seeded with a copy, and the parameter's own release at the function's
  return emptied that cell: the closure grew an empty slice, and the
  reference the caller gave the parameter was never given back. The cell
  takes over the parameter's reference now, and its last holder gives it
  back.
