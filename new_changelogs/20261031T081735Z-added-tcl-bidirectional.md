- **The Tcl host has the bidirectional embedding API.** `contrib.host.tcl`
  gains Lua's and Duktape's second tier (#904): a persistent VM that is its
  own interpreter (`vm_new` / `vm_free`), Aether callbacks registered as Tcl
  commands (`vm_register(vm, "app.add", fn)`, called `app.add 2 3`), global
  setters, typed arguments and returns (`arg_*` / `push_*`), and typed eval
  results, with a `TCL_TFLOAT` tag the other two lack. A callback may
  evaluate on its own VM: the nested eval runs at global level, inside the
  outer eval's limits, and gets `return`, `return -code error` and a stray
  `break` right. There are two guards: `vm_set_instruction_limit` (Tcl
  commands) and `vm_set_time_limit`. Only the second stops a loop that runs
  no commands, such as `while 1 {}`. The eval compiles the script whole,
  because Tcl checks its limits only in compiled code, where a top-level
  `while 1 {incr i}` run command by command never reached one. A VM is refused
  from any thread but the one that created it, as Tcl requires.
