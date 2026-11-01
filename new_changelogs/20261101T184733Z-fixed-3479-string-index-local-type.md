- **Binding one byte of a string to a local no longer leaves the local's type unresolved.**
  Type inference now types string indexing (`s[i]`) as `byte`, matching the language's
  byte read semantics and eliminating the warning in codegen that defaulted the local to `int`
  (#3479).
