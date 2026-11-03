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

## Undefined behaviour: the corpus under UBSan

A program can agree with itself at both levels and still rest on undefined
behaviour that a later compiler, or another flag, turns into a different
program. `make test-ubsan` (#2488) runs the C unit tests and the whole `make
test-ae` sweep with UndefinedBehaviorSanitizer on everything: aetherc, ae and
libaether are rebuilt instrumented, and so is every program and C host the
tests build. The flags reach all of them through one sanitizing compiler,
`build/ubsan/bin/gcc` (and `cc`), first on PATH and named by its path in `CC`,
so the Makefile, `ae build` and the shell tests that run `"$CC"` or `gcc` by
name all go through it. Naming it by path keeps the build-flags digest and
`ae`'s cache key apart from a plain build's: nothing instrumented is mixed with
anything plain.

A report stops the program (`-fno-sanitize-recover`), so the test fails, and is
written to `build/ubsan/reports/report.<program>.<pid>`, where a test that
expects its program to fail, or compares stderr, cannot hide it. The target
fails on any report and lists them grouped by source location. Before the
tests it proves the configuration took: `build/libaether.a` must call the UBSan
handlers, and `tests/ubsan/canary.ae`, whose C half shifts an `int` by 32, must
be stopped with a report. A run whose flags never reached the programs would
otherwise read as a clean corpus.

A finding is fixed where it is, never suppressed, unless it is in vendored
third-party code and shown to be; then the suppression names that code's
upstream report. The first run found a misaligned 16-bit load in liblzf (our
`lzfP.h` had turned its `STRICT_ALIGN` off), signed shifts that overflowed in
`std.mem`'s u64 readers and in two base64 decoders, a test child that faulted
by writing through null, and a `lane_wide` check that counted the instrumented
build's stack spills as heap moves.

It needs a UBSan runtime: gcc's libubsan or clang's compiler-rt, so Linux and
macOS (MinGW has none, and the target stops there and says so). CI runs it in
`.github/workflows/ubsan.yml` on ubuntu-22.04, on the pull requests that touch
the compiler, the runtime, std, `ae`, the Makefile or the sweep's scripts,
every night on main, and by hand. It takes about 11 minutes (the sweep under
UBSan is 1.75 times the plain one), which is why it is not on every PR.

## Memory errors and leaks: the corpus under ASan and LSan

`make test-asan-ae` (#3539) is the same run under AddressSanitizer with
LeakSanitizer, through the same script (`tests/scripts/sanitizer_sweep.sh`,
`SAN=asan`) and the same sanitizing compiler, `build/asan/bin/gcc`. A heap
overflow, a use after free or a double free stops the program that made it,
and a block it never freed is reported when it exits; the reports go to
`build/asan/reports/`, grouped by the first frame that is not the
sanitizer's or an allocator's. An error fails the run. A leak fails it only
with `SAN_LEAKS=fail`: until the test programs that never free what they
make are fixed (#3547), leaks are listed in the summary and the run passes
on them. A fault stays the signal it is (`handle_segv=0`), so a program's
own fault-to-panic handler and `ae run`'s crash report behave as without
ASan. Libraries that carry their own runtime copy and the sandbox's preload
are part of the design, so the one-definition and link-order checks are off
(`detect_odr_violation=0`, `verify_asan_link_order=0`). Tests that check
only what ASan replaces (a bound on RSS, the sandbox's preload, the shape of
compiled code, compiler recursion deeper than ASan's frames allow) are left
out of this sweep, each with its reason, in `tests/ae_sweep_prune_asan.txt`;
they run on every other leg. Two more take another path under ASan: one
that needs an address-space limit and one that needs Valgrind read
`AETHER_SANITIZER`.
Its canaries
are `tests/asan/overflow.ae`, which writes one byte past an 8 byte block, and
`tests/asan/leak.ae`, which drops the only pointer to one: each must be
stopped with its report before the corpus counts. aetherc and `ae` run
instrumented too, so a memory error in them stops the build that hit it, but
with leak detection off by default (`compiler/aether_toolchain_asan.h`): the
compiler's own leaks are gated a program shape at a time by
`tests/scripts/check_compiler_leaks.sh` (#1667). A test that asks for more
memory than exists still gets NULL (`allocator_may_return_null`), and
`mem.heap_in_use` counts the sanitizer allocator's live bytes, so the
ownership tests' steady growth checks keep their meaning under it.

`make test-asan` is older and narrower: the C unit runner only. Before #3539
that was all ASan, LSan and Valgrind ran on Linux, and the macOS leaks gate
over `tests/regression` was the only leak check of generated code. CI runs
the new target in `.github/workflows/asan.yml`, in two shards
(`AE_SWEEP_SHARD`), on the same pull requests and nights as UBSan.

## Extending it

The exe-vs-lib pair and the two optimisation levels are path comparisons; there
are others. Per-target codegen is one (the Linux and MSYS2 builds CI already
runs are each checked only against their own expectations, never against each
other).

The harness self-checks that its comparison can fail. Without that, a broken
diff would make every case vacuously agree and the suite would report success
while testing nothing.
