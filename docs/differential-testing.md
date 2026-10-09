# Differential testing across lowering paths

Aether compiles the same program in more than one way. `--emit=exe` produces a
binary with `main()`; `--emit=lib` produces an artifact with no `main()` where
every top-level function is exported as `aether_<name>`. The two are supposed to
be observationally equivalent: a program's behaviour should not depend on which
one you asked for.

Nothing checked that. Each path was verified against its own expected output,
which is a weaker property than it looks:

- A bug that miscompiles **both** paths the same way passes, because both
  outputs match each other and the expectation was captured from a build that
  already had the bug.
- A bug that miscompiles **one** path still passes, as long as that path's
  expected-output file was generated from the same broken build.

Cross-path agreement catches the second class directly, and it does so without
anyone having to write down what the right answer is.

## What runs

`make test-differential`, and step 9 of `make ci`.

For every `tests/differential/cases/*.ae`:

| Path | How it runs |
|---|---|
| `--emit=exe` | the case's own `main()` calls `run()` |
| `--emit=lib` | `tests/differential/driver.c` dlopens the artifact and calls `aether_run` |

stdout and the exit code are captured from both and compared. A difference is a
hard failure that names both paths and prints the diff; it is never downgraded
to a warning.

One driver serves every case. It resolves the entry point by name at runtime, so
adding a case needs no generated glue.

## Writing a case

```aether
import std.string

run() {
    println("whatever the case exercises")
}

main() { run() }
```

Two rules, both load-bearing:

- **`main()` must do nothing but call `run()`.** The comparison is only
  meaningful if both paths execute the same code. If `main()` does its own work,
  a difference in output stops being evidence about lowering.
- **The case must be capability-empty and deterministic.** No `fs`, `net` or
  `os`; no clocks, addresses, or ordering that depends on where the allocator
  happened to put something. A case that varies run to run reports a divergence
  that is not one, and a suite that cries wolf gets ignored.

Cases are a deliberately tagged subset rather than all of `tests/regression`.
Every case doubles its run count, and the point is coverage of *lowering*
surface, not of every program in the tree.

## Carveouts

A case whose paths legitimately differ goes in `tests/differential/carveouts.txt`
as `<case> <reason>`:

```
some_case the reason this case cannot agree across paths
```

The discipline matters more than the mechanism:

- A carved-out case is **reported on every run** with its reason, and counted in
  the summary line. It is never silently skipped. An accepted divergence that
  disappears from the output stops being a decision anyone revisits.
- A carveout naming a case that does not exist is a **hard error**, so the file
  cannot rot into a list of ghosts that excuse nothing.

There are no carveouts today. Nothing has yet been found that differs between
`--emit=exe` and `--emit=lib`.

## Platform note

The `--emit=lib` half needs `dlopen`, which MSYS2/MinGW does not provide (the
runtime's `-ldl` dependency does not exist there). On Windows the suite reports
a skip with that reason rather than passing silently, the same treatment the
C-interop link cases give their Windows gap.

## Optimisation levels: -O0 against -O2

`ae build` compiles the generated C at `-O2`; `ae run` and `ae build --quick`
compile it at `-O0 -g`. The `.ae` corpus used to run only at `-O2`, so a bug
that shows only at `-O0` passed every sweep and reached the people using
`ae run`: a stack canary tripping on an overflow that `-O2` lays out
harmlessly, a read of uninitialised memory, a string read after its owner was
freed. #1957, #2128 and #2484 had that shape.

`make test-ae-opt-diff` (#2488) builds every program `make test-ae` builds at
both levels, runs both, and compares stdout and the exit code. The list is the
sweep's own (`tests/scripts/ae_sweep_list.sh`, which `test-ae` reads too), so
the two cannot drift apart. Each program is built the way the sweep builds it,
and both of its binaries run from the same path one after the other, so a
program that prints `argv[0]` agrees with itself. A difference fails the
target and prints both sides; a program that fails to build at either level
fails it too.

CI runs it on the Linux / GCC leg only, after `make ci`. The sweep there has
just built every program at `-O2` with the same compiler and flags, so those
builds are normally served from the build cache and the step costs about one
`-O0` sweep.

Its carveouts live in `tests/differential/opt_diff_carveouts.txt`, as
`<program> <reason>`, and follow the rules above: reported on every run, an
error if the program no longer exists. A carved-out program still has its
exit codes compared, so a crash at one level is caught in it too; only its
stdout is excused. A program belongs there only when its stdout varies from
run to run at a single level (a clock, a pid, an interleaving of threads). One
that prints the same thing each time at each level, but something different
between them, is a bug in the compiler or in the test, and is fixed instead.

`OPT_DIFF_ONLY=<regex>` narrows a run to the matching programs, for re-running
the ones a sweep reported.

## Extending it

The exe-vs-lib pair and the two optimisation levels are path comparisons; there
are others. A UBSan build is the natural third configuration of the
optimisation-level target. Per-target codegen is another (the Linux and MSYS2
builds CI already runs are each checked only against their own expectations,
never against each other).

The harness self-checks that its comparison can fail. Without that, a broken
diff would make every case vacuously agree and the suite would report success
while testing nothing.
