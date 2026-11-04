# Ownership grid

Stage 1 of the ownership design (`docs/design/ownership-model.md`, PR
#2883): a generated test per value kind, place and transfer, which measures
whether Aether gets ownership right for that shape today. It replaces
probing by hand and filing one issue per shape: a new ownership finding
becomes a cell, and the stages of the design close cells.

## What a cell is

A cell is one value **kind** (a fresh string, a `string?`, a closure that
captures a fresh string, an `fn[2]`, a struct owning a string, a
`(string, int)` tuple, a `string[2]`, a `Doc[2]`, a named function used as a
closure, the error slot of an `int!`, a sum whose variant owns a string or a
closure, a slice owning the buffer `make` handed out) held in one **place**
(a local, a local rebound in a loop, a struct
field, a field of a heap box reached through a pointer, an array element, a
tuple slot, an optional, a module `var`, a
closure's capture or cell, a list or map, actor state, a message field, a
`ptr` slot, a part of a parameter, a temporary)
and moved by one **transfer** (store and leave, overwrite, `break`/`continue`,
early `return`, a caught panic, a reader, a keeper, a freer, return, hand
back, `defer`, alias, store into a global, capture, an `if` expression or a
value `match` of fresh values passed as an argument, a temporary given
straight to a builtin, to an extern's `string` parameter or to a
comparison). Its id is
`<kind>-<place>-<transfer>`, `K1-P5-T7` for instance, and the tables in
`gen.ae` (`kind_title`, `place_title`, `transfer_title`) say what each code is.
A fourth axis is the **producer**: which std C function made the string.
`docs/design/ownership-r1-inventory.md` classes every std function that
returns a string by what it returns (A, a counted string the caller owns;
A~, a counted string another holder keeps; B, a plain malloc'd buffer; C, a
pointer into memory C keeps; D, a literal; a tuple carrying one of those).
The kinds `SA1` to `ST2` take the string of one real function per class (two
where the class is large or mixed: `string.replace`, `string.concat_wrapped`,
`string_list_get`, `path.join`, `path.rel`, `string.seq_head`,
`string.to_cstr`, `path.separator`, `http.mime_type`, `fs.read_binary`,
`fs.realpath`) through the main places (local, field, element, module `var`,
capture, list, message) and transfers (scope exit, overwrite, caught panic,
keep, return, alias), so a change to how the producers return their strings
shows in the grid. `SA3` to `SA5` (`string.from_int`, `string.pad_start`,
`string.new_with_length`) are three of the functions std declared as
returning a `ptr` until #3018 typed them `string`.

The sum kinds `KS` and `KSF` (`Owned = Named | Plain`, whose `Named` owns a
string, and `Acting = Act | Plain`, whose `Act` owns a closure) cross the
places a sum can be held in, a module `var` aside (its initializer is no
constant) and the containers and `ptr` slots (strings and closures only):
a sum is a composite that owns what its variants own (R1).

The slice kind `KM` (the buffer `make([]int, 3)` hands out) is crossed with
the temporary only (P17), passed to a function that reads it and returned:
a slice local, field or element is slice ownership's, which
`tests/integration/slice_own` measures. The transfers `T17` to `T19` are a
temporary's too (#3549): a fresh string given straight to builtins lowered by
name, to an extern's `string` parameter and to a comparison, each borrowed
for the use and released after it, as an Aether function's argument is.

Each cell is a standalone program, written by `gen.ae` into
`build/ownership_grid/cells/<id>.ae`. It runs the shape in a function called
100 times per round, checks every value it reads against the one it stored
(the expected values come from the construction), and measures
`mem.steady_growth` over rounds of 100 calls. When a cell fails, its program
is the reproducer: `build/ae run build/ownership_grid/cells/k1_p5_t7.ae`.

There is no garbage collector to wait for: a cell passes only when each
round of 100 calls gives back everything it took before the next round
reads the heap. A value freed only at program exit, or freed a round late by
a sweep (an actor ended later, say), is growth, so it is a LEAK. A cell that
ends actors ends them itself (`scheduler_reclaim_released`) before it reads
the heap.

`run.ae` builds each cell once, with `poison_alloc.c` linked
(`-Wl,--wrap=free,--wrap=realloc,--wrap=aether_numa_free,--wrap=aether_numa_free_aligned`),
and runs it twice. The wrapped functions pass straight through unless
`OWNERSHIP_GRID_POISON` is set, so the first run measures the program as it
is. The second sets it: every freed block is then filled with 0xDD, never
handed back, and a second free stops the program. The runtime's NUMA frees
are wrapped beside `free`, because an actor's block goes back through
`_aligned_free` (or `VirtualFree`) on Windows, which `--wrap=free` never sees.
A freed string keeps its 32-byte header (so a second release still reaches
`free` and is caught), which is why every cell compares a string's
characters, never only its length. To rerun one by hand:

    AE_CC="gcc -Wl,--wrap=free,--wrap=realloc,--wrap=aether_numa_free,--wrap=aether_numa_free_aligned" \
        build/ae build build/ownership_grid/cells/k1_p5_t7.ae \
        --extra tests/ownership_grid/poison_alloc.c -o p.exe
    ./p.exe
    OWNERSHIP_GRID_POISON=1 ./p.exe

The poisoning allocator needs a gcc/ld linker: macOS's linker has no
`--wrap`, so there the cells are built without it and no cell is UAF. The
leak measurement needs an exact heap count (`mem.heap_in_use_exact()`:
Windows, or a sanitizer allocator); elsewhere a cell prints that growth was
not measured and is UNMEASURED.

Not every combination is meaningful (an array cannot hold an array, a
module `var` cannot hold an array, only a string has a free a callee can
call, a cell that is the same program as another). `gen.ae` decides, and
`build/ownership_grid/run --skips` lists every skipped combination with
its reason (`--list` the cell ids, `--show K1-P5-T7` one cell's program);
the report lists them grouped by reason.

## Statuses

| status | meaning |
|---|---|
| OK | every value right, no growth, nothing under the poisoning allocator |
| UNMEASURED | values right and nothing under the poisoning allocator, but the heap count is not exact on this platform, so growth was not measured; never OK |
| LEAK | `mem.steady_growth` is not 0; the size is the median of three runs, in bytes per 100 calls |
| INCOMPLETE | the poisoned run did not finish (a timeout, an end that is no memory fault) or could not be built, so the use-after-free check did not happen; never OK |
| NOCOMPILE | aetherc refused the program (NC-a) or the generated C did not compile (NC-c); the first error is kept |
| TIMEOUT | the build or the plain run did not finish in its time |
| CRASH | the plain run died, and the poisoned run pins no memory error |
| WRONG | a value is wrong in the plain run |
| UAF | the poisoned run stopped on a double free (of a heap block or a runtime actor block), crashed with an access violation or heap corruption, or read a wrong value; this outranks a plain WRONG or CRASH |
| HARNESS | the runner itself failed on the cell twice (a child it could not start, a log it could not read, a fault of the runner): never a finding about the compiler, and the check fails with exit code 2 saying so |

## Targets

Each cell also has a target: the status the approved design wants. It is
OK for every cell but one family. A function may not free a value it borrows
(a parameter): under R3 the caller still owns it, so `string.free(param)`
releases a reference the function does not own, and from stage 4 the checker
refuses `string.free` or `release` of a parameter, with a message pointing to
keeping the value (store it) instead. The cells that pass a value to a
function that frees it, on every path (T8) or on some (T8S), therefore have
REFUSED as their target, which an aetherc refusal (NC-a) meets. Better and
worse in the check stay relative to the baseline; the report adds how far
each cell is from its target, and how many cells are not at it.

## Running it

    make ownership-grid            # every cell, the default C path
    make ownership-grid-check      # the same, then compare with baseline.txt
    make ownership-grid OWNERSHIP_GRID_ARGS="--mode both"     # also -DAETHER_GCC_COMPAT=0
    make ownership-grid OWNERSHIP_GRID_ARGS="--only 'K3-*,K4-P5-*'"

The make targets build the runner, `run.ae` with the generator `gen.ae`, into
`build/ownership_grid/run` and run it from the repository root, where it can
also be run by hand (`--help` lists its options). It is one thread: it starts
up to `NPROC` builds and runs with `os.spawn_proc_to` (each one's output in its
log file, its whole tree killable), waits for the first to end with
`os.wait_any_timeout`, kills whichever passes its deadline (600 s for a
build, 120 s for a run), and reads the log.

`NPROC` bounds the parallel cells. The output is `build/ownership_grid/`:
`report.md` (counts overall, per kind and per place; the total LEAK, the
bytes per 100 calls summed over every LEAK cell, which is the number to
drive to zero; the ten worst cells; the producers by class; the std modules
the cells reach and the ones none reaches; the cells the PR #2883 comments
found by hand; one matrix per kind, places by transfers; every cell that is
not OK with its first error or wrong value; the skipped combinations),
`report.csv` (a row per cell and C path), `results.json` (what
`run --report-only` rewrites the report from), `cells/` (the programs)
and `<path>/bin/` (each build's and run's log; the binaries too with
`--keep-bins`, deleted otherwise since they come to 4 GB). Builds go through
a cache of the grid's own, `build/ownership_grid/cache`, capped at 1 GB.

## The baseline and the check

`baseline.txt` holds every cell's status on main: `<cell> <default C path>
<-DAETHER_GCC_COMPAT=0 path, or ->`, a LEAK with its size (`LEAK:3600` is
3600 bytes per 100 calls). `make ownership-grid-check` fails when a cell is
worse than its baseline: OK became anything else; LEAK became INCOMPLETE,
NOCOMPILE, TIMEOUT, CRASH, WRONG or UAF; INCOMPLETE or NOCOMPILE became one
of the last four; or a LEAK more than doubled and grew by more than 2 KB per
100 calls. The size rule is the measured noise: the same cell's growth moved
by up to about 1 KB per 100 calls between runs, never by a factor of two,
while a new leak in a shape is at least one string or environment per call,
1.8 KB or more per 100 calls. The check also fails when a cell in the
baseline is no longer generated (a removed cell is removed from the baseline
on purpose), and with exit code 2 on a HARNESS cell. A cell that got worse
is run again before it counts, and the rerun's result is the one kept in the
report and in `--update-baseline`. Changes within a tier (UAF to WRONG) are
listed but do not fail it, and nor do cells that got better: those are
listed so the baseline can be updated.

Off a platform with an exact heap count every cell that would be OK or LEAK
is UNMEASURED: the check then compares values and use after free only, says
so, and refuses `--update-baseline`, since the baseline records leaks.

A fix that makes cells better updates the baseline in the same PR:

    make ownership-grid-check OWNERSHIP_GRID_ARGS="--mode both --update-baseline"

and the diff of `baseline.txt` is the list of shapes the fix closed. A fix
that makes any cell worse fails the check, whatever it was fixing, and the
check then writes no baseline (a regression is recorded only on purpose, by
`make ownership-grid OWNERSHIP_GRID_ARGS=--update-baseline`, without the
check). With `--only`, `--update-baseline` rewrites only the lines of the
cells it ran.

The baseline was measured on Windows (MINGW64 gcc), where the heap count is
exact. A few cells move between UAF and WRONG from run to run (whether the
freed block was reused before the read), which the check ignores. A cell
that got worse is run again before it fails the check.

The cells that spawn an actor per call (`K*-P11-T1`) end every released
actor before the next call, and read the heap over 20 windows of 10 calls
instead of 5 of 100: the scheduler keeps an ended actor's block for the next
spawn, so the heap count moves by a block in some windows without a leak,
and under load five such windows in a row read as growth (1 run in 72, with
24 copies at once). A leak grows every window, so it is still reported (a
list leaked per call reads 400 bytes per 10 calls); the 20 short windows
made 240 runs under the same load all OK.

## In CI

windows.yml's `Windows / ownership grid [k/3]` jobs run the check on the
default C path, a third of the cells each (`OWNERSHIP_GRID_SHARD=k/3`, the
cells whose position modulo 3 is k-1, as `AE_SWEEP_SHARD` splits the test
sweep), on MINGW64. Windows is where the heap count is exact, so it is
where leaks are measured; the jobs upload the report, the cells and their
logs. `--shard` runs one shard by hand:

    make ownership-grid-check OWNERSHIP_GRID_ARGS="--shard 2/3"

## The runner's own tests

`build/ae run tests/ownership_grid/test_runner.ae` (from the repository
root) checks two failure paths of the runner itself: a cell's binary held
open by another process (the run completes and reports the file as left),
and a fault inside the runner on one cell (`OWNERSHIP_GRID_FAULT=<cell>`: the
cell becomes HARNESS, the other cells keep their statuses, and the check
exits 2 with a message).

## Adding a cell

A new ownership finding is a new kind, place or transfer in `gen.ae` (or a
case the generator skipped that is meaningful after all), not a test of its
own. Add it, run `make ownership-grid-check`, and add its line to the
baseline with the status it has on main.
