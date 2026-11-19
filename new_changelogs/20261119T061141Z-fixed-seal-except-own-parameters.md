- **A `seal except` on a function's body admits the function's own
  parameters and hoisted locals.** Each parameter had to be listed
  (`seal except p, argv, ...`, else E0304), and a local an `if` arm binds
  and the body reads after it was refused, while a closure's seal always
  admitted its own parameters. A seal on a block nested deeper in the
  function still refuses them, and `hide p` still hides a parameter.
