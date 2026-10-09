- **`when target.os` / `target.arch` choose the arm for a cross build's
  target.** They always reported the machine running `aetherc`, so
  `ae build --target=x86_64-windows` on Linux took the `target.os == "linux"`
  arm, and every cross build saw the host's architecture. `ae build --target`
  now passes the target to `aetherc` (new `--target-os` / `--target-arch`
  flags, which reject unknown names), so the arm matches what `os.platform()`
  reports at run time on that machine.
