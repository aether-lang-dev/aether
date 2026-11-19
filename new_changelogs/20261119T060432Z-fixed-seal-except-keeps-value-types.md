- **`seal except` and `hide` no longer change the code they guard.** A
  struct the list did not name lost its field types inside the sealed body,
  with no diagnostic: `b.app_dir == ""` on a `*Browser` parameter compiled
  to a pointer comparison, `d = b.app_dir` made an `int` local the function
  did not own, a closure stored in a field went unretained, and a local
  bound from a call returning such a struct was refused as "of a type the
  program does not define" (E0302). A value now keeps its type; the list
  restricts the names the body writes. Names on the list also stopped
  counting as uses: a listed function was marked as a function value (its
  string returns wrapped in `aether_fnptr_give`), a listed local's last use
  copied instead of moving, a listed constant or slice parameter read as a
  binding of it, a listed slice or `Isolated` parameter as used or moved
  before the body's own move of it (E0906, E0200). The checker now drops
  the directives once the program has checked, so a body that compiles with
  and without a seal compiles to the same C.
