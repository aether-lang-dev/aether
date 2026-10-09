- **`import contrib.host.tinygo` links on Windows when the bridge was built
  with libffi.** `ae` adds `-lffi` for a bridge whose archive references
  libffi, and found out by running `nm -u <archive> 2>/dev/null | grep -q
  ffi_prep_cif` through `system()`. On Windows that is cmd.exe, which has
  neither `/dev/null` nor `grep`, so the probe failed on every build and the
  link stopped at `undefined reference to ffi_prep_cif`. `ae` now reads the
  archive itself and looks for the symbol in its string tables, on every
  platform, with no external tool (#2686).
