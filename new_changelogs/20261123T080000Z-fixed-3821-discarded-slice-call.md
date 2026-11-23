- **A discarded call that returns a slice compiles clean under clang's
  `-Wunused-value` again (#3821).** A statement such as `length_scratch()`,
  where `length_scratch() -> float[]`, was emitted on 0.937 as a comma
  expression ending in the temporary that holds the call's slice. Clang
  reported "expression result unused", and projects building with warnings
  as errors failed (ae3d on macOS, 37 suites). The statement is now cast to
  `(void)`, as other discarded values are.
