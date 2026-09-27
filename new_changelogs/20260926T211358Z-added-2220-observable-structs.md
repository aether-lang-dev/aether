- **Observable struct models: `struct T @observable` and `std.observe` (#2220).**
  A UI toolkit binding a view to a model had no way to hear that a field was
  assigned, so every app kept `refresh_*` methods and dirty flags in step
  with each store by hand. Now a struct declared `@observable` has every
  field store (`m.count = 1`, `p.status = "ready"`, the desugared
  `m.count += 1`) followed by a runtime call with the object's address, and
  the closures registered on that object through `observe.observe(obj,
  |obj: ptr| { ... })` run synchronously, in registration order;
  `unobserve(obj, token)` and `unobserve_all(obj)` remove them and release
  their environments. Identity is the address: a `heap.new` box as it is, a
  local value as `&m`. A nested value store notifies the inner value then
  the enclosing one; a store through a pointer field reaches the pointee
  only. Re-entrancy is guarded per object, so an observer may update the
  model it watches, and removal from inside an observer is deferred to the
  end of the pass. Observers live in a runtime side table, so the struct's
  layout is unchanged and an unobserved program pays one counter read per
  store. Per-field notification and cross-thread marshalling are left for a
  caller that needs them.
