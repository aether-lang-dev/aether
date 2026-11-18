- **A module's call to an extern it declares stays that extern when the
  program defines a function of the same name.** `pkg.cabs` declaring
  `extern abs` and calling it, in a program with its own `abs`, ran the
  program's, silently; std.cryptography's `extern free` beside a
  program's two-argument `free` failed every `free(ctx)` in std, with the
  errors pointing into std. Two passes did it: the checker let the
  program's symbol through because the module's extern counted as a
  declaration of its own (#3088's rule never fired), and codegen, renaming
  the program's function out of the extern's way (`abs` → `ae_abs`,
  #1366), renamed the module's calls with it. Both now leave code merged
  from a module alone. `tests/integration/module_extern_beside_program_function`.
