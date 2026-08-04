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

## Extending it

The exe-vs-lib pair is the first path comparison, not the only possible one.
Others worth adding when they exist: optimisation levels against each other, and
per-target codegen (the Linux and MSYS2 builds CI already runs are currently
each checked only against their own expectations, never against each other).

The harness self-checks that its comparison can fail. Without that, a broken
diff would make every case vacuously agree and the suite would report success
while testing nothing.
