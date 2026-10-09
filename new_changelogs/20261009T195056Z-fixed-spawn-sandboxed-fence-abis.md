- **`spawn_sandboxed`'s kernel fork fence covers every ABI and more
  architectures.** The seccomp filter trapped x86_64 syscall numbers and
  allowed every other ABI, so a static 32-bit binary (i386 ABI, out of
  `LD_PRELOAD`'s reach too) forked freely inside a sandbox with no `fork`
  grant, and on arm64, riscv64 and loongarch64 there was no kernel fence at
  all. The filter now traps clone/clone3/fork/vfork with each ABI's own
  numbers (x86_64 with x32 and i386, aarch64 with 32-bit ARM, riscv64,
  loongarch64), kills any other ABI, and `spawn_sandboxed` refuses to run the
  child on an architecture it does not cover instead of falling back to the
  libc fence alone. `sandbox_clone_fence` checks the i386 case live and every
  syscall number against the kernel headers for its architecture.
