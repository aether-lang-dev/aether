- **The passes around the checker take a long name, a long path and a wide
  call graph whole (#3569).** Each built into a fixed buffer or list and
  cut what did not fit: `@derive(eq)` and `@derive(schema)` named their
  helper after a struct name cut at 250 bytes, so a call of `<Struct>_eq`
  named nothing; a heredoc's marker past 255 bytes never matched its
  closing line; a contract did not fold a member of an enum whose name ran
  past 256 bytes, so a call that breaks a precondition compiled; the slice
  escape check cut field paths at 256 bytes and refused returning a field
  for another one whose name shared its first 250 bytes; `sandbox.enforce`
  trusted a module's C extern reached under a name past 512 bytes; and the
  effect walks kept the functions they entered in 512 slots: an effect tag
  on a function calling more was refused for a limit instead of for what it
  reaches, and `__pure` stopped following there and said pure, as it did
  for the module globals past the first 256 in `--emit=effects`. Past the
  walks' depth bound, `__pure` now says impure and `--emit=effects` says the
  function may reach anything, where they said pure and reported what they
  had seen.
