- **A diagnostic keeps the words that say what is wrong, however long a name
  is in it (#3574).** The compiler formatted its messages into fixed buffers
  of 160 to 2048 bytes, and a long name formatted ahead of the explanation
  cut the explanation off: a local, a field, a function, a type or a module
  of 600 characters left a message of the name and nothing after it, in the
  checker, the free, protocol, ownership, escape, derive, missing-return and
  quote checks, the code generator and the module loader. Every message is
  as long as it is now. The few kept in a fixed buffer name only what is
  bounded (a keyword, a character) or put the names last, so a long one
  shortens a name and never an explanation.
