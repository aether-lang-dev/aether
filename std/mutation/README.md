# std.mutation

Mutation testing: change the code under test, and check the tests notice.

A green suite proves the tests ran, not that they would catch anything.
Mutation testing closes that gap by making small semantic changes to the
subject — `>=` to `<`, `==` to `!=`, a string literal to empty — and rerunning
the suite. A mutant the tests still pass is a **survivor**: a change to the
code that nothing objected to, which is a gap in the tests.

Driven through `ae mutate` rather than called directly; the module is the
engine behind that subcommand.

```
ae mutate --sut std/foo/module.ae --test std/foo/test_foo.ae
```

Output is a score plus the survivors, located by source line:

```
Aether mutation testing (std.mutation)
baseline: suite passes on unmutated SUT ✓

killed  module.ae:42 GTE->LT
SURVIVED module.ae:87 EQ->NE
...
14/15 mutants killed — mutation score 93%
1 survived (test gaps):
  - module.ae:87 EQ->NE
```

The operators are the arithmetic and comparison swaps (`>=`→`<`, `==`→`!=`,
`+`→`-`, `&&`→`||`), plus two string ones: a non-empty literal to empty, and
an empty literal to a marker. Mutations inside string literals are skipped —
changing text a program merely prints is not a semantic change.

**The baseline is checked first.** If the suite does not pass on the
unmutated subject, the run aborts rather than reporting a meaningless score:
every mutant would "die" for the wrong reason.

A mutant that does not compile is excluded rather than counted as killed —
counting it would inflate the score with changes no test could have caught.

This module is exercised by shell drivers rather than a co-located spec,
because it rewrites files and shells out to the compiler: see
`tests/integration/mutation_testing/`.

## Exports

`run`, and the `SURVIVED` / `KILLED` / `NOCOMPILE` verdict constants.
