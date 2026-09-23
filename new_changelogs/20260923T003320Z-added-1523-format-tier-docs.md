- **Worked examples for the six Format-tier `std` modules (#1523):**
  `bits`, `hash`, `bignum`, `msgpack`, `cbor` and `yaml`. Five are
  compiling, running programs whose output the doc gate checks; `yaml`'s is
  compile-checked instead, because the module needs libfyaml at build time
  and its output depends on how the toolchain was built — the section says
  so rather than shipping an example that fails for a reason the reader
  cannot see.

  Each section states what is easy to get wrong rather than only listing
  functions: that `std.bits` exists because Aether's `int` is signed, so
  `>>` propagates the sign bit; that a `std.hash` function other than
  SipHash has no secret, so an attacker who knows which one you use can
  pick keys that all land in one bucket; that every `std.bignum` operation
  returns a **new** handle needing its own `free`, and that its
  public-key set is correct but not constant-time; and that `cbor.stringify`
  is an alias for `encode` and returns bytes, while `diagnose` is the
  readable form.

  With the Common tier (#2161), that is 14 of the 37 modules the issue
  listed. Systems and Niche remain.
