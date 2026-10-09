- **`ae build` type-checks every function of every imported module, called
  or not.** The tree-shake that keeps uncalled library functions out of the
  emitted C ran before the typechecker, so a function of an imported module
  that nothing called was never checked: aephysics' `body_apply_force` added
  a `Vec3` to a `Vec3f` and built clean in every consumer until the first
  one called it, while the same function uncalled in the main file failed.
  The prune now marks before checking and sweeps after it, so a program
  emits the same functions and only the checking covers more (50 to 90 ms,
  10 to 20%, more front-end time on a program that pulls in the TLS stack,
  within the noise of a whole build). A local function spelt like one of a
  selectively imported module's mangled names (`m_other`) is now the E1001
  collision it already was under a bare import (#2613).
- **A selective import merges the whole module.** `import m (a)` merged
  only `a` and the functions it called, so a selected function that read a
  constant the import did not select failed with `Undefined variable 'm_K'`,
  and a qualified `m.other()` to an unselected function failed with E0301
  although the qualified surface stays whole under a selective import
  (#878). The selection now decides only which names the file may write
  bare (#2630).
- **A module can only call the modules it imports itself.** Inside a merged
  module function a qualified `ns.name` resolved against every module loaded
  anywhere in the program, so `top` could call `low.f()` while only `mid`
  imported `low`: `ae build` accepted it and `ae check` of `top` rejected it,
  and `top` broke, at a line nobody touched, the day `mid` dropped the
  import. `ae build` now applies the stricter rule `ae check` did, in a
  module's functions, actors and constants: E0301 for a call (E0300 for a
  constant), with a help line naming the missing `import`. std.bignum was
  relying on it for std.string and now imports it (#2614).
- **`ae check` of a module file resolves the module's own name.** A module
  sees itself in a build, so `selfq.a()` inside module `selfq` built and
  ran, but checked on its own the file knew no namespace and `ae check`
  rejected the call (E0301). The file now gets the namespace a build gives
  it, the last segment it is imported under, so its own functions and
  constants resolve and its private ones stay private (E0303) in both
  (#2631).
