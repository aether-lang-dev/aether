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

1. Syncs `origin/main` → HEAD in a clean tree, pinning `PATH`/`AE`/`AETHERC`/
   `AETHER_HOME` to the freshly-built compiler (never a version-managed `ae` on
   `PATH`).
2. A **fail-on-missing-deps** gate — a dedicated build box should test
   everything, so an absent contrib dependency is a provisioning bug that turns
   the run RED (the opposite of `contrib_build.sh`'s probe-and-skip).
3. `make ci` → `make contrib` → `make contrib-host-check` → a `contrib` `ae
   check` sweep → `make contrib-check-valgrind` (build + run every contrib
   `test_*.ae`, valgrind-gating the leak-clean ones), each timed (ms) and
   recorded.
4. Publishes a topline pass/fail table to the **`nightly-results`** orphan
   branch (no shared history, no CI triggered) and writes a dated summary + log
   locally (last 14 kept).

## Running it

Cron isn't installed on CachyOS by default; use a **systemd --user timer** (see
the header comment in `nightly.sh`). Provision the contrib deps first — package
installs plus the `aether-lang-dev/factor-language` fork build — and export the
`AETHER_*` dep env vars in the timer environment. The gate stays RED until every
dep is present, by design.
