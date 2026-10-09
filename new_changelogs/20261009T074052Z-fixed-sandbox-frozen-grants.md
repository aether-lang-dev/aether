- **A sandbox can no longer be widened from inside.** `sandbox.enforce`
  checked against the caller's live grant list, and passed that list to the
  block, so code running inside the sandbox that held the list could call
  `sandbox.grant_env(perms, "SECRET")` (or any `grant_*`) and read what it had
  just granted itself. Nested sandboxes still intersected, so this could not
  exceed an outer sandbox, but the outermost one could be widened completely.
  `enforce` now freezes a private copy of the grants when the block starts,
  and the block's parameter is `null`; a grant added to the list mid-block
  applies only to the next `enforce` of it.
