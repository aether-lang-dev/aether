- **`contrib.quickjs`: `eval_async`, a script with top-level `await` that
  keeps its completion value.** `quickjs.eval` refused `await` at top level
  and `quickjs.eval_module` allows it but a module has no completion value;
  `quickjs.eval_async(q, code, filename)` is a global eval with
  `JS_EVAL_FLAG_ASYNC`, returning a promise that settles to
  `{ value: <completion> }` (nib-code ask
  aether-quickjs-async-eval-and-interrupt).
