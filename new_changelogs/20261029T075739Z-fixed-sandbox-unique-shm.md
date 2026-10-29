- **Sandbox grant transport is isolated and validated.**
  Linux and FreeBSD now allocate an exclusive shared-memory object per
  `spawn_sandboxed` call. Empty grant lists run as deny-all policies, while
  malformed or oversized lists are rejected instead of being skipped or
  truncated during serialization.
