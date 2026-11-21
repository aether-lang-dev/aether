- **`contrib.quickjs`: an interrupted `Array.prototype.find` no longer frees
  an element twice.** In quickjs-ng 0.18.0, `find`, `findIndex`, `findLast` and
  `findLastIndex` released each element at the end of a step without clearing
  the variable, so when a deadline, cancel or stop interrupted the next step
  the error path released it again: the array kept pointing at a freed
  object, and the next garbage collection aborted in `gc_decref_child`. The
  fetched amalgamation is now patched with quickjs-ng's own fix (e22ce4de43,
  #1826, on its master after 0.18.0) from `contrib/quickjs/patches/`, applied
  by `scripts/fetch-quickjs-amalgamation.sh` after the checksum, so a source
  install, the release archives, CI and cross builds all get it. Reported by
  sae.
