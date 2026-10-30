- **The embedded Python host now restores nested shared-map contexts.**
  `python.run_sandboxed_with_map` preserves the caller's map token across a
  nested hosted run and still clears the token when the outermost run returns.
  A rejected over-depth call no longer replaces the active map context.
