- **`ae_toolchain_zig` no longer fails about one run in three on macOS.** Its
  steps 4 and 5 copied a different fake zig over the one `--check` had
  already run; macOS kills a Mach-O rewritten in place (its code-signature
  cache is for the old bytes), so `--check` said "`zig version` failed"
  instead of "reports 0.0.1". The test removes the file first, so each copy
  is a new one. The same failure is in `origin/main`.
