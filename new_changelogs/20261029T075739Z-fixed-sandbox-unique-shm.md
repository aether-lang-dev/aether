- **Sandbox grant transport is isolated and validated.**
  Linux and FreeBSD now allocate an exclusive shared-memory object per
  `spawn_sandboxed` call. An empty grant list starts the child with an active
  sandbox and nothing allowed (a compute-only worker: the pipes it was given,
  but no files, exec or network). Malformed lists, including CR/LF or `:`
  injection into a category or pattern, and lists that exceed the transport
  size (E2BIG) are refused with `-1` instead of being skipped or truncated,
  and the diagnostic now says "invalid grant list" or "grant list too large"
  rather than a shared-memory failure.
