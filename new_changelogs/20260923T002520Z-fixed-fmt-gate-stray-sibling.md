- **An interrupted `fmt_gate` run no longer poisons the tree for every run
  after it.** Its IR tier copies each sampled file to a sibling *inside the
  source tree* — it has to, because imports resolve relative to the source
  file's directory — and removed it on the normal path and on the explicit
  failure paths, but not when the script is killed. A sweep that times out
  or is interrupted does exactly that, leaving a stray `.ae` under `tests/`
  that the next run samples and `ae fmt` then reports as unformatted. Found
  as a real leftover, not hypothetically. The cleanup now covers the
  signals a kill actually sends rather than `EXIT` alone, and a tree
  already carrying leftovers heals itself at startup, loudly, instead of
  failing the gate for a file nobody wrote.
