- **On Linux a scheduler core names the actor it looks at with a plain
  store again (#3723).** The #3718 fix stored it seq_cst before every look,
  an `xchg` on x86, and a send that wakes its actor paid it once per
  message (`send_string_fresh` +6 to 11% on Linux). The rare deregister now
  pays instead, with `membarrier`; Linux falls back to the seq_cst store
  when the kernel refuses it. Other systems keep the seq_cst store, which on
  ARM64 is already cheap and on Windows measured no slower.
