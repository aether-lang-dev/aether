- **Under `seal except` or `hide`, a tuple destructure binds a new local as
  a plain binding does.** `r, err = f()` was refused (E0304 'r') when `r`
  also named a top-level function, constant or extern, though `r = f()` in
  the same sealed body compiled; listing `r` compiled, but admitted the
  outer `r` too. A destructured name now shadows a file-level name as a
  plain binding, a `for` variable and a `match` binding do.

- **A hidden or sealed-out local can no longer be overwritten from inside
  the block that hides it.** Beneath `hide x` (or a `seal except` that does
  not list it), `x = 5` in a nested block was taken by the checker for a new
  local, and the generated C wrote the enclosing `x`, the variable the
  directive hid: the program printed 5 after the block. Aether has no
  block-level shadowing of a local, so the binding is now refused (E0304,
  "a local is not shadowed, pick another name"), plainly bound or
  destructured. A `for` loop's variable and a `match` arm's binding are
  fresh variables and stay accepted. `docs/hide-and-seal.md` promised a
  fresh inner binding here; it now says what the language does.
