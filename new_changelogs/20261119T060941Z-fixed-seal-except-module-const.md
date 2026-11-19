- **`seal except mm` admits the module's constants.** `mm.K`, an exported
  constant of a listed module, was refused as "module 'mm' has no export
  'K'", and no spelling on the list helped; `mm.bump(p)` beside it compiled.
  The constant's merged name `mm_K` was looked up through the seal; it is
  now looked up past it once the prefix `mm` has passed, as a call is.
