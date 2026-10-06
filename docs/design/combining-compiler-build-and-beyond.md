# Combining the compiler, the build system, and beyond

Nic,

An essay worth ten minutes: ["How big is the problem?"](https://shrub.industries/words/problem.html)
on shrub.industries. Below is what it argues, what we think of it, and why it
reads almost like a description of the Aether family, which already owns more
of its stack than any of the projects it cites. At the end are four concrete
things we could do. None of them is started; this is for discussing.

— Paul (drafted with Claude, 2026-10-06)

## What the essay argues

Operating systems miss optimisations because the compiler, the build system,
the package manager, the filesystem and the scheduler each know useful things
and share none of them. It builds the case up one layer at a time:

1. **Compiler + build system.** If the build system tells the compiler which
   files end up in one output, the compiler can optimise the whole program
   before linking, parse shared headers once, and rebuild incrementally per
   function, not per file.
2. **+ package manager.** Packages that are sources with build descriptions,
   not prebuilt binaries, give "dependency knowledge across package
   boundaries": whole-program optimisation through libraries, exact
   recompile-what-changed upgrades, and a declarative `system { packages, config }`
   that reconfigures incrementally and atomically.
3. **+ filesystem.** Link everything statically, then have the filesystem
   deduplicate the copies of each library, so you keep static linking's
   benefits without its disk cost.
4. **+ scheduler.** The build tells the kernel which job is on the critical
   path, or that a huge link is coming, so it can prioritise.

It is upfront that the parts exist separately (ThinLTO, ccache, Nix/Guix,
dedup filesystems), and says the missed opportunity is that they don't talk.

## What we think

**The core observation is right.** Each tool rebuilds or guesses knowledge
another tool already has, and most real build bugs live at those seams. We hit
one this week (below).

**The layers are uneven:**

- *Whole-program before linking* is roughly what LTO, unity builds and the
  SQLite amalgamation already do, and the author concedes it would be about
  as fast as LTO.
- *Per-function incremental builds and whole-program optimisation pull in
  opposite directions*, which the essay doesn't notice. Once the optimiser
  sees across functions, a function's code depends on its callers and
  callees, so one edit invalidates widely. ThinLTO's summaries are the
  industry's compromise; you can't have both at full strength.
- *Source packages that know how to build themselves* already exist: Nix and
  Guix, Bazel and Buck with cross-repo graphs and remote caches, Gentoo, Go,
  Zig. The genuinely missing piece is narrower: the **compiler reporting what
  it actually resolved**, instead of the build system re-deriving it.
- *Static linking plus filesystem dedup* mostly wouldn't work. Inlining and
  per-program specialisation mean two binaries' copies of a library aren't
  byte-identical, and relocations shift everything else, so block dedup finds
  little. It also forgets the bigger win of shared libraries, which is shared
  memory pages at run time, and it makes every libc security fix a rebuild of
  the world (as Nix users know).
- *Scheduler hints* add little. Build tools already schedule the critical
  path in userspace (Ninja, Bazel), and memory-heavy links are handled with
  job pools and cgroups.

## Why it reads like a description of us

| The essay's layer | Ours | What it already knows |
|---|---|---|
| Compiler | **aether** (`ae`, `aetherc`) | the exact module closure; `@link`/`@source` C dependencies; capability facts (`hide`, sealed modules, sandbox grants) |
| Build system | **aeb** | the build graph across languages, with content-addressed artifact keys |
| Package manager | **aeb** too (`ae-add`, `override-dep`, `.packages.ae`, pinned `get.sh`), plus **aether-crossbuild**'s per-target sysroots | sources and versions across package boundaries; the essay's `@libfour:out:four` is close to what aeb's dependency nodes already are |
| "System" (the essay's `system { packages … config … }`) | **aeo** | a declarative composition of what runs where (VMs, jails, containers), with snapshots and rollback |
| Filesystem, scheduler | — | not worth chasing, per the above |

And `ae` already compiles a program plus its whole module closure into one C
translation unit, so we have whole-program compilation by construction.

**The bug that makes the essay's point.** This week aeb served a stale binary
after an edit to aether-ui's `ui/module.ae`. aeb computes its cache key by
re-deriving aetherc's module search rules, and its copy of those rules missed
modules found through a build's `lib()` directories (OpenDisk-ae reaches
aether-ui that way). Fixed in aeb 8e808f7, but the fix makes aeb's *guess*
more complete; it doesn't stop the two drifting again. That is exactly the
essay's "components that don't share information".

## Four things we could do, best first

1. **Compiler → aeb: the resolved import closure.** `aetherc` emits the
   modules and C sources it actually read (like `gcc -MD` depfiles), and aeb
   keys its cache on that instead of re-deriving search rules. The stale-cache
   class of bug goes away, whatever the search rules become. *Small: an
   aetherc flag (an Aether PR) plus aeb reading it.*
2. **aeb → aeo: artifact identity.** aeo deploys by aeb's content hash, so a
   redeploy touches only the nodes whose artifact changed, and rollback means
   "point back to the previous hash". That is the essay's atomic, incremental
   reconfiguration, done at the deployment layer, where it's tractable.
   *Design work in aeo.*
3. **Compiler → aeo: capabilities.** This one goes past the essay. Aether
   knows what a program can reach: sealed modules, `hide`, sandbox grants, and
   whether std's net, fs or spawn modules are in its closure at all. aeo
   already enforces Capsicum, seccomp and network policies, written by hand.
   If `aetherc` emitted a capability manifest beside each binary, aeo could
   derive the jail, seccomp filter or network policy from it, so a program
   gets exactly what its code can use. sae's scoped seeks are the same idea at
   page level. *"The compiler writes the sandbox": Nix, Bazel and Zig don't
   do this, and we own every piece.*
4. **aeo → aeb: deployment targets.** aeo knows a composition runs on FreeBSD
   amd64 and Linux arm64, so aeb could build exactly those cross-triples
   (with aether-crossbuild's sysroots), with no hand-kept target list.
   *Design work across both.*

**Our suggestion:** start with 1, since it's small and fixes a real failure
mode. Think hardest about 3, since it is the distinctive one. 2 and 4 can wait
until aeo has a consumer that needs them.

Questions for you:

- Does 3 fit how you see `hide`, sealing and the sandbox grants evolving? A
  capability manifest makes them a published interface rather than an
  internal compiler fact.
- For 1: a sidecar file next to the output, or a mode on `ae build` that aeb
  queries?
