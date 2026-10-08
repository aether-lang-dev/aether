- **A C build no longer looks for system headers in std's directories
  first (#2552).** `ae` put every runtime and std directory on a program's
  include path ahead of the compiler's own, so each system header a program
  included (`<windows.h>` pulls in dozens) was first looked for in each of
  them. On Windows one such probe came back EINVAL, and gcc stopped the
  build. They now come after the compiler's and the system's directories
  (`-idirafter`), and only the directories that hold a header are listed: 98
  of an install's 224, which also makes the compile command shorter.
- **`ae version use` switches versions on Windows instead of hanging
  (#2529).** It copied the new version over the running `ae.exe`, which
  Windows locks, and robocopy retried that file a million times, 30 seconds
  apart, with its output discarded. Each file the switch replaces is now
  moved aside first (a running executable can be renamed), robocopy never
  retries, a failed copy puts the moved files back and says it could not
  write there, and the next switch deletes the leftovers.
- **`ae version install` says why a version cannot be downloaded (#2530).**
  A tag with no published release printed "Downloaded file not found": on
  Windows a failed download was reported as a success. It is reported as a
  failure now, and `ae` asks GitHub whether the tag has a release and
  whether that release has an archive for the platform, then names the
  latest release.
- **A dev-mode `./build/ae` compiles against its own std wherever it runs
  (#2487).** Run outside the repository with `AETHER_HOME` naming an older
  install, it compiled that install's std with the new compiler, because on
  Linux and macOS it did not replace the variable it hands to `aetherc`. It
  does now, as it always did on Windows.
