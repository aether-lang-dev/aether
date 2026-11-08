- **An answered ask no longer waits on a preempted replier.** After its reply
  arrived, an ask waited, yielding, until the replier had let go of the reply
  slot. A replier preempted between waking the asker and letting go could hold
  it for a time slice or more: on an oversubscribed FreeBSD host, where
  `sched_yield` runs nothing from another run queue, asks waited up to 3.5 s
  and `test_ask_slot_reuse` timed out in about half of its runs under 8-way
  load. The ask now spins a moment and then lets go of its half; the replier
  frees the slot when it lets go, as for a timed out ask. 2400 of 2400 runs
  pass under the same load. Test: `tests/regression/test_ask_slot_reuse.ae`.
