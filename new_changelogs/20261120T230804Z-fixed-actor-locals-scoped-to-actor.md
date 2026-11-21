- **An actor handler's locals no longer type another function's locals of the
  same name.** The early inference walk never unwound what an actor bound, so
  a handler's `f = spawn(Fetcher())`, a state, a handler's loop variable or
  destructured local stayed in scope for every function and `main` walked
  after the actor, in every module merged after it. A later `f` bound from a
  value that walk could not type yet read the actor's instead: importing
  `std.cryptography` beside such a handler failed with E0302 "Unknown type
  'Fetcher'" at `g = f` inside `std/cryptography/sha2`, and in the program's
  own functions with "Type mismatch in variable initialization". An actor is
  now walked as a scope of its own, and each receive arm's locals are unwound
  when the arm ends.
