- **A refused binary import leaves nothing in the temp directory (#3485).**
  `ae` refused it from inside the stub it was writing, so on Windows the
  open file kept its `ae-binimport-<pid>-<n>` directory from being
  removed: one more for every refused build, over a thousand on one
  development machine. The stub is now closed and removed before `ae`
  exits.
