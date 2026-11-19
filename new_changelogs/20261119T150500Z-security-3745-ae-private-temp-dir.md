- **`ae` keeps its temp files in a private directory, and an unset HOME no
  longer means `/tmp` (#3745).** `ae test`, an uncached `ae run`, `ae
  build`'s generated C, the REPL, `ae version doctor`, `ae bindgen`, cross
  builds and the compiler probes wrote C and binaries to predictable names in
  the shared temp directory (`$TMPDIR/_ae_<pid>.c` and the like), then
  compiled and ran them; another local user could make such a file first, or
  a symlink, and change it between the write and the compile. Each `ae`
  process now makes one directory with `mkdtemp` (mode 0700, a name no one
  can guess) on first use, puts every such file in it, and removes it when it
  exits; Windows uses the same layout under the user's own `%TEMP%`. With
  HOME unset, `ae` used to keep its cache, versions and packages under
  `/tmp/.aether` (`C:\Users\Public` on Windows), which any local user can
  write: it now takes the home the password database gives (HOMEDRIVE and
  HOMEPATH on Windows), and with none stops and asks for HOME to be set.
