# CachyOS nightly

`nightly.sh` is a **self-run** nightly build for a rolling-release Arch box
(CachyOS), the sibling of [`tests/freebsd/nightly.sh`](../freebsd/nightly.sh):
a platform the GitHub Actions matrix can't cover.

Its value is the **toolchain**: CachyOS ships GCC/Clang several major versions
ahead of the Ubuntu-22.04 CI box, so building HEAD there surfaces newer-compiler
`-Werror` promotions and warnings before they reach anyone. It also does the
things the portable CI never does — **type-checks every `contrib/*/module.ae`**
*and* **builds + runs every contrib `test_*.ae`** (under valgrind where the test
is leak-clean by design). Type-checking alone let tinyweb's WebSocket server
path rot — it compiled fine but was runtime-broken — so running the tests is the
half of the coverage gap that actually catches that class of bug.

## What it does

1. Syncs `origin/main` → HEAD in a clean tree (**`--tags`**, see below), pinning
   `PATH`/`AE`/`AETHERC`/`AETHER_HOME` to the freshly-built compiler (never a
   version-managed `ae` on `PATH`).
2. A **fail-on-missing-deps** gate — a dedicated build box should test
   everything, so an absent contrib dependency is a provisioning bug that turns
   the run RED (the opposite of `contrib_build.sh`'s probe-and-skip).
3. **Records the version of every third-party dependency** it built and tested
   against (see below) — captured right after the gate, so a run that later
   fails still says what it was running.
4. `make ci` → **`make install` + a freshness check** → `make contrib` → `make
   contrib-host-check` → **a no-skipped-cases gate over the host specs** → a
   `contrib` `ae check` sweep → `make contrib-check-valgrind` (build + run
   every contrib `test_*.ae`, valgrind-gating the leak-clean ones), each timed
   (ms) and recorded.
5. Publishes a topline pass/fail table to the **`nightly-results`** orphan
   branch (no shared history, no CI triggered) and writes a dated summary + log
   locally (last 14 kept).

## Running it

Cron isn't installed on CachyOS by default; use a **systemd --user timer** (see
the header comment in `nightly.sh`). Provision the contrib deps first — package
installs plus the `aether-lang-dev/factor-language` fork build — and export the
`AETHER_*` dep env vars in the timer environment. The gate stays RED until every
dep is present, by design.

### Why every contrib step here is fail-hard

The portable CI runners probe-and-skip because they cannot have every
language's dev kit installed. This box can, and does — so the same skip that
is honest on a runner is a **silently shrinking test surface** here. Each layer
hides the one below it, so all of them have to be closed:

| Layer | Default behaviour | On this box |
| --- | --- | --- |
| A dep is absent | — | step 1's dep gate turns the run RED |
| A module fails to **build** | `make contrib` tallies a SKIP, exits 0 | `MODULES=<all>` → fail-hard (exit 1) |
| A bridge **archive** is missing | phase [3/3] prints SKIP, exits 0 | `CONTRIB_HOST_STRICT=1` → FAIL |
| A spec **runs but skips its cases** | prints `⊘`, exits 0 | `skipped=0` gate → RED |

The dep gate alone is not enough for the middle two: it catches "the library
is gone", never "the library is here and the module no longer compiles against
it" — which is precisely the breakage a GCC 16 / Clang 22 box exists to find.
In probe-and-skip mode that break reports as a skip and the nightly stays
green.

The last row is the subtlest and was found by testing rather than reasoning.
`contrib/host/factor`'s archive builds fine with no Factor installed (the
bridge is pure `dlopen`), so both `MODULES=` and `CONTRIB_HOST_STRICT` pass it
— and then all six of its cases skip at runtime because `AETHER_FACTOR_SONAME`
is unset, reporting `0 passing / 6 skipped` and exiting 0. `std.spec` states
the principle directly: *"A skip that reports as a pass is worse than no skip
at all."*

That gate reads the **machine-readable** report rather than the human output:
`AE_SPEC_FORMAT=aeocha` + `AE_SPEC_REPORT=<path>` make each spec write
`skipped=<n>`, which is summed across the host specs. Parsing the `⊘` lines
would mean parsing ANSI colour and would break the moment the renderer
changes.

The module list for the fail-hard build is **derived from
`contrib_build.sh`'s own `CATALOGUE`**, not hardcoded, so a module added there
is covered here automatically and cannot be forgotten.

`CONTRIB_HOST_STRICT` defaults to `0`, so GitHub CI and dev boxes are
unaffected — only this nightly sets it.

### Dependency versions: recorded, not pinned

Nothing in the repo pins any contrib dependency. `probe_racket` checks only
that `chezscheme.h` and `racketcs.h` *exist*; the dep gate checks
`command -v racket`; the `pacman -S` line in the header is unversioned.

**That is deliberate.** This box is rolling-release precisely so it runs
*ahead* of CI and finds breakage early — the same rationale as its GCC 16
against CI's GCC 11. Pinning Racket would mean never learning that a new
Racket broke the embedding surface.

What was missing is the **record**. Without it a green run does not say what
it tested, and a red run the morning after `pacman -Syu` reads as a code
regression rather than an upstream bump — a bisect nobody should have to do.
So each run writes `deps_<stamp>.tsv` and publishes it as a table beside the
step timings.

Measured on the box (2026-08-18), which shows why this matters:

| dependency | version |
|---|---|
| `racket` | 9.2 |
| `lua` | 5.5.0 |
| `python` | 3.14.6 |
| `node` | 26.4.0 |
| `java` | openjdk 26.0.2 |
| `go` | 1.26.5 |
| `tinygo` | 0.41.1 (LLVM 20.1.1) |
| `factor-fork` | `91a3639cf6` (a commit — it is built from source) |

Racket 9.3 shipped 2026-08-13 while the box was on 9.2. The Racket CS
embedding surface the bridge compiles against (`chezscheme.h`'s `Snil`,
`Scons`, `Smake_bytevector`) is macro-based and has moved across majors, so
that upgrade is exactly the kind this table makes legible.

Reporting is all this step does. Whether a missing dep is fatal stays the dep
gate's decision — duplicating that verdict here would give two sources of
truth for one condition. A dep that is absent records `(absent)` and the step
still passes.

### Why the nightly installs, and why it fetches tags

Two failure modes that both read as "the compiler on this box is broken" and are
neither.

**The install drifts.** A few tests link the *installed* artifacts rather than
the build tree — `ci_coverage_smoke` is the current one, and it *skips* when no
install is present. So deleting the install silences a test instead of fixing
it. Left unrefreshed it rots: a Jul-19 install against an Aug-8 tree failed with

    undefined reference to `aether_unwind_forget'

because the symbol was added in between. The nightly log shows only `gcc
--coverage link failed` without the linker's line, which reads like a bug in
this box's newer GCC. It is not. Hence `make install PREFIX=$HOME/.local` as a
pipeline step (never sudo), plus a freshness check so the failure surfaces as
one line rather than a mystery.

**Tags are the version, not the `VERSION` file.** The Makefile takes the highest
`git tag` as authoritative and falls back to `VERSION` only for tarballs. A
fetch without `--tags` therefore leaves the tree at the newest *commits* but the
previous *tag*, and the build correctly stamps the older version — which then
looks like a stale build to anything comparing against the `VERSION` file. Seen
2026-08-08 at 0.505.0's merge commit with `v0.504.0` as the newest local tag.

Related trap when reading versions by hand: **`ae --version` reports the version
*manager's* active install** (`~/.aether/active_version`), not the version
compiled into the binary you just ran. On this box that state reads `0.417.0`,
so a correctly built 0.505.0 binary still *says* 0.417.0. Both the sweep guard
and the install check use `strings` against the compiled-in value for this
reason.

### The `de_DE.UTF-8` locale

The dep gate also requires a **generated comma-decimal locale**. It backs
`tests/regression/test_string_double_locale.ae`, which pins that `std.string`
and `std.json` float text stays `.`-separated no matter what locale an
embedding host has selected — the bug found in review of PR #1429, where
`string.to_double("3.14")` failed outright under a comma-decimal locale.

Arch ships glibc with the locale *sources* present but ungenerated, so:

```sh
sudo sed -i 's/^#de_DE.UTF-8 UTF-8/de_DE.UTF-8 UTF-8/' /etc/locale.gen
sudo locale-gen
locale -a | grep de_DE          # expect de_DE.utf8
```

This box is the **only** place that test really runs: the portable CI images
ship C/POSIX only, so it SKIPs there (deliberately — see the plan doc). A green
nightly whose locale test *skipped* is not a passing locale test; check the log
for `PASS`, not merely the absence of failure.

For a one-off local run without root, `localedef` writes anywhere and `LOCPATH`
points libc at it:

```sh
localedef -i de_DE -f UTF-8 "$PWD/loc/de_DE.UTF-8"
LOCPATH="$PWD/loc" ./build/ae run tests/regression/test_string_double_locale.ae
```

(`LOCPATH` locales do **not** appear in `locale -a`, so this trick satisfies the
test but not the dep gate — the gate wants the real system locale.)
