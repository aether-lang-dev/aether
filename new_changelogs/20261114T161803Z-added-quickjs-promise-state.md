- **`contrib.quickjs`: `promise_state` and `promise_result`.** A host holding
  a promise handle can ask how it stands (`quickjs.PROMISE_PENDING`,
  `PROMISE_FULFILLED`, `PROMISE_REJECTED`, or `PROMISE_NOT_A_PROMISE`) and
  read what a settled one holds, its value or its rejection reason, without
  running JavaScript; before, the only way was a `.then` that wrote to a
  global (nib-code ask aether-quickjs-async-eval-and-interrupt).
