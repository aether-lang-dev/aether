- **The LD_PRELOAD sandbox holds `openat2` on glibc 2.42 and later.**
  glibc 2.42 added `openat2` as a function that makes the system call
  itself, so the sandbox's refusal of a raw `SYS_openat2` under a policy
  never saw it, and a program on a current glibc read a file outside its
  grants with it. It is now checked as `openat` is; with `RESOLVE_` flags the
  file it opened is checked again where it is, and `RESOLVE_IN_ROOT`, which
  the check cannot follow, is refused under a policy.
