- **`--target=riscv64-linux-musl` and `loongarch64-linux-musl`.** Two more
  self-contained zig cross targets (`loong64-linux-musl` is accepted too):
  static binaries for RISC-V 64 (rv64gc) and LoongArch 64, needing no
  sysroot. Run under `qemu-riscv64` / `qemu-loongarch64`, all 380 regression
  tests behave as the native build does, apart from differences shared with
  `x86_64-linux-musl` (musl and the container) and tests that spawn their own
  binary, which user emulation cannot exec. LoongArch needs QEMU 8.1 or newer;
  7.2 stops on an illegal instruction even for plain C. `os.arch()` and
  `target.arch` now name `loongarch64` instead of `unknown`.
