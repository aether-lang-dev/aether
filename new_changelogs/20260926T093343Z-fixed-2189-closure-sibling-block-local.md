- **A closure's own local is no longer compiled as a capture of a same-named
  variable from a sibling block.** A trailing block (`spec.describe(fw, "first")
  { ml = ... }`) inlines as a C block, so its `ml` is out of scope by the time
  a later sibling block's callback binds its own `ml`. The capture analysis
  treated every trailing block as transparent to the enclosing function and
  matched the two on name alone, emitting C that referenced an undeclared
  name. A `name = expr` in a closure now captures only a binding that is
  visible from the closure, in the enclosing scope's own top level or a
  trailing block the closure sits inside, and that precedes it (#2189).
