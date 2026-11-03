- **`contrib/host/stunt` no longer reports "refused" when nothing refused.**
  Several checks counted an outcome as a sandbox refusal that was not one,
  so a sandbox failing open could pass: ENOSYS from `syscall()` (the
  preload's answer when its grants never applied), any host-function error
  (a missing fixture, a failed read, a nil `getenv`, no such function), an
  unset `HOME` in `getenv-home` and `env-cached`, and EPERM at `connect()`
  (a firewall's drop, not the sandbox's). A host error is now a refusal only
  when its reason starts `refused:`, a missing variable only with evidence
  it was withheld, and a connect only on EACCES; the rest are errors. The
  connect checks go to TEST-NET-1 (`192.0.2.1`) instead of `1.1.1.1`, so a
  Linux `make ci` no longer sends SYNs to Cloudflare. A new fork-only policy
  tests the execve refusal itself (until now the fork was refused first),
  `dlopen-libm` runs as the control for the dlopen rows, `silent`,
  `half-frame` and `spin` can no longer outlive a broken host, a missing
  `libaether_sandbox.so` on Linux is a `[SKIP]` (a failure on GitHub), and
  the scripts build only when a check applies. Thanks to Nic for the review
  of #3513.
