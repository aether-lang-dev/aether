- **`contrib.quickjs`: the interrupt is the host's, and `set_time_limit` is
  gone.** `set_time_limit(q, ms)` restarted its deadline at every entry, so
  a budget of JavaScript time across an eval and its job runs could not be
  expressed, nothing could stop a busy loop from another thread, and
  `to_string` (which never entered) ran under a stale deadline or none. Each
  runtime now has an absolute deadline on `quickjs.now_ns()`'s monotonic
  clock that the host sets and moves (`set_deadline_ns`, `clear_deadline`,
  `deadline_ns`), a cancel and a stop `std.sync` cell any thread may set
  (`set_cancel_cell`, `set_stop_cell`), and `interrupted(q)` saying which
  stopped JavaScript first (`INTERRUPTED_DEADLINE`, `_CANCEL`, `_STOP`;
  `clear_interrupted`). Every entry honours them, conversions that run a
  `toString`, `valueOf` or getter included. **Migration:**
  `quickjs.set_time_limit(q, ms)` before an entry is
  `quickjs.set_deadline_ns(q, quickjs.now_ns() + ms * 1000000)`, and
  `set_time_limit(q, 0)` is `quickjs.clear_deadline(q)` (nib-code ask
  aether-quickjs-async-eval-and-interrupt).
