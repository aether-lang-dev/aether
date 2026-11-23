- **The narrowed `-idirafter` list covers response files and the Homebrew
  include directory.** A compile names only the list directories its
  includes reach, and gives every one whenever the scan cannot follow
  them. An `@file` response file in the cflags or in the `$AE_CC` / `$CC`
  command was not among those cases, though it can hold any flag that
  moves the search; it now gives every directory. And on macOS (Homebrew)
  and FreeBSD (LOCALBASE), a header found only in the include directory
  `ae` adds after the list was taken for a system header and not
  followed, so a std header it included lost its directory and the build
  failed; the scan now follows headers there too (#3841).
