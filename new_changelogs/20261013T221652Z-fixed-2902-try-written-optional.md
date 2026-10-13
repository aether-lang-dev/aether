- **An optional or sum local a `try` body writes keeps the value after the
  catch.** Every other variable a `try` body writes is declared `volatile`,
  so the panic's longjmp cannot leave it in a register; an `int?`,
  `long?`, struct or `fn?` optional local, and a sum local, were not, and
  at -O1 and -O2 the catch and the code after it read the value from
  before the `try` (an `int?` gave -1 for 50), on both C paths. Their
  declarations now take the qualifier (#2902).
