- **A call of a clause set is ordered by what every clause does.** The
  evaluation-order rule read a set's effects off its first clause, so a
  write through a pointer in a later clause, or in a guard, went unseen:
  `show(q.n, wr(p, 3))` read `q.n` after `wr(p, 3)` wrote it and printed
  5002 for 1002. The set's entry now reads every clause's body, guard and
  contracts, each against its own parameters, counted by position (a
  literal pattern ahead of a pointer no longer shifts which argument is
  written through). Sets whose clauses write nothing still get no
  temporaries (#2857).
