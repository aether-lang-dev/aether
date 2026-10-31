- **`contrib.host.python` gains the #904 bidirectional tier.** A persistent VM
  the host owns (`python.vm_new`), Aether `@c_callback` functions the script
  calls (`python.vm_register`; `"app.math.add"` makes the modules `app` and
  `app.math`), injected globals, and typed values both ways (`vm_result_*`,
  `arg_*`, `push_*`, `raise_error`), as Lua, Duktape, QuickJS and Tcl have.
  Python adds two tags: `PY_TBYTES`, which crosses as a `std.bytes` handle with
  embedded zeros intact and is never read as text, and `PY_TFLOAT`. `True` tags
  `PY_TBOOL`, not an int; an int past 64 bits tags `PY_TOTHER` and reads 0,
  never a wrapped value. `vm_eval` compiles an expression, or failing that
  statements, and runs the code once. A callback can evaluate on its own VM.
  `vm_set_instruction_limit` is a `PyEval_SetTrace` hook on the outermost eval,
  re-raised at every later event so a script cannot catch it and carry on; it
  does not bound a loop inside C. Every VM shares the one interpreter (and
  `sys.modules`), and every call takes the GIL, so any thread can use a VM. A
  host function that outlives its VM raises rather than reaching freed memory.
  `python.finalize` now restores the main thread state before `Py_Finalize`,
  never finalizes an interpreter it did not start, and a later `init` registers
  the shared-map helpers afresh.
