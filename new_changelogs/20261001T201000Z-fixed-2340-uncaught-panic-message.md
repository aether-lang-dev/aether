- **An uncaught panic prints a message built at run time.** An
  interpolated or otherwise heap-built panic message reached the runtime as
  an `AetherString`, and the uncaught fallback printed it as a C string. The
  line showed the string's header bytes instead of the text. The fallback and
  an actor's death hook now take the text out of the string, and a literal
  message prints as before (#2340).
