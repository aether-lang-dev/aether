- **Effect-tag validation now fails closed at analyzer limits.** A call graph
  deeper than 128 levels or wider than the 512-function visited set is
  rejected instead of treating the unexamined path as capability-free.
