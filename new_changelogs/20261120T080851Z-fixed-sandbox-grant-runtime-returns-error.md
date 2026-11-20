- **`sandbox.grant_runtime` returns an error instead of panicking.** A name
  it does not know, or an interpreter it cannot ask for its paths (not on
  `PATH`, or exiting non-zero), panicked, and took a host process down with
  it (sae builds grant lists inside a long-lived browser). It now grants
  nothing and returns the reason, `""` when it granted the runtime's
  library: `why = grant_runtime("python3")` in the `sandbox.new` block. A
  list without the library fails closed, as before.
