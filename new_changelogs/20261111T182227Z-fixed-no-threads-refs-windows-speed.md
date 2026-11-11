- **The `no_threads_refs` test no longer times out on Windows.** It took
  five to six minutes under MSYS2, past the sweep's limit inside a full run:
  it parsed the Makefile once per variable (nine times, seven seconds each
  there), started a shell and a `tr` per source file and an `nm` per object,
  and wrote a dependency file and temporaries for each of 226 compiles. One
  `make` run per build now compiles the sources from rules of its own, with
  `-pipe` and no dependency files, and one `nm` reads every object: 324 s to
  143 s on the win11 lab VM, what is left being gcc itself.
