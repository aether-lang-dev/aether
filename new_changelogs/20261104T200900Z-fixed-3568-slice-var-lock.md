- **A module-level slice `var` is safe to read and store from several
  actors (#3568).** A store released the slice the var held before writing
  the new one, and a read copied the slice and then took a reference, so a
  reader could take a reference to a freed buffer or see a slice torn between
  two stores, and a push grew the var's buffer in place under a reader. Each
  slice `var` has a lock of its own now, as a string `var` does: a read takes
  a reference under it, a store swaps under it and releases the old slice
  after, and a push or an `append` grows a copy and stores it back.
