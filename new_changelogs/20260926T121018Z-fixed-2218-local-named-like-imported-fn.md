- **A local variable spelt like another module's exported function no longer
  pulls that function into the build.** The dead-code prune treated every bare
  identifier as a possible function reference, and the suffix match that serves
  glob imports turned a local `record` in one module into a use of `ui.record`
  in another. aether-ui's `record` builder then failed to compile under gcc 14.
  The prune now skips names the enclosing function binds itself: locals,
  parameters, loop and tuple variables, closure parameters and catch names.
  A `return` whose value sits on the other side of the int/pointer boundary
  from the function's return type, such as `return _builder` from a builder
  declared `-> int`, now gets the same cast a call argument already gets.
  (#2218)
