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
3. `make ci` → **`make install` + a freshness check** → `make contrib` → `make
   contrib-host-check` → a `contrib` `ae check` sweep → `make
   contrib-check-valgrind` (build + run every contrib `test_*.ae`,
   valgrind-gating the leak-clean ones), each timed (ms) and recorded.
4. Publishes a topline pass/fail table to the **`nightly-results`** orphan
   branch (no shared history, no CI triggered) and writes a dated summary + log
   locally (last 14 kept).

## Running it

Cron isn't installed on CachyOS by default; use a **systemd --user timer** (see
the header comment in `nightly.sh`). Provision the contrib deps first — package
installs plus the `aether-lang-dev/factor-language` fork build — and export the
`AETHER_*` dep env vars in the timer environment. The gate stays RED until every
dep is present, by design.

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
