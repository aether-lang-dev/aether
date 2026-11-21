- **A listing the LD_PRELOAD sandbox refuses reads as the kernel's own
  `EACCES` in `glob`, `nftw` and `ftw` too.** `opendir`, `scandir` and the
  opens already failed as an unreadable directory does, but a walk or a glob
  of one did not: `nftw` and `ftw` returned -1 without calling the function,
  where the kernel's refusal calls it with the directory as `FTW_DNR` and
  returns its answer; `glob` returned `GLOB_ABORTED` without calling the
  error function and whatever the flags, where the kernel's refusal calls it
  with `EACCES` and goes on to `GLOB_NOMATCH` (or the pattern itself under
  `GLOB_NOCHECK`) unless `GLOB_ERR` is set. A refused root is now handed to
  the walk's function as the kernel's refusal hands it, and `glob` lists
  every directory, not only the first, through the checked `opendir`
  (`GLOB_ALTDIRFUNC`), so the C library answers a refusal exactly as it
  answers the kernel's. A new test runs one program against a `chmod 000`
  directory and under the preload and compares the two outputs byte for
  byte.
