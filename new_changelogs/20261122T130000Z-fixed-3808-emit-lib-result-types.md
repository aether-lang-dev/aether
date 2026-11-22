- **An `--emit=lib` export's result crosses as its own type, or the export
  gets no alias.** A result type the public ABI had no spelling for fell
  back to `int32_t`: a slice result did not compile, an actor reference (a
  pointer) was cut to 32 bits, and a `bit_set` lost its high 32 bits. Such
  a result now skips the alias with a warning, as such a parameter does;
  an enum crosses as `int32_t`, a `bit_set` as `uint64_t` and a bitstruct
  as its backing word, as parameters and results alike (#3808).
