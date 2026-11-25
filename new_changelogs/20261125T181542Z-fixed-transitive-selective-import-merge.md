- **A module reached only through selective imports keeps its `@extern`s,
  fault sets, messages and actors.** With `import std.math (math_sin)` and
  `import a (sq)` in one module, `a`'s own `math.sqrt(x)` failed with E0301
  "Undefined function"; a fault the module returned was E0300 "Undefined
  variable", and an actor it spawned "Undefined function 'spawn_X'". A
  module the program reaches only through other modules' imports is merged
  by the transitive pass, which kept its own copy of the merge with only
  functions, constants and types in it. A bare import anywhere on the path
  merged the module directly and hid the gap. 0.944.0 made std.math's
  `sqrt`, `floor`, `ceil`, `round`, `abs`, `min`, `max` and `clamp`
  `@extern`s, which is what exposed it. Both passes now merge a module
  through one function.
