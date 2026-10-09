- **`ae build` type-checks every function of every imported module, called
  or not.** The tree-shake that keeps uncalled library functions out of the
  emitted C ran before the typechecker, so a function of an imported module
  that nothing called was never checked: aephysics' `body_apply_force` added
  a `Vec3` to a `Vec3f` and built clean in every consumer until the first
  one called it, while the same function uncalled in the main file failed.
  The prune now marks before checking and sweeps after it, so a program
  emits the same functions and only the checking covers more (50 to 90 ms,
  10 to 20%, more front-end time on a program that pulls in the TLS stack,
  within the noise of a whole build). A selective import merges and checks
  the whole module too, which also fixes a selected function that read an
  unselected constant (`Undefined variable 'm_K'`) and a qualified call to
  an unselected function of a selectively imported local module; a local
  function spelt like one of the module's mangled names (`m_other`) is now
  the E1001 collision under a selective import that it already was under a
  bare one (#2613).
- **A module can only call the modules it imports itself.** Inside a merged
  module function a qualified `ns.name` resolved against every module loaded
  anywhere in the program, so `top` could call `low.f()` while only `mid`
  imported `low`: `ae build` accepted it and `ae check` of `top` rejected it,
  and `top` broke, at a line nobody touched, the day `mid` dropped the
  import. `ae build` now applies the stricter rule `ae check` did, in a
  module's functions, actors and constants: E0301 for a call (E0300 for a
  constant), with a help line naming the missing `import`. std.bignum was
  relying on it for std.string and now imports it (#2614).
