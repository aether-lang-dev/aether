- **A panic out of a sandboxed block no longer leaves the sandbox in force.**
  A panic that unwound out of a `sandbox_push` / `sandbox.enforce` block
  skipped the pop, so code after the catching `try` was still held to the
  block's grants. In a program that uses the sandbox, a `try` now records
  the sandbox depth and its `catch` restores it: a catch outside the block
  is no longer sandboxed, and a catch inside it stays sandboxed.
