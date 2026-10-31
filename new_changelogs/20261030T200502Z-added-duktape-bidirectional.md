- **The Duktape host has Lua's bidirectional embedding API.**
  `contrib.host.duktape` (and its `contrib.host.js` alias) gains a persistent
  VM (`vm_new` / `vm_free`), Aether callbacks the script calls by dotted name
  (`vm_register(vm, "app.math.add", fn)`), typed arguments and returns
  (`arg_*` / `push_*`), global setters, and typed eval results with the same
  type tags as Lua (#904). A callback's `return duktape.raise_error(vm, msg)`
  throws into the script after the callback has returned, so its defers still
  run. Numbers that are not an int64 (NaN, out of range) read as 0 or
  saturate rather than cast with undefined behaviour, and a thrown non-Error
  still reads as an error result. `vm_set_instruction_limit` reports that the
  limit is unavailable: the system libduktape this host loads is built
  without `DUK_USE_EXEC_TIMEOUT_CHECK`.
