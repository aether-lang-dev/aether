# Changelog

All notable changes to Aether are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

**Workflow**: New changes go under the `[current]` section. When a PR merges to
`main`, the release pipeline automatically replaces `[current]` with the next
version number before tagging the release.

## [current]

### Fixed

- **Actors are usable from inside closures (#1626).** Two codegen gaps hit
  any actor used from a closure or trailing block — e.g. a `std.spec` `it`
  block. `a ? Msg {}` lowered to nothing, because closure BODIES were
  emitted before the message types had been registered, so the ask could
  not resolve the message and emitted an error comment where the
  expression belonged. And a captured actor handle got an env slot typed
  with the bare actor name before that name was typedef'd, so it read as
  an implicit `int`. Closure declarations now emit early (other code
  references them) while bodies emit after the message definitions, and
  each actor gets a forward `typedef struct X X;`. Actor specs no longer
  need the hoist-the-exchange-into-a-helper workaround.
- **`tinyweb.schema_api` member routes now match (#1625).**
  `show`/`update`/`destroy` were registered as the regex `"/(\w+)"`, but
  the tinyweb router does not do regex — it supports exact segments,
  Express-style `:param`, and `*`. Every real member request 404'd, and
  had since they were written; `update` is the costly one, being the other
  validated verb. They now use `/:id`.
  The auto-mounted `GET {prefix}/schema` also had to move. The router
  matches in REVERSE registration order (tinyweb walks its list forward
  while `http_server_add_route` prepends), so `json_api()`'s own route was
  tried *after* everything the user declared and `show()` answered
  `/schema` with `"schema"` as the id. Each leaf builder now mounts
  `/schema` after registering its own route.


### Fixed

- **`ae run` now forwards `SIGTERM`/`SIGINT`/`SIGHUP` to the program it
  launched.** `ae run prog.ae & ; kill $!` killed only the wrapper and
  orphaned the program, which kept whatever socket it had bound: `ae run`
  builds and then *spawns* the binary (it cannot `exec` it — it still has
  to evict a crashed binary from the cache and delete a non-cached temp
  exe), so the wrapper was always a separate process.
  Ephemeral CI cannot catch this, because the runner is discarded with its
  orphans. On a persistent box the orphan squats its port and the next run
  of the same test fails to bind, so a green run poisons the one after it
  with no code change in between. 25 integration tests background `ae run`
  and kill `$!`, so this is fixed once in the driver rather than 25 times
  in the tests.
  Scoped to the program run: build steps keep the plain path, handlers are
  installed only while the child is alive and the previous dispositions
  restored after, and `waitpid` resumes on `EINTR` (abandoning it there
  would orphan the child — the bug itself). Windows is unchanged; the
  report is POSIX-specific.
  Regression test in `tests/integration/ae_run_signal_forwarding/`,
  verified to FAIL against an `ae` built without the fix.

### Changed

- **All 13 co-located `contrib/` tests now use `std.spec`.** They were
  already beside the modules they test, but were still straight-line
  drivers that called `exit(1)` (or accumulated a failure count) on the
  first mismatch, so one broken assertion hid every later one in the same
  file. Each case now reports independently:
  `i18n/collate` (12), `avcodec` (8), `tinyweb/spec` (13),
  `tinyweb/inventory` (11), `tinyweb/integration` (8),
  `tinyweb/websocket` (3), `tinyweb/schema_api` (7 + 3 skipped), and the
  six `vulkan` specs (159 assertions).
  Assertion preservation was verified per file by comparing call-site
  counts *and* diffing the sorted set of assertion label strings, so a
  renamed or silently dropped check would surface. GPU and
  external-dependency cases use `it_when`, matching each original's
  "print SKIP, exit 0" path.
- `tinyweb/schema_api` gains coverage for `index`, `show`, `update` and
  `destroy` — four of the module's seven exports had never been driven.
  Closing that gap exposed #1625.

### Fixed

- `contrib/vulkan/test_vulkan_resources` no longer leaks the four SPIR-V
  blobs it loads; the original freed none of them.
- `contrib/vulkan/test_vulkan_frames` asserts the failed-submit count its
  slot-recycling loop was already computing but never checking.


### Changed

- `contrib/host/aether` and `contrib/host/factor` gain co-located specs,
  `test_host_aether.ae` and `test_host_factor.ae`, replacing
  `tests/integration/host_{aether,factor}/`. These were the last two
  contrib integration directories whose wrapper did nothing a spec cannot:
  the `[3/3]` discovery phase already builds each bridge archive on demand
  and skips when it cannot, leaving only env-var gating (now `it_when`)
  and, for the aether bridge, a child script to stage — which the spec
  writes itself under a PID-scoped temp dir, so it runs under a bare
  `ae run`.
  The aether spec **gains three checks**: the retired wrapper asserted only
  the happy path, so a child that exited non-zero, failed to compile, or
  did not exist could not be distinguished from success. The factor specs
  are the old driver's assertions unchanged in substance, but the original
  returned on the first failure — one broken call hid every later one.
  The factor specs skip without the aether-lang-dev/factor-language fork.
  To confirm that guard hides nothing broken, they were run with it forced
  on: all 16 assertions execute and fail against the absent library, so
  they are live code rather than text that merely compiles.

### Fixed

- **CHANGELOG structure repaired.** Three `## [current]` headings were
  stranded inside released history (between 0.546.0/0.545.0,
  0.545.0/0.544.0 and 0.542.0/0.541.0), and four version headings were
  duplicated — `0.546.0`, `0.547.0`, and the `0.435.0`/`0.497.0` pair the
  release workflow's own guard already names as known-bad. Each stray
  section held entries that really shipped; only the heading was never
  renamed, so the file misreported which release contained what, and one
  section carried untagged work under an already-released number.
  Each orphan was reattributed from the release tag containing the commit
  that introduced its text (`git tag --contains`), not from its position
  in the file: the three resolve to 0.546.0, 0.545.0 and 0.542.0
  respectively. Sections sharing a version were then merged and the whole
  file ordered strictly descending, so every version now appears exactly
  once.
  **No entry was edited, added or removed** — verified by diffing the
  sorted set of every non-heading line before and after (identical; 462
  entries both sides). Only headings moved.
  Cause is the catch-up-merge fold: merging main across a release folds a
  PR's `[current]` into the released section above it with no conflict
  marker, so it silently survives as a second heading.


## [0.547.0]

### Changed

- `contrib/parsers/xml_expat` gains a co-located spec,
  `contrib/parsers/xml_expat/test_xml_expat.ae`, replacing
  `tests/integration/contrib_xml_expat/`. The highest-risk co-location so
  far: SAX handler registration through `as fn(...)`, user-data pointer
  arithmetic, multi-chunk streaming and a closure-builder veneer. The
  probe's strbuilder captures stay at `main()` scope, outside the
  describe/it closures — the original documents an Aether codegen bug
  where a strbuilder captured inside a nested block dangles at block exit,
  and that constraint had to survive the move. 16 specs, `xml_expat.*` API
  surface identical.
- `contrib/templating/liquid` gains five co-located specs —
  `test_syntax`, `test_values`, `test_tags`, `test_filters` and
  `test_inheritance` — replacing 22 `tests/integration/liquid_*/`
  directories. 293 passing. Grouped by feature rather than one file per
  retired directory, so comments and raw sit in `test_syntax` (they are
  lexical) and Shopify's `{% liquid %}` tag in `test_tags`.
  `test_inheritance.ae` writes its own partial fixtures at startup under a
  PID-scoped temp root, because liquid resolves partials from the
  filesystem and has no in-memory registry; that also retires the
  `LIQUID_PARTIAL_ROOT` env-var coupling, so the suite runs under a bare
  `ae run`. The fixtures keep per-suite subdirectories: the former
  `layout_block` and `extends_super` both ship a `base.liquid` and the two
  differ, so flattening them would silently assert against the wrong
  parent. `liquid_sandbox_gate` stays a `.sh` — it drives `aetherc` over
  generated source and asserts on compiler diagnostics.
  Coverage was verified by diffing every single-line string literal in
  every retired probe against the new specs (954 of 955 present; the one
  absence is dead code the original never used). That check caught
  `liquid_block_tag`, whose 14 assertions an earlier pass had stranded by
  misreading the directory name.

- `contrib/templating/native` gains a co-located spec,
  `contrib/templating/native/test_native.ae`, replacing the four
  `tests/integration/native_templating_{dsl,pretty,skeleton,xml}/`
  directories. Unlike the sqlite and expat cases, those shell wrappers had
  no dependency probe to justify them — this module needs no system
  library, so each `.sh` did nothing but run its probe and check the exit
  code.
  **All 29 assertions carried over verbatim**; the only change is
  `expect_eq()` -> `spec.assert_str_eq()`. That is not cosmetic: the
  originals called `exit(1)` on the first failure, so one broken escape
  rule masked every later assertion in the same file.
  The module also gains **runtime coverage in the nightly for the first
  time** — `templating/native` had no `contrib_check.sh` entry, so it was
  type-checked and never executed. It now appears as
  `PASS templating/native (run)`.

## [0.546.0]

### Added
- Three specs closing coverage gaps left when `contrib/sqlite`'s tests were
  co-located (#1614): `step()`, `bind_i64`/`column_i64`, and
  `bind_blob`/`column_blob`. The retired `tests/integration/sqlite_prepared/`
  touched these and the replacement did not — a reduction found by diffing
  the two API surfaces rather than by anything failing, since a missing test
  is silent by construction.
  Each pins the property that makes the API worth having: `step()` returns
  the raw `SQLITE_ROW`/`SQLITE_DONE` codes that `while step() == SQLITE_ROW`
  callers depend on (the `next_row()` wrapper collapses them to 1/0, so
  testing only the wrapper leaves the documented codes unpinned); the i64
  round-trip uses a value above 2^32, the only kind that proves both halves
  of the (hi, lo) pair survive; and the blob test embeds a NUL, which is the
  entire reason blob binding exists rather than reusing `bind_text`.
  The blob assertion was verified to be able to fail — sabotaged to expect
  the wrong length, it reports the real one (5) rather than passing
  vacuously.


### Changed
- `contrib/sqlite` gains a co-located spec, `contrib/sqlite/test_sqlite.ae`,
  replacing `tests/integration/sqlite_roundtrip/` and
  `tests/integration/sqlite_prepared/`. Those shell wrappers existed almost
  entirely to probe for libsqlite3 and skip when absent — std.spec's
  `it_when` (#1610) expresses that directly, so the wrapper had nothing left
  to do and the assertions move next to the module they describe.
  **Coverage grew rather than moved.** The old probes hand-rolled their
  checks and called `exit(1)` on the first failure, so one broken query
  masked every later one; as independent specs all 11 report. Four checks
  are new: a SQL error must be distinguishable from an empty result set (a
  caller that cannot tell "no rows" from "your SQL was wrong" will silently
  do the wrong thing), writing to a missing table must error, `reset()` must
  really re-bind rather than replay a cached result, and `prepare` must
  reject malformed SQL. The whole prepared-statement surface the old
  `sqlite_prepared` covered — prepare/bind/step/column/reset/finalize/changes
  — is retained.
  Wired into `contrib_check.sh`, which already had a pkg-config column that
  SKIPs an entry when the system library is absent, so the spec runs only
  where sqlite3 exists.

## [0.545.0]

### Added

- Co-located embedding specs for four more host bridges —
  `contrib/host/{duktape,lua,perl,python}/test_host_*.ae` — following the
  pattern established for Ruby. Each drives its bridge through
  `import contrib.host.<lang>`, the path a real user takes, so the specs
  cover module.ae's declarations and the contrib-link plumbing rather
  than only the C shim.
  Each asserts five properties: a script evaluates; the interpreter
  **genuinely runs** (the language computes a value and a second eval
  asserts on it language-side, so a bridge that loaded the library but
  never evaluated would not pass); runtime errors propagate as failures;
  syntax errors propagate as failures (a different path through most
  loaders); and `init` is idempotent while the VM is live.
  All five bridges now have runnable coverage — 25 specs, all passing
  locally. Previously these bridges had only a `~7ms ae check` of
  module.ae and a `-fsyntax-only` of the C shim; nothing executed them.
- `make contrib-host-check`'s `[3/3]` phase resolves each runtime's
  soname the way that ecosystem documents it — `RbConfig::CONFIG`
  for Ruby, `sysconfig` for Python, `Config{archlibexp}` for Perl, and a
  filesystem probe for Lua. Without this the Debian-style packagings
  fail to dlopen: the bridges fall back to unversioned names
  (`libpython3.so`, `libruby.so`) that several distributions do not ship.
  A runtime that cannot be resolved yields **skipped** specs, not
  failures.


- `std.spec` gained skip verbs (#1610): `it_when(cond, desc, reason)` for
  dependency gating, `skip_it(desc, reason)` for a permanent exclusion, and
  `skip_all_if(fw, cond, reason)` for a file-level bail-out. A skipped test
  does not run its body, counts in neither passed nor failed, and prints a
  distinct `⊘` line carrying its reason plus an `N skipped` summary.
  Until now a spec whose subject might be absent from the host — a
  host-language bridge, a driver, a system library — had to fail (presenting
  a *provisioning* gap as a code defect), pass dishonestly, or bail out
  before `spec.init()` where the framework could not see it. The dishonest
  option is the one that bites: a box that skips everything looked exactly
  like a box that passes everything, which is how nine silently-skipping
  contrib/vulkan tests and `ruby SKIP (no demo)` went unnoticed on the
  nightly.
- The aeocha-v1 structured report carries `skipped=N`, and skipped tests
  emit rows with a `SKIP` status and their reason as the message. This is
  **additive** — docs/testing.md's contract already permits new keys and
  requires consumers to ignore unknown ones, so there is no `version=` bump.
  `total` deliberately remains `passed + failed`: folding skips in would
  *repurpose* an existing key, which the contract forbids.
- `contrib/host/ruby/test_host_ruby.ae` — the first **co-located** contrib
  test, written in Aether against `std.spec` rather than as a shell harness.
  It drives the bridge through `import contrib.host.ruby`, the path a real
  user takes, so it covers the module declarations and contrib-link plumbing
  as well as the C shim. Covers evaluation, that the interpreter genuinely
  runs (Ruby computes a value and a second eval asserts on it Ruby-side),
  exceptions *and* syntax errors propagating as failures rather than being
  swallowed, and init idempotence while the VM is live. Uses `it_when`, so a
  box without Ruby reports skipped rather than failed.
- `make contrib-host-check` gained a `[3/3]` phase that auto-discovers
  `contrib/host/*/test_*.ae` and runs them, performing the bridge's
  documented `LIBRUBY_SO` probe first. A bridge that adds a co-located spec
  is picked up with no Makefile change.

### Fixed

- Documented two `contrib.host.ruby` behaviours that cost real debugging
  time (contrib/host/ruby/README.md): **Ruby's `puts` output is buffered and
  silently vanishes** unless `ruby_finalize_host()` runs or the script sets
  `$stdout.sync = true` — the script *did* execute, only the output was
  lost; and **`ruby_finalize_host()` is terminal** — CRuby's
  `ruby_finalize()` destroys the VM permanently, yet a post-finalize
  `ruby_init_host()` still returns 0 and the next `ruby_run()` segfaults
  inside libruby. Call it once at shutdown, or not at all.

## [0.544.0]

### Fixed
- Module namespacing: a **closure parameter** now shadows a same-named
  module-level function or const (#1606). `rename_intra_module_refs`
  skips its `<ns>_<name>` rewrite for locally-bound names, but
  `collect_local_names` recognised only `AST_PATTERN_VARIABLE` /
  `AST_VARIABLE_DECLARATION` / `AST_CONST_DECLARATION` — a closure
  parameter is an `AST_CLOSURE_PARAM`, and closures established no scope
  of their own. So inside a module defining `item()`, the closure
  `|item: ptr| { f(item) }` had its `item` rewritten to `<ns>_item`,
  passing **the address of the function** where the parameter's value
  belonged. The C compiled and type-checked cleanly, and the callee then
  read a function pointer as data.
  Found via aether-ui, where it segfaulted `table_demo` at startup inside
  `aether_string_data`; verified A/B on the reporting commit (`e6feacc`):
  SIGSEGV with the unfixed compiler, runs clean with the fix, from an
  identical source tree. Reduced to a 16-line fixture that printed the
  raw machine code of the shadowed function instead of its data.
  Closures now establish a scope that EXTENDS the enclosing one, so a
  closure can still read enclosing locals and still resolve non-shadowed
  module functions and consts normally.

## [0.543.0]

### Fixed
- codegen: a top-level function whose name is renamed — a leading
  underscore (#279, kept out of C's reserved namespace) or a collision
  with a declared extern (#1366) — now has its VALUE references rewritten
  too, not just its calls (#1598). `rename_calls_to` matched only
  `AST_FUNCTION_CALL`, so `takes_fp(_handler)` (an `AST_IDENTIFIER`) kept
  the old spelling while the definition moved, and the emitted C named a
  symbol that no longer existed: *"'_h_health' undeclared; did you mean
  'ae_h_health'?"* — the compiler suggesting the definition it had just
  renamed. This blocked aeo's HTTP route registration, which is
  `server_get(raw, "/health", _h_health, 0)` throughout.
  The extern-collision half failed far more quietly and was fixed with
  it: the un-renamed reference resolved to the real libc symbol the
  extern declared, so the program linked cleanly and SEGFAULTED at
  runtime (verified on the pre-fix compiler), handing libc's
  `puts(const char*)` an int.
  The rename skips any function body that REBINDS the name, because a
  local may legally shadow a top-level function and is emitted verbatim —
  rewriting it would break a program that compiles today. That direction
  is deliberate: it can only leave a reference un-renamed, never rename a
  binding that should have stayed put.

## [0.542.0]

### Added

- std.spec's fluent value-comparison matchers take an optional trailing
  intent message (#1576): `to_equal`, `to_be_gt`, `to_be_lt`,
  `to_be_truthy`, `to_be_falsy`, `to_equal_str`, `to_contain` and
  `to_start_with` now accept `msg` and render `"<msg> — <generated
  text>"` on failure. The generated text says what the values were; the
  message says why the check matters, which is what a failing-CI
  triager needs. The issue weighed a breaking required-`msg` sweep
  against parallel `*_because` variants — neither is necessary: Aether's
  default arguments resolve through UFCS method-call chaining, so the
  parameter is optional, existing chains are untouched (their wording is
  byte-identical), and one chain can annotate only the links that need
  it. `not_()` negation carries the message too.
- `to_equal_str` now reports through the caret-aligned diff (the
  `assert_str_eq_diff` form) once either string reaches 24 characters,
  where a quoted pair stops being legible and the index + caret is what
  actually locates the differing byte. Shorter strings keep the compact
  form.
- `ae add` installs from a published release artifact instead of always
  git-cloning (#1360). With `@version` it looks for
  `<repo>-<tag>-<os>-<arch>.tar.gz` (then `.zip`) under the package's
  `releases/download/<tag>/`, the layout Aether's own releases use, and
  falls back to the clone when nothing matches — so a package that
  publishes no artifacts behaves exactly as before. `--source` forces
  the clone. A sibling `<asset>.sha256` is verified when published: a
  MISMATCH is fatal, installs nothing, and deliberately does NOT fall
  back to git (which would defeat the verification); a missing checksum
  installs with a warning. `AE_RELEASE_BASE_URL` repoints the origin at
  an internal mirror. The consuming half already existed — the
  binary-import prepass reads a `--emit=lib` artifact's catalog and
  synthesizes its interface — so this closes the fetch gap only.

- `WINDOWS=1` cross-build knob: build the `ae`/`aetherc` toolchain FOR
  Windows from a Linux host with `zig cc -target x86_64-windows-gnu`
  (#1592). Companion to `FREEBSD=1`, and simpler — zig bundles the
  mingw-w64 headers and CRT, so there is no sysroot to fetch.
  Capability-lean by design (OpenSSL/zlib/nghttp2/YAML forced off, as
  the host's pkg-config would poison the target; vendored PCRE2 keeps
  std.regex).
- `AE_TEST_RUNNER`: when set, `ae run` / `ae test` prefix the binary they
  just built with that runner instead of exec'ing it directly — the
  cargo `CARGO_TARGET_<triple>_RUNNER` pattern (#1592). Empty by
  default, so every existing caller is unaffected. Keeps
  target-awareness at the edge: nothing in std.spec or the test sources
  knows it is running under an emulator.
- CI fast lane `windows-cross` in windows.yml (#1593): cross-builds
  compiler + ae + stdlib for Windows on an ubuntu runner and verifies
  the artifacts really are PE. The slow MSYS2 legs now `needs:` it, so a
  fast-lane failure skips ~20 minutes of doomed compute, and Windows-only
  COMPILE breakage surfaces minutes into a PR instead of ~20. It is an
  EARLIER signal, never a replacement: MSYS2 remains the fidelity tip and
  the only tier that runs anything.
  Running the suite under Wine was attempted and deferred: `ae` is a
  compile-and-run driver, so `ae run` inside a Wine prefix wants a
  Windows C toolchain there to compile the C it emits, and driving the
  native `ae` via `AE_CC` needs a Windows `libaether.a` alongside the
  native one. The analysis of what such a lane must never claim to cover
  is kept in `tests/ae_sweep_prune_wine.txt`.
- `make test-ae` gained `AE_SWEEP_EXTRA_PRUNE=<file>`, layering an extra
  exclusion list over `tests/ae_sweep_prune.txt` — applied to both the
  `.ae` and the `.sh` sweeps (the latter previously had no prune filter
  at all).
- A build-target stamp (`build/.build-target`): a native build after a
  cross build (or vice versa) now fails immediately with an actionable
  message instead of dying deep in the link with "unknown file type" —
  object and archive formats do not mix in one `build/` tree. `make
  clean` / `make help` are exempt so the guard never blocks its own
  remedy.

### Fixed

- README's "enforced from compile time down to libc" now names the
  platform boundary: the libc (LD_PRELOAD) tier is Linux/FreeBSD only,
  and Windows gets the compile-time and scope layers (#1594).
  docs/containment-sandbox.md states that Windows is out *by
  construction* rather than by backlog, and that running under Wine
  does not exercise containment at all (preloading into the wine
  process intercepts wine's libc calls, not the guest's) — so a green
  Wine run must never be read as containment coverage.

## [0.541.0]

### Fixed
- `--emit=lib`'s `aether_lib_meta()` catalog entry point is now emitted
  weak (`__attribute__((weak))` under GCC/Clang), so linking N lib TUs
  into one artifact — aeb's regen shape, one `--emit=lib` C per module
  and one final link — no longer dies with N-1 "multiple definition of
  `aether_lib_meta`" errors or forces `-Wl,--allow-multiple-definition`
  back onto the link line (the GNU-only escape hatch the 0.539 multi-TU
  static-clone model retired; ld64 rejects it) (#1590). Lone-`.so`
  consumers are unchanged: one definition, same dlsym contract; in a
  multi-TU link the first TU's catalog wins. The
  `multi_tu_import_link` regression now has an `--emit=lib` phase that
  links three catalog-bearing TUs with no escape hatch and pins the
  weak linkage via nm.

## [0.540.0]

### Added
- `std.mutation` — Tier-1 (text-based) mutation-testing driver, adopted
  from the sunsetting aeocha repo's `contrib/mutate` (the last real
  technology blocking that repo's archival). One entry point,
  `mutation.run(sut, test, lib_dir)`, plus a runnable front-end at
  `examples/mutation-testing/mutate.ae` and a worked calc example.
  Perturbs the SUT source one padded-operator (or string-literal) site
  at a time, rebuilds, re-runs the suite, and classifies each mutant
  killed / survived / no-compile via std.spec's structured report
  (`AE_SPEC_FORMAT`/`AE_SPEC_REPORT`), so a non-compiling mutant never
  masquerades as a kill; the SUT is restored byte-identical. Sub-builds
  honour `AE_BIN` (in-tree harnesses point it at `build/ae`). The two
  deterministic aeocha regression fixtures came along
  (`tests/integration/mutation_testing/`): the operator fixture pins
  `1/2 … 50%` with a MUL→DIV survivor + md5-identical restore, the
  strings fixture pins the string-boundary skip (an operator inside a
  string literal is never mutated as code). docs/mutation-testing.md
  documents usage, the operator set, and the honest Tier-1 limits; the
  AST-level upgrade path is the reason the tool now lives in-tree.

### Fixed
- FreeBSD: the `ae` driver, `aetherc` module resolver, and `ae help` now
  locate the running executable via the `KERN_PROC_PATHNAME` sysctl
  (with a `/proc/curproc/file` fallback for jails that deny the sysctl)
  instead of Linux-only `/proc/self/exe`, so `ae build`/`ae run` work on
  a stock FreeBSD install without a mounted linprocfs (#1586). Verified
  on GhostBSD 14 (build + `ae build` + self-path acceptance).
- `std.spec` no longer leaks closure environments: `before_each` /
  `after_each` store hook closures through the list-owned coercion (the
  list reclaims box and environment at teardown), and `it` / `it_within`
  invoke the test body themselves instead of forwarding it, so the
  caller-side transient-callback drain frees each capturing it-callback's
  environment (#1577). `test_spec_module` is now valgrind-clean (0
  leaks, was 4 blocks) and its `tests/leaks_known.txt` cap is removed.
  docs/closures-and-lifetimes.md documents both leak shapes.

## [0.539.0]

### Added
- Regression pin for the multi-TU link model (aeb's orchestrator shape,
  from the aeb-line asks): four modules compiled to separate objects
  link with NO -Wl,--allow-multiple-definition and run — imported
  functions are cloned per-TU as `static` (landed with the #1568 audit),
  a module's own functions stay external. Guards the macOS/ld64 story:
  downstream multi-TU tools need no GNU-only dedup flags.
- The `AE_SPEC_REPORT` aeocha-v1 test-report format is now a documented,
  versioned CONTRACT (docs/testing.md): header keys, `---` separator,
  row shape, and the incompatible-change-bumps-`version=` rule —
  `_format_aeocha_v1` carries a producer-side pointer. Downstream
  parsers (aeb's driver_test reporting) may depend on it deliberately.
- docs/testing.md states `contrib.aeocha`'s retirement explicitly:
  nothing in std/contrib references it (verified), `std.spec` +
  `std.os.testing` + `std.http.client.httptest` are the replacements,
  and the env-file report transport replaces the old IPC-pipe
  convention for spec children.
- **`make check-standalone`: a compile gate for every C source with its own
  `main()`.** The runtime examples, micro-benchmarks and demos are linked by no
  target, so nothing noticed when they stopped compiling: three carried include
  paths that had moved directories, one was written against a renamed logging
  API, and the whole `tests/runtime/bench_*` set failed on ARM because
  `micro_profile.h` included `<x86intrin.h>` unconditionally. All of them build
  clean now, the cycle counter has an AArch64 and a portable path, and the gate
  runs inside `make ci` and `make test-all`.

- **139 compiler and memory unit tests that were never run.** `tests/compiler`
  held a parser, typechecker, codegen, pattern-matching, struct and
  module-orchestrator suite that no target compiled; the C test binary went
  from 230 to 390 tests (`test_typechecker.c` stays out until #1575, a
  pre-existing leak in `typecheck_program`, is fixed; it fails the
  LeakSanitizer job and a suppression is not a fix). Three of them asserted against the first 4 KB of
  generated C, which is prelude, so they matched prelude text rather than the
  program under test; they now read the whole output. `tests/compiler/test_structs.c`
  registered nothing at all: its cases were plain functions the harness never
  saw, behind a comment claiming they were wrapped.

- **A test for `std.http.server.lb`**, which shipped with none: the backend-list
  parser, the config constructors and the two refusals that report a bad call
  instead of standing up a broken load balancer.

### Fixed

- **A use-after-free in the compiler's diagnostic context.** `codegen_note_diag_pos`
  and `codegen_note_diag_func` stored borrowed pointers into AST nodes, so a
  process that compiled one program, freed it, and compiled another read freed
  memory the next time a codegen diagnostic fired. Valgrind counted 906 invalid
  reads from that one dangling global once the compiler tests ran in the same
  binary. They copy now.

- **Parser error paths leaked the nodes they had built.** A match arm whose
  `->` was missing, a range whose upper bound failed to parse, an alternation
  with a bad element, and a struct definition that hit EOF or a bad field name
  all returned NULL and dropped everything parsed so far. Found with
  LeakSanitizer over the newly-running compiler tests: 3428 allocations before,
  0 after (the test helpers were leaking their token streams too).

- **A generated actor's struct was missing the last `ActorBase` field.** The
  scheduler is handed a pointer to the generated struct and casts it, so the
  prefix has to match; codegen stopped at `dead` and left out `alloc_size`,
  which put the first user state field at exactly that offset (1640 on
  LP64/arm64). `scheduler_spawn_actor` wrote the allocation size through it,
  and `scheduler_destroy_actor` later passed the field's *value* to
  `aether_numa_free` as a length. Where that resolves to `free()` the size is
  ignored and nothing showed; under libnuma on a multi-node host it is
  `munmap(ptr, <state field>)`, which either fails and leaks the actor or
  unmaps memory that was never part of it. The field is emitted now, the
  fallback allocation path sets it, and every generated actor carries a
  `_Static_assert` that its `alloc_size` sits at the `ActorBase` offset, so
  the next field added to `ActorBase` breaks the build instead of the heap.

- **`std.http.server.lb` had no way to release a config.** `health()`,
  `breaker()`, `cache()` and the `no_*()` forms allocate a handle that nothing
  could free; `serve()` blocks forever so it never mattered there, but any
  program that builds one and takes another path leaked it. `config_free()` is
  exported.

- **Generated code no longer breaks a downstream `-Wall -Werror` build.**
  Imported wrappers are emitted with internal linkage, so a translation unit
  that uses part of a module is left with unused statics. They now carry
  `AETHER_MAYBE_UNUSED`, which is the compiler's business rather than a `-Wno`
  flag every consumer has to remember.

- **`import std.message` (a module path segment spelling a keyword).** Module
  path segments were matched against a short whitelist of keywords, so a
  stdlib module named after a reserved word could only be imported by
  backtick-escaping it, and the parse failed with an error pointing at the line
  *after* the import. A segment is a name, and is parsed as one.

- **`ae help` dropped a file named `-o` into the working directory.** It
  compiles the script to a sink to collect diagnostics and passed that sink as
  `-o <path>`, but aetherc takes the output positionally: it read the flag
  itself as the output path and wrote the generated C there, in whatever
  directory the user ran `ae help` in. The sink is passed positionally now,
  aetherc refuses an output path that starts with a dash rather than creating
  such a file, the `.gitignore` entry that had been hiding it is gone, and
  `ae help` is asserted to leave its working directory empty.

- **The language server dropped `aether-lsp.log` into whatever directory the
  editor started it in.** Logging is opt-in through `AETHER_LSP_LOG=<path>`, and
  `lsp_server_create` checks its allocation instead of dereferencing NULL.

- **Two self-assigning `realloc` calls.** `store_emit_string` in the YAML
  emitter lost the old buffer on failure and left the capacity claiming a size
  the now-NULL buffer did not have, so every later call under that size
  returned NULL; `aether_register_test` wrote through the null pointer one line
  down. Both go through a temporary and handle failure.

- **Hand-mirrored `ActorBase` layouts in the scheduler tests.** The fields live
  in one `AETHER_ACTOR_BASE_FIELDS` macro now: the mirrors were already one
  field stale, and the drift is silent memory corruption rather than a
  compile error.

- **Tests and examples no longer litter the tree they run in.** contrib tests
  run from a per-test scratch directory that mirrors the repo through symlinks,
  so an example writing `sprites.ppm` writes it there; the codegen tests
  generate into `tmpfile()` (an assertion longjmps, so `remove()` never ran on
  failure); the `log.init` test deletes the log it writes.

- **`tests/compiler/test_msvc_compat.sh` could not compile** what it was
  checking (two headers had moved into subdirectories since the include list
  was written), so the C11 `-pedantic -Werror` gate on generated code had been
  silently passing over a failed compile.

- **`make test-manual-runtime` could not compile** (an include path left behind
  when `actor_state_machine.h` moved), and
  `tests/integration/aether_extern_param_annotation` could never pass on macOS:
  it linked with GNU's `--allow-multiple-definition`, which `ld64` rejects and
  which the static-marking made unnecessary years ago. It now also compiles the
  generated C with `-Wall -Wextra -Werror` rather than `-w`.

### Removed

- **Dead code that no build compiled**: the superseded `runtime/actors/aether_actor.c`
  (whose `aether_send_message` had a different signature from the live one, and
  whose header advertised ten functions with no definition anywhere), three
  scheduler/foundation tests that handed the scheduler a hand-rolled actor
  struct with a stale layout and hung on the corruption, an HTTP test written
  against an API renamed long ago, two duplicate copies of the test harness,
  three copies of a test runner `main()`, an unused 318-line test framework
  header, a byte-identical duplicate of `bench_scheduler.c`, and two profiler
  demos that nothing built. Coverage that only existed in the deleted files
  (wildcard route matching, the middleware chain) moved into the suite that
  runs, including the message-coalescing cases (the only coverage that header
  had) and a bidirectional ping-pong between two actors. `tests/compiler/run_tests.sh`
  went too: it had been broken since a source file it lists was removed, and
  the tests it compiled now run in `make test`.

## [0.538.0]

### Added
- `std.os.testing` + `std.http.client.httptest` (stage 2 of the aeocha
  tease-out): the process-shape matchers (`expect_exit`,
  `expect_stdout_*`, `expect_stderr_*` over `os.run_capture` triples)
  and the HTTP-shape matchers (response-handle asserts, one-call
  GET/POST conveniences, `within()`/`without()` one-shot retry budgets,
  and the generic poll-a-predicate `eventually()`),
  ported verbatim from aeocha and reporting through `std.spec`'s ambient
  framework cell. The arms are negative-fire tested: a probe
  deliberately fails matchers and asserts the run goes red with the
  right messages. `httptest` is named per Go's `net/http/httptest` — and
  because two co-imported modules cannot share a namespace tail.
- **`std.spec` — a BDD test framework in the stdlib.** The pure,
  dependency-light core of the standalone
  [aeocha](https://github.com/aether-lang-dev/aeocha) framework is now
  shipped as `import std.spec`: `describe` / `it` / `it_within`,
  `before_each` / `after_each` hooks, the flat `assert_*` family
  (`assert_eq`, `assert_str_eq`, `assert_str_eq_diff`, `assert_gt`,
  `assert_contains`, `assert_null`, …), the fluent subject-first chain
  (`expect_int(x).to_be_gt(0).to_equal(4)`, `expect_str(s).to_contain(…)`,
  `not_()`, `satisfies`, plus the exported `IntSubject`/`StrSubject` for
  UFCS-extended matchers), the string-list collection matchers
  (`expect_list_size`/`_empty`/`_has_str`/`_contains_all`/`_every`), and
  the `Duration`-budget timing matchers (`it_within`,
  `expect_elapsed_under`). Failures are soft — a check records and
  continues, so several assertions report per `it`. Assertions read an
  ambient framework cell set by `init()`, so no `fw` threads through them;
  only the top-level `describe(fw, …)` and `run_summary(fw)` take it. The
  process- and HTTP-shaped integration matchers and the mutation-testing
  facility stay in aeocha. New doc: `docs/testing.md`. Imports only
  `std.list`/`std.map`/`std.string`/`std.strbuilder`/`std.os`/`std.file`,
  so it needs no build wiring.
- **`ae test --format=<fmt>` structured reporting (opt-in).** `ae test`
  gains a machine-readable reporting mode, off by default. `--format=tap`
  emits one aggregated TAP version 13 stream with the test points from
  every file renumbered into a single `1..N` sequence and each failure's
  captured message as a YAML diagnostic block; `--format=aeocha-v1` emits
  the aeocha v1 key/value + tab-packed-rows report, one block per file. In
  a report mode the human progress lines and summary are suppressed so
  stdout carries only the machine stream; the exit code still reflects
  pass/fail. Under the flag `ae test` hands each child `AE_SPEC_FORMAT` +
  `AE_SPEC_REPORT` (a path); `std.spec`'s `run_summary` writes the report
  there, so `AE_SPEC_FORMAT=tap AE_SPEC_REPORT=out.tap ae run t.ae` works
  standalone too.

### Fixed
- **`std.spec` tears down its framework tree.** `run_summary` now frees
  suites, hook closure-boxes, it-records and the fluent chain's
  registered string copies (`expect_str`'s defensive copy is registered
  on the framework and released at end-of-run — borrowing instead is a
  valgrind-confirmed use-after-free). Took the spec regression test from
  92 leaks to a capped 4 on the macOS leaks gate; the residual 4 are
  compiler-owned closure environments, tracked in #1577.
- **Mutable module-level `var` writes miscompiled for any dotted module
  path** (`std.*`, `contrib.*`, nested local `a.b`). A `name = expr`
  assignment to a module-scope `var` inside one of that module's own
  functions was emitted as a fresh shadowing local instead of a store to
  the shared file-scope static, so the global kept its initial value and
  the next read of it dereferenced `NULL` (segfault). The import-merge
  write-rename pass looked the module up by its short namespace tail
  (`spec`) while modules are registered under their full dotted path
  (`std.spec`), so the rename never fired; single-segment local imports
  (path == namespace) were unaffected, which is why it stayed latent.
  `name_is_module_global_var` now falls back to a namespace-tail scan of
  the registry when the exact lookup misses. Surfaced by `std.spec`, the
  first stdlib module to use a top-level mutable `var`; no working module
  changes its generated C. (`compiler/aether_module.c`.)

## [0.537.0]

### Added
- Wycheproof wave 4 (#739): RSA decryption families — RSAES-PKCS#1 v1.5
  (67 Bleichenbacher-shaped cases) and RSA-OAEP 2048/SHA-256 (37 Manger-
  shaped cases; the 8 labelled cases are counted-skipped — `decrypt_oaep`
  has no label parameter yet). Own CI suite slot: every case is a
  private-key modexp.

### Fixed
- `std.cryptography.rsa`: `decrypt_oaep` and `decrypt_pkcs1` accepted
  UNREDUCED ciphertexts — c + n decrypts identically after the mod-n
  exponentiation (the ciphertext-side twin of 0.535.0's signature
  malleability fix; RFC 8017 §7.1.2/§7.2.2 step 1 requires exactly
  k octets with c < n). The existing range gate now guards both decrypt
  paths. Caught by Wycheproof rsa_oaep tcId 27 ("added n to c").

## [0.536.0]

### Added
- Wycheproof wave 3 (#739, #1298): ECDSA P-256 **DER-encoded** signature
  vectors (371 cases) driven through the real TLS parse path
  (`tls13_cert.split_ecdsa_sig`, now exported), and RSA-PSS
  2048/SHA-256/MGF1-32 vectors (108 cases — clean on import; the
  0.535.0 `sig_in_range` guard already covers PSS).

### Fixed
- `std.cryptography.tls13_cert.split_ecdsa_sig` accepted six classes of
  malleable ECDSA signature encoding (Wycheproof DER family): BER
  long-form/non-minimal/indefinite lengths, superfluous INTEGER padding,
  trailing garbage after the SEQUENCE, and oversized scalars (r + 2^320)
  silently truncated by the fixed-width conversion. An ECDSA-Sig-Value
  must now be strict minimal DER — enforced by re-encoding the parsed
  (r, s) and requiring byte-equality with the input — with non-negative
  scalars whose magnitudes fit the curve width. TLS certificate
  verification inherits the strictness; legitimate certs are unaffected
  (the asn1 reader itself stays BER-tolerant for X.509 field reuse).

## [0.535.0]

### Added
- Project Wycheproof adversarial test vectors for the crypto suite (#739,
  #1298): pinned vector files under `tests/vectors/wycheproof/` with
  per-family drivers for X25519 (518 cases: twists, small-order points,
  non-canonical encodings — all pass), ChaCha20-Poly1305 (325 cases incl.
  60 forgeries — all pass, and sealing is checked as well as opening),
  AES-GCM (all supported-shape cases pass; unsupported IV/tag sizes are
  counted, not silently dropped), and X448 (stride-sampled by default —
  its bignum field math costs ~1s/op; `WYCHEPROOF_FULL=1` sweeps all).
  The adversarial complement to the existing RFC/ACVP KATs. Wave 2 adds
  Ed25519 (151 cases), ECDSA P-256/SHA-256 in P1363 form (262,
  stride-sampled), RSA PKCS#1 v1.5 2048/SHA-256 (259), HMAC-SHA256 and
  HKDF-SHA256 — and caught two real accepted-forgery bugs (below).

### Fixed
- `std.cryptography.ed25519`: point decoding accepted the non-canonical
  encoding "y = 1 with the sign bit of x set" (x = 0 has no negative
  form; RFC 8032 §5.1.3 step 4 requires failure) and silently produced a
  garbage x when no square root exists (step 3). Both now reject.
  Caught by Wycheproof ed25519 tcId 151 — verify accepted a forgery.
- `std.cryptography.rsa`: `verify_pkcs1` accepted unreduced signatures
  (s + n verifies identically after the mod-n exponentiation — RFC 8017
  §8.2.2 step 1 requires s < n and exact k-octet length) and had no
  length check; `verify_pss` had the same missing range check. Both
  verifies now reject out-of-range and wrong-length signatures.
  Caught by Wycheproof rsa_signature tcId 244 ("signature is not
  reduced", SignatureMalleability).

## [0.534.0]

### Added
- `std.regex` works everywhere without a system libpcre2-8 (#1389): the PCRE2
  engine (pinned upstream 10.44, BSD-3-Clause) is vendored under
  `std/regex/pcre2/` and compiled as a single translation unit when no system
  library is available. `ae build --target ...` cross builds get a working
  regex with no `CROSSBUILD_SYSROOT` for every target; native builds on boxes
  without libpcre2-8 compile the vendored engine instead of silently stubbing
  `std.regex` out (the failure mode behind the aether-ui SVG-path
  misdiagnosis); `ae`'s no-`libaether.a` source fallback now always has a
  working regex too. A system libpcre2-8 (pkg-config) or a sysroot-staged one
  keeps precedence; `PCRE2=0` opts out, `PCRE2=vendored` forces the vendored
  engine. Behaviour is identical either way — `std.regex` never used PCRE2's
  JIT, so the vendored interpreter matches the system-library path.

## [0.533.0]

### Added

- **contrib/vulkan: materials, draw batching, 16-bit indices and mipmaps.** A
  pipeline owned one descriptor set, so a frame could use exactly one texture:
  binding a second overwrote what the first draw was still going to read.
  `material_create(pipe)` now gives each draw its own set, and a target holds a
  list of draws (`batch_add(t, mat, first, count)`, `batch_reset(t)`), each
  naming the material it uses. Four differently textured sprites in one frame
  from one pipeline is `example_sprites.ae`. Sets come from pools of 16 the
  pipeline grows on demand; an empty batch stays the default and draws all the
  geometry once, so every existing caller is unchanged.

  `indices_reserve_ex(t, count, 16)` halves the index buffer for meshes under
  65536 vertices, and refuses a value the width cannot hold rather than
  wrapping it into the wrong triangle.

  `texture_create_ex(dev, w, h, mipmapped, linear, repeat)` builds a mip chain
  on upload with `vkCmdBlitImage` and chooses the sampler filter and addressing
  mode. A 128x128 checkerboard minified eight times reads back as pure black
  and white texels without a chain and as its true mean of 128 with one.
  Mipmaps require the device to advertise linear blitting, and creation fails
  with that reason rather than returning empty levels.

## [0.532.0]

### Added

- **contrib/vulkan: frames in flight, and a configurable GPU timeout.** `draw`
  records, submits and blocks on a fence, so the CPU idled for the whole GPU
  execution. `submit` now hands work to the queue and returns, and
  `target_set_frames(t, n)` gives the target `n` slots to rotate through. On an
  M1 Pro at 512x512, 300 frames each with a different clear colour so nothing
  is served from the recorded command buffer: **351 us per frame synchronous,
  161 at two frames in flight, 155 at three**, a 2.2x improvement.

  Each slot owns its command buffer, its fence and its own readback buffer,
  because a shared one would let frame N+1 overwrite pixels frame N had not
  been read yet. That is `width * height * 4` per slot, so the feature is
  opt-in and one frame stays the default: the synchronous shape is what makes
  the existing tests deterministic, and it is unchanged.

  `pixel`, `copy_rgba` and `save_ppm` wait for the newest submitted frame
  before reading, and the recorded-command cache is per slot so a geometry
  change invalidates all of them rather than the next one only.
  `target_set_timeout_ms` replaces the hardcoded five seconds, which was a hang
  detector rather than a frame budget.

### Fixed

- **`ae build` ignored `@link`, forcing duplication that `-D` could silently
  invalidate** (#1549). A module declares its native dependencies with
  `@link("-laether_sqlite -lsqlite3")`, and codegen unions those across the
  resolved import closure into the `// aether-link:` header on the generated C
  (#1259). `ae build` never read it back, so consumers had to restate the same
  `-l` flags in `aether.toml` — and with the archive on the search path but the
  flags only in `@link`, the build still failed with `undefined reference to
  sqlite_open_raw`.

  Conditional compilation is what made this worth fixing rather than
  documenting. The link requirement is derived from the AST, so it is
  transitive (a module three imports deep contributes its own deps) and it
  tracks `when defined`: an import inside a losing region is gone before
  codegen, so its `@link` never appears. `aether.toml` is static, so a
  hand-written `-lsqlite3` is passed on *every* build including the ones that
  dropped the import — reintroducing exactly the coupling `when defined`
  removes, from a second source of truth that cannot track the code.

  `ae build` now reads the header back and appends the tokens **before**
  `aether.toml`'s `link_flags`, so consumers keep override authority. `-L`
  search paths stay the consumer's job, being site-specific in a way a module
  cannot know — with one exception: `<lib_dir>/contrib` is added, the same
  dev-layout path `host_bridge_a_path()` already searches, so the veneer
  archives `make contrib` builds resolve without configuration. It is emitted
  only when that directory exists: GNU `ld` ignores a missing `-L` silently but
  macOS `ld` warns, and the warning reaches stdout and corrupts every test that
  compares exact program output.

  Only module-declared flags are taken. The header also carries rows from
  codegen's static `g_link_reqs` table (`std.http` → `-lssl -lcrypto
  -lnghttp2`, and so on), which exist for downstream C builds that have no
  other way to learn them. `ae` already passes those from `AETHER_*_LIBS`,
  which the Makefile fills in from pkg-config and leaves **empty when the
  library is absent** — and that emptiness is load-bearing: on a box without
  libnghttp2, `std.http` links without it and the h2 surface degrades to its
  "unavailable" stub. Taking the static row would put `-lnghttp2` back
  unconditionally and fail with `cannot find -lnghttp2`, turning a graceful
  degradation into a build error on exactly the machines the capability probe
  exists to serve. So tokens governed by a capability macro are dropped; a
  module's own `@link` names something the toolchain does not probe for, which
  is why the module had to declare it, so it survives the filter.

  A workspace with no `link_flags` and no `extra_sources` now builds and runs
  against contrib.sqlite on the strength of `@link` alone, while the same
  source with the symbol unset produces a binary that does not link libsqlite3
  at all — asserted with `ldd`, since a binary that merely does not *call*
  sqlite is not the same as one that does not *link* it.

## [0.531.0]

### Added

- **contrib/vulkan: depth attachments and multisampling.** The offscreen target
  was one colour attachment at one sample, so draw order decided visibility and
  every edge was a staircase. `target_create_ex(dev, w, h, depth, samples)` adds
  either or both: the depth format is chosen from what the device reports,
  preferring plain depth over combined depth+stencil, and the sample count is
  checked against `framebufferColorSampleCounts & framebufferDepthSampleCounts`
  rather than rounded down silently. A multisampled colour attachment resolves
  into the single-sample image, so readback, `pixel()` and `save_ppm` are
  unchanged, and both multisampled attachments are transient so a tiler never
  writes them to memory.

  The test is built so a feature doing nothing fails it: the depth case draws
  the same two overlapping triangles in both submission orders, requires the
  overlap to match, then runs the identical comparison on a target with no
  depth attachment and requires it to disagree. The MSAA case counts pixels
  that are neither background nor a saturated primary, 0 at one sample against
  105 at four.

## [0.530.0]

### Fixed

- **Freeing an interpolated string leaked it once it crossed a boundary.** An
  Aether `string` is either a refcounted AetherString or a plain malloc'd
  payload, and the runtime cannot free the second: given a bare `char*` it
  cannot tell a heap payload from a static literal, so `string.free` no-ops.
  The compiler's per-variable ownership flag covered the direct case, but a
  parameter carries no flag, so handing an interpolation to a helper that frees
  what it is given leaked one block per call.

  Interpolation now produces the refcounted shape, which the runtime can free
  wherever the value ends up. The payload lives in the same allocation as the
  header, recognised on release by position rather than by a flag, so it costs
  a larger `malloc` rather than a second one: measured at +1.6% on a loop that
  does nothing but interpolate two million strings, inside that benchmark's own
  run-to-run spread. An earlier version that allocated the header separately
  cost 16%, which is why it does not.

## [0.529.0]

### Added

- **contrib/vulkan is safe to use from several actors.** Vulkan requires the
  caller to synchronise a `VkQueue` and a `VkCommandPool`, and the module made
  no claim either way, so two actors sharing a device was undefined behaviour
  while being the obvious thing to write in an actor language. There is now one
  lock per device, taken across queue submission and every command-pool access,
  and the README states what is covered: concurrent draws to separate targets
  and concurrent target and texture lifetimes are safe, two threads on one
  target are not, and `last_error()` stays thread-local.

  The probe behind `available()` is serialised too. Repeating it would have
  been harmless, but it fills a 256-byte device-name buffer that
  `device_name()` reads, and that read could have seen it torn.

  `example_parallel_render.ae` shows the point: four actors rendering their own
  tile on one device, assembled into a contact sheet. Both vulkan examples are
  now RUN by `make contrib-check` rather than only compiled.

  The cost is not measurable on the offscreen path, where a draw already blocks
  on a fence: the two-actor test takes the same wall time with the lock
  compiled out. That test asserts the contract holds rather than proving the
  lock is load-bearing, and says so: run with the lock removed it passes on
  MoltenVK, because a driver tolerating unsynchronised access is not the same
  as the access being defined.
## [0.528.0]

### Added

- **Conditional compilation on build symbols.** `select()` could choose a value
  on a fixed set of platform keys, and nothing could exclude a subsystem: a
  library with an optional part had to ship it in every binary behind a runtime
  `if`, symbols and all. Builds now declare symbols with `-D NAME` (on both
  `ae build` and `aetherc`) or `[build] defines` in aether.toml, and
  `when defined(NAME) { … } else { … }` guards a group of declarations or a
  group of statements, with `!`, `&&`, `||` and parentheses.

  A losing region is **dropped from the AST**, not wrapped in a preprocessor
  `#if`. It is gone before type checking, so nothing it names has to exist, and
  its symbols are not in the binary: `nm` on a build without the symbol finds
  no trace of the guarded functions. That is what a runtime `if` cannot do and
  what the feature is for.

  `select()` also takes build symbols as keys now. A symbol that is set wins
  outright, since it is known at compile time; otherwise the platform chain
  decides as before.

  The build cache key includes the symbols. Without that, building the same
  source twice with different `-D` serves the first artifact for the second and
  the region silently comes back.

### Fixed

- **`select()` over strings printed a pointer as a number.** Nothing inferred
  the expression's type, so it fell to unresolved, codegen defaulted it to
  `int`, and `s = select(linux: "a", other: "b")` assigned a pointer into an
  int slot. It now takes its type from its branches, and branches that
  disagree are an error rather than a surprise on whichever platform happens to
  compile the odd one. Pre-existing, not a regression: same output on the
  released compiler.

## [0.527.0]

### Fixed

- **An actor `?` ask could only ever return an int.** `x = actor ? Request {}`
  was always declared `int`, whatever the handler replied, so any non-int reply
  failed the C compile with `-Wint-conversion` and pointed at generated C
  naming no Aether cause. Every other part of the pipeline already handled
  other types: the reply statement deep-copies a string, and the ask expression
  emits `const char*`, `double`, `int64_t` or `void*` as the reply requires.
  The gap was inference, which had no case for an ask, so the receiving
  variable fell to unresolved and codegen defaulted it to int. That default was
  what the locationless `unresolved type` warning had been pointing at all
  along.

  The request-to-reply mapping now lives in one place, `analysis/actor_reply`,
  which the type checker and codegen both call: they cannot disagree about what
  an ask yields, which is exactly how the expression's type and the variable's
  type came apart. A string reply is also tracked as owned, since the handler
  deep-copied it, so it is reclaimed at scope exit instead of leaking a copy per
  ask.

- **`string.free` on an interpolated string leaked it.** An Aether `string` is
  either a refcounted AetherString or a plain malloc'd payload, and the runtime
  can only free the first: given a bare `char*` it cannot tell a heap payload
  from static storage, so it no-ops. The compiler knows, through the
  per-variable ownership flag, but the tracker was withheld from any variable
  passed to a free, so the emitted call was the bare runtime one and every
  interpolation freed by hand leaked.

  Fixing it meant separating two questions the escape walker had been answering
  with one predicate. At a caller's own call site, `string.free(local)` must not
  count as an escape, or the tracker is withheld and the value leaks. Inside a
  callee's body, the same call on a parameter must count as one, because that
  function has consumed what it was handed and the caller must not free it
  again. Answering the second the way the first is answered makes every caller
  of a `free_it(s) { string.free(s) }` helper double-free, which is how
  `contrib/tinyweb/example_schema_api` aborted while this was being built.

  Still open, unchanged in either direction and measured: handing a
  plain-payload string to such a helper leaks, because a parameter carries no
  ownership flag. That needs per-parameter ownership.

## [0.526.0]

### Added

- **contrib/vulkan: caller-described vertex layouts and index buffers.** The
  pipeline was built with exactly one vertex layout, an interleaved vec2
  position and vec3 colour, so "bring your own shader" held only for shaders
  with that signature, and without an index buffer every triangle cost three
  full vertices. `pipeline_create_ex` now takes a layout the caller describes
  as bindings and attributes with formats, offsets, stride and input rate, and
  `indices_reserve` / `indices_set` drive `vkCmdDrawIndexed`. A mesh with
  position, normal and UV renders, and a quad draws from four vertices and six
  indices. `pipeline_create` is unchanged and still uses the built-in layout.

- **contrib/vulkan: push constants, uniform buffers and textures.** The
  pipeline layout was empty, so a shader could receive nothing but vertex
  attributes: no transform, no camera, no material, no texture. A pipeline now
  declares a push-constant block of up to 128 bytes, the minimum every device
  guarantees, plus uniform-buffer and combined-image-sampler bindings, and owns
  the descriptor set layout, pool and set for them. Textures are RGBA sampled
  images uploaded through a staging buffer with the layout transitions the
  driver requires. Uniform writes reuse their buffer and are host-coherent, so
  a per-frame update is a memcpy rather than an allocation or a descriptor
  rewrite.

  Both are verified by reading pixels back, not by trusting a status: a mat4
  pushed per draw mirrors the triangle, a 2x2 texture lands one texel per
  quadrant of the indexed quad, and a tint uniform scales what the sampler
  returned. The refusals are covered too, including binding a texture that was
  never given pixels, which would otherwise sample undefined contents. Zero
  leaks under `leaks --atExit`.

## [0.525.0]

### Fixed

- **A string returned through a heap-returning boundary leaked when the caller
  freed it.** An Aether `string` is either a refcounted AetherString or a plain
  malloc'd payload, and the runtime cannot free the second: given a bare
  `char*` it cannot tell a heap payload from static storage, so `string.free`
  no-ops and the value leaks. Every heap-returning function routes its returns
  through a uniform-heap helper whose cold half (a literal, or anything not
  already owned) minted exactly that bare buffer, so the two branches of one
  function returned two different shapes and the caller could only free one of
  them. `json.parse` leaked a block per call at its success-path error slot,
  and so did every literal returned the same way. The cold path now mints the
  same refcounted shape the sibling branch already returns, which costs the
  hot path nothing. `tests/leaks_known.txt` loses its
  `test_fs_read_error_detail` entry, which was filed for this and annotated to
  be dropped once fixed; that test reports zero rather than its allowed three,
  and `test_string_double_locale` goes from one leak to none.

  Freeing an interpolated string still leaks: interpolation has a different
  producer, and closing that half means separating "escaped because something
  else owns it" from "escaped because this call frees it" in the ownership
  analysis. Tracked separately with the measurements behind that conclusion. Registering `string_free` as non-storing
  in the codegen looks like the fix for both and is not, because it withdraws
  the escape marking that suppresses automatic frees, and
  `contrib/tinyweb/example_schema_api` then double-frees and aborts.

- **`or` took its handler on success when the error slot was a refcounted
  string.** "Is this an error" is "is the error slot non-empty", and the slot
  holds either physical string shape, but the check indexed the pointer raw, so
  a refcounted slot's magic header read as content and every such result looked
  like a failure. A function whose success path returns `string.concat("", "")`
  in the error slot, the uniform shape std/json documents, took the `or`
  handler on success. The `defer` success/error guard used the same convention
  and had the same flaw. Both now read the payload through
  `aether_string_data`, which handles either shape.

- **`unbox_closure()` on a value that was never boxed segfaulted with no
  diagnostic.** A bare function passed into a `ptr` slot stays a raw code
  address; unboxing it read `.env` out of the function's own machine code and
  jumped through it. Boxed closures now carry a tag after the `{fn, env}`
  prefix, and unbox panics with a message naming the cause and the fix. The tag
  goes after that prefix deliberately: it is the FFI layout std/collections and
  std/worker read, and a tag in front would turn their `fn` into the tag and
  call it.

- **`box_closure()` rejected a bare function**, which is the value most likely
  to need boxing and what the new panic message recommends. It now wraps one in
  the same env-ignoring adapter a ptr-typed field assignment already used.

- **The codegen `unresolved type` warning had no source location**, so it could
  not be traced or fixed from the source side, and no flag produced one. It now
  reports file, line, column and the enclosing function. The warning fires on
  in-tree examples today, and one of those turned out to be a real defect
  rather than noise, filed separately: an actor `?` ask never infers its reply
  type, so a non-int reply fails to build.

- **The cross-language skynet benchmark credited every implementation for
  actors it never created.** All eleven divided by the tree's full node count,
  1,111,111, while creating between 1,111 and 1,111,111 concurrency units
  depending on the language, so whichever implementation created the fewest
  scored highest. Go spawned 1,111,111 goroutines and reported 3.66 M msg/sec;
  C spawned 1,111 pthreads and reported 72.02; Aether spawned 11,111 actors and
  reported 225.05. Every implementation now goes sequential below a subtree of
  1000, so all create the same 1,111 units, each divides by the count it created,
  and each prints that count so the invariant is checkable.

  1,111,111 is not the number to equalise on: a pthread is not a goroutine, and
  this machine refuses the 4,096th concurrent pthread.

- **The sequential half of skynet was not equal either.** C, C++, Go, Rust and
  Zig summed a leaf subtree by recursing 10-ary to size 1; Erlang built a
  1000-element list per leaf, which cost it 2.7x. All ten now use a plain
  accumulator loop, so the non-concurrent operation count matches.

  Corrected results, median of five runs, 1,111 units each, every one returning
  the correct sum: Go 490 ns per unit, Aether 516, Erlang 990, Elixir 1,096,
  C 10,534, Zig 11,700, C++ 11,745, Rust 12,386, Java 13,968, Scala 38,322.
  Go and Aether are indistinguishable, their ranges overlap almost entirely, and
  the suite should not be read as showing Aether ahead of Go on this pattern.

- `zig/skynet.zig` printed `0.+4 M msg/sec` for any rate below 1 M/sec, which
  the runner's parser reads as garbage. It assembled the rate from an integer
  and a fraction; the other four Zig benchmarks already used a float and
  `{d:.2}`.

- The benchmark README claimed "All 11 languages implement all 5 patterns
  (55 total benchmarks, zero skips)". Pony has three: no `ping_pong`, no
  `skynet`. The real figure is 53.

### Added

- **`contrib`-free Scala benchmarks.** All five Scala implementations imported
  `akka.actor`, so the Scala column measured a third-party framework while the
  other ten measured standard libraries. They now use `java.util.concurrent`
  from Scala, bounded queues for the message patterns and `ForkJoinPool` for
  skynet, and `build.sbt` declares no dependencies. Dropping Akka made skynet
  8x faster, 38,322 ns per unit to 4,869.

- **`.gitignore` hid every Pony benchmark.** The compiled-binary patterns are
  `benchmarks/cross-language/*/<pattern>`, and Pony compiles a directory, so
  those patterns matched its source directories: a new Pony benchmark never
  appeared in `git status` and could not be committed. The three that exist
  predate the rule and survived only because tracked files ignore it. The
  directories are re-included and their contents ignored except `*.pony`.

- **Pony's two missing benchmarks.** `pony/` had three of the five patterns, so
  the README's "55 total, zero skips" was really 53 and the runner would have
  failed on the missing directories. `ping_pong` and `skynet` are written and
  building; Pony's skynet is the fastest of the eleven at 501 ns per unit.

- **Ping-pong timed different regions.** Aether, Erlang and Elixir created their
  units before starting the clock while the other eight started it first. All
  eleven now start the clock first.

- `benchmarks/cross-language/FAIRNESS.md`: the six rules the suite holds itself
  to, the audit that produced them, and how to check a change against them.

## [0.524.0]

### Fixed

- **CHANGELOG repair: a missing section, a corrupted heading, and two releases
  out of order.** Three separate defects, all from the release pipeline as it
  behaved before the #1477 guards landed in 0.515.0:

  - **`[current]` was wholly missing**, so there was nowhere for new entries to
    go and the next release would have had nothing to rename.
  - **0.514.0 had no section.** It shipped one PR (#1485) and is now recorded.
  - **The 0.515.0 heading was corrupted** into `## [0.515.0]'; then rename;`,
    with a dozen lines of the preceding paragraph duplicated beneath it. The
    cause is worth stating plainly: that entry's prose *quotes the pipeline's
    own* `if grep -q '## [current]'; then rename; fi` **logic**, and the release
    `sed` matched that literal `## [current]` **inside the prose** and rewrote
    it. The pipeline corrupted the one entry documenting its bug — a fourth
    failure mode in the #1477 family, and an argument for anchoring the rename
    to a line-start match rather than a substring.
  - **0.520.0 sat above 0.521.0**, breaking the descending order. An audit
    of every heading turned up one more of these, much older: 0.275.0 sat
    above 0.276.0. Both are fixed.

  **The corrupting `sed` is fixed, not just its damage.** The #1477 guard
  greps for `^## \[current\]`, correctly anchored — but the rewrite beneath it
  was `s/## \[current\]/## [VERSION]/`, neither anchored nor limited to the
  first match, so it rewrote every occurrence anywhere in the file. It is now
  `0,/^## \[current\]$/s//## [VERSION]/`. Verified both ways against this very
  file: the old form rewrites three prose mentions and re-corrupts the entry
  you are reading, the new one renames the heading alone.

  Releases 0.516.0 through 0.523.0 were audited against the merge log and are
  complete, so the guards added in 0.515.0 are holding. The duplicate
  `## [0.435.0]` and `## [0.497.0]` headings are left alone: they are noted in
  #1477 as evidence and rewriting history to hide them would cost more than it
  is worth.

## [0.523.0]

### Fixed

- **`tools/ae_cross.c` emitted two warnings on Linux/GCC during a source
  install.** `(void)system(rmcmd)` does not suppress glibc's
  `warn_unused_result` on GCC the way it does on clang, so the temp-object
  cleanup warned on every build; the status is now checked and a failure
  reported. Separately, `snprintf(base, bsz, "%s/share/aether", tc.root)`
  could compose up to 1037 bytes into a 1024-byte buffer, since `tc.root` is
  itself `char[1024]`, which GCC flagged as `-Wformat-truncation`. The root is
  now bounded with a precision specifier and truncation is treated as a
  lookup failure rather than silently producing a wrong base path. Verified
  under gcc 14 with `-D_FORTIFY_SOURCE=2 -Wall -Wextra`, both warnings gone.

- **`test_worker` flaked on Windows CI** with "detached worker drained pending".
  The test waited for the pool thread by counting iterations rather than time,
  and hot-spun on `worker.drain(0)` while it counted. Two million mutex-takes
  elapse in a fraction of a second, so on a two-core runner the cap could expire
  before the pool thread was ever scheduled, and the spinning was itself part of
  why it was not scheduled. The waits are now bounded by wall-clock time and
  sleep between polls, so the test stops competing with the thread it is waiting
  for. The comment above the loop already described this flake; it is now fixed
  rather than described.

### Documentation

- Stopped defining the language by other languages. The README led with
  "Erlang-style actors, Rust-grade capability discipline, and Go-flavored
  ergonomics", and the language reference opened with "combining Erlang-inspired
  actor concurrency". Both now state what Aether does. The same substitution ran
  through the entry docs: "Erlang-inspired pattern matching" said nothing a
  reader could act on, and `(value, err)` describes its own shape without being
  called Go-style. The Acknowledgments section already credits the lineage
  properly, at the end, which is where it belongs; the top of a README is for
  what the thing is.

## [0.522.0]

### Fixed

- **`string.char_at` reported bytes above 0x7F as negative** (#1516). It
  returned a signed `char`, so 0x85 came back as -123 and every binary parser
  built on it broke on high bytes. `contrib/tinyweb` read a masked WebSocket
  header, computed a negative payload length, and dropped the connection
  instead of echoing the frame. The C definition also disagreed with the
  prototype codegen emits and with the stdlib extern, both of which say `int`;
  only the implementation said `char`. `char_at` and `char_at_n` now return the
  byte as 0..255, which is what `bytes.get` has always documented.

- **`contrib/tinyweb` would not build** (#1516). `tw_start` had no declared
  return type and returned `-1` on one path and `http.server_start`'s string on
  another, so the emitted C assigned an int to a `const char*` and every
  tinyweb example and test failed to compile. It now declares `-> string` and
  returns `""` on success, matching `http.server_start` and the stdlib's error
  convention. The examples compared that result against `0`, which asked
  whether the pointer was NULL and so never took the success branch; they now
  compare against `""`.

### Documentation

- Audited the docs against the code. `docs/stdlib-reference.md` opened with
  "Complete reference for Aether's standard library modules" while 37 of the 69
  shipped modules appeared in neither it nor `docs/stdlib-api.md`. It now opens
  with a generated index covering every module, its purpose and its export
  count, taken from the source so it cannot drift.

- Corrected API references that no longer matched the code: `http.server_listen`
  (the function is `http.server_start`), and `cryptography.base64_encode` /
  `base64_decode`, which live in `std.encoding` and return `string` and
  `string!` rather than the tuples the docs described. `std.cryptography`'s own
  header has said "use encoding.base64_*" for some time.

- Fixed two documented snippets that could not compile: the `c-interop.md`
  SQLite extern used `callback` as a parameter name, which is a reserved word,
  and the `02-functions-and-control-flow` tutorial taught
  `if (is_prime(num))` against an int-returning function, which the type checker
  rejects. The tutorial now uses `-> bool`.

- Repaired every broken relative link outside the changelog archive: the docker
  guide pointed at two `docs/setup/` pages that were never in the tree, the
  benchmark README missed the `design/` path segment, and `contrib/host/TODO.md`
  pointed at a roadmap document that does not exist. The archive's dead
  `contrib/aether_ui/` paths are left as written, with a note explaining that
  the UI library moved to a sibling repository.

- README: the CI suite is 10 steps, not 9, and the `ae` command list was missing
  `inspect`, `bindgen`, `cflags` and `lib-path`.

## [0.521.0]

### Added

- **`contrib.vulkan`: offscreen GPU rendering** (#1495, phase 1). Instance,
  physical-device selection, logical device and queue; an offscreen RGBA target
  with its render pass and framebuffer; a graphics pipeline built from
  caller-supplied SPIR-V; vertex upload, draw, and readback. Plus an example
  that renders a triangle and writes a PPM.

  Nothing links against libvulkan. The loader is opened with `dlopen` at
  runtime, so a program using the module builds and starts on a machine with no
  driver at all and `vulkan.available()` reports 0. Only the Vulkan headers are
  needed to build, and they are header-only. Device entry points come from
  `vkGetDeviceProcAddr`, which returns the driver's own function rather than the
  loader's trampoline; measured at 11.6 ns per call against 12.2 ns through the
  loader.

  On an M1 Pro through MoltenVK at 512x512: 0.350 ms per draw including the GPU
  fence wait, and 0.025 ms to read back 1 MiB, which is host memcpy bandwidth
  because the readback buffer stays mapped for the target's lifetime. The
  command buffer is recorded once and resubmitted, re-recording only when the
  pipeline, vertex count or clear colour changes. Vertices are written straight
  into mapped GPU-visible memory rather than through a host array.

  The test covers the failure modes as well as the happy path: zero and negative
  sizes, a size past `maxImageDimension2D`, empty SPIR-V, a length that is not a
  multiple of 4, bytes that are not SPIR-V, out-of-range vertex indices, writes
  before a reserve, out-of-image pixel reads, a null readback destination,
  destroying null handles, and eight create/draw/destroy cycles. It skips
  cleanly where no driver is installed. `leaks -atExit` reports 0 leaks.

  `tests/integration/contrib_vulkan_portability` cross-compiles the module for
  Windows and asserts the object reaches `LoadLibraryA` rather than `dlopen`.
  contrib-check runs on Linux only, so without it the `_WIN32` branch would be
  shipped unbuilt by any leg.

  The Linux contrib CI leg now installs lavapipe, Mesa's CPU implementation, so
  the GPU path is exercised on a runner with no GPU, and then asserts the test
  did not skip: a driver is installed on that leg, so a skip would be silent
  loss of coverage rather than a pass.

  Surfaces, swapchains and window presentation are phase 2 and deliberately
  absent, along with the aether-ui seam that goes with them.

### Changed

- `contrib_check.sh` can now express a dependency the test needs only the
  HEADERS of, separately from one it must link against. `contrib/vulkan` needs
  the former: linking the Vulkan loader would reintroduce exactly the hard
  dependency its runtime `dlopen` exists to avoid.

## [0.520.0]

### Fixed

- **An idle program pegged every core** (#1517). The scheduler's extended-idle
  branch called `scheduler_io_poll(sched, 1)` expecting it to block for a
  millisecond. That function returns immediately when no descriptors are
  registered, and the `aether_sched_yield()` after it also returns immediately
  when nothing else is runnable, so the thread reset its counter to 5000 and
  spun again. Nothing in the path ever slept. The report that opened the issue
  was a leftover binary that had been running for five days at 650% CPU doing
  no work.

  A core with nothing registered with the poller now parks on a condition
  variable. Producers signal it when they hand it work, so a message still
  arrives without waiting out a timeout; the wait is timed as well, so a
  producer that never signals costs latency rather than a hang. Cores that do
  have descriptors registered are unchanged: the poll blocks for its timeout
  and always did.

  Measured on an 8-core M1 Pro, a program that does one thing and then idles
  for two seconds:

  | | CPU consumed |
  |---|---:|
  | before | 14,058,539 us (about 7 cores) |
  | after | 5,661 us |

  Throughput improves rather than regresses, because idle cores are no longer
  competing with working ones for the machine. Medians over repeated runs:
  thread_ring +18%, counting +187%, fork-join +17%, ping-pong within noise at
  +2.5%. `tests/regression/test_scheduler_idle_cpu.ae` compares process CPU
  time against wall time while quiescent and fails at 35x its budget against
  the old scheduler.

## [0.519.0]

### Fixed

- **Released installs compiled only 40 of the 95 runtime sources when building
  without `libaether.a`.** Three separate faults lined up:

  `release.yml` builds the release tree by hand and never copied
  `build/MANIFEST`, the list of link-suitable `.c` files, so no released
  install has ever had it. `ae` responded by silently substituting a
  hand-written list kept in `tools/ae.c`, which had drifted to 40 entries and
  was missing `std/bytes` among others. And even where MANIFEST was present,
  the list was assembled into a fixed 8 KB buffer: 95 absolute paths need
  6.0 KB under a 32-character prefix and 9.7 KB under a 73-character one, and
  on overflow the same silent substitution kicked in.

  The visible symptom was a link failure with no connection to its cause, most
  often from `ae build --trace`, which compiles the runtime from source by
  design:

  ```
  Undefined symbols: _aether_bytes_data, referenced from _fs_pread_into_raw
  ```

  MANIFEST is now packaged by all three release jobs and by the release-archive
  smoke test, which also asserts it is present. The source list grows on demand
  through the same helper the include list already used, so path length cannot
  truncate it. The hand-written list is deleted: a MANIFEST that cannot be read
  is now a clear error naming the file and what to do, instead of a quietly
  degraded build.

  `tests/integration/manifest_srcs_long_path` builds through a symlink deep
  enough to have overflowed the old buffer and fails if a hand-written source
  list reappears in `tools/ae.c`.

- **Four stdlib modules documented usage that does not compile.** `std.xml`,
  `std.arena` and `std.snapshot` showed `loop { ... }` in their header
  examples, but `loop` is not an Aether keyword, so copying one gave
  `error[E0300]: Undefined variable 'loop'`. They now use `while 1 == 1 {`.
  `std.http1`'s example called `read_response(resp, recv_next, conn)`, which
  does not exist; the function is `read_response_conn(rp, conn)`, and the
  prose above it stated a third signature again. Both mentions now match the
  implementation. Comments only, no behaviour change.
## [0.518.0]

### Fixed

- **The shipped Windows archive could not be linked by half of Windows**
  (#1494). A user on msys2 ucrt64 could not compile any program at all, with
  the undefined references inside the `libaether.a` we ship:

  ```
  libaether.a(aether_locale_num.o): undefined reference to `__imp__snprintf_l'
  ```

  Our release builds that archive in MINGW64, which links the legacy
  msvcrt.dll and exports the `_l`-suffixed printf family. The UCRT does not,
  and UCRT64 is the current MSYS2 default, so the archive was fine on the
  machine that built it and unlinkable on the default toolchain. CI stayed
  green because it both built and linked with the same CRT.

  The Windows float formatter no longer uses that family. It asks
  `localeconv()` for the radix character the formatter is about to use and
  brackets a per-thread locale switch only when that character is not `.`, so
  a locale that already formats with `.` pays nothing and every symbol on the
  path is exported by both CRTs. The `_scprintf` truncation recovery went with
  them, since mingw's `__mingw_snprintf` and the UCRT's `snprintf` both return
  the C99 would-be length directly.

  Three things now prevent a repeat. Windows CI is a matrix over MINGW64 and
  UCRT64, so the reporter's environment builds and runs the full suite on
  every pull request. `tests/integration/windows_crt_symbols` bans the
  `_l`-suffixed printf family across `runtime/` and `std/` with no toolchain
  needed, and cross-compiles every source in `build/MANIFEST` to require each
  external symbol in both `libucrt.a` and `libmsvcrt.a`. And the locale
  regression test now asserts the restore by reading the radix character
  through `localeconv`, which is thread-locale aware; the previous check went
  through `std.number`, which takes an explicit locale and kept reporting
  correct output from a thread stranded in "C".

- Untracked `cov_demo.gcda` and `cov_demo.gcno`. Two gcov output files had
  been committed by accident and the suite rewrote them, so unrelated pull
  requests picked up a spurious binary diff. `*.gcda`, `*.gcno` and `*.gcov`
  are now ignored.

## [0.517.0]

### Fixed

- **The built toolchain reported the wrong version.** `v0.516.0` shipped an
  `ae --version` that said `0.417.0`. The binary compiled current sources
  correctly — it was the right toolchain, mislabelling itself — but anything
  keying off the banner recorded the wrong number, including aeb's
  content-addressed cache key.

  Two independent causes, both fixed:

  1. **The Makefile preferred the highest `git tag` over the `VERSION` file.**
     Tag visibility depends on clone depth, and `release.yml`'s build jobs
     check out shallow, so the visible set could be stale and `tail -1` land on
     an *older* tag — overriding a correct `VERSION`. The tree a tag points at
     always carries the right number (`git show v0.516.0:VERSION` → `0.516.0`),
     so the tree is now authoritative and tags are consulted only for an
     untagged dev checkout. `fetch-depth: 0` added to the `build` and
     `build-freebsd` jobs as defence in depth.

  2. **A `VERSION` bump did not rebuild the objects that bake it in.** The
     version arrives as `-DAETHER_VERSION` on the command line rather than
     through an `#include`, so `-MMD` never saw it and every object stayed
     "up to date" — leaving a binary that reported the previous version. The
     version-bearing objects now depend on the generated `aether_version.h`,
     which is already regenerated whenever `VERSION` changes.

  New `tests/integration/version_stamp/` asserts `ae --version` matches the
  `VERSION` file, so a mislabelled build fails CI rather than shipping.

## [0.516.0]

### Fixed

- **A leading-underscore function name no longer collides with the C runtime.**
  `_write(path, content)` was emitted as a C function of the same name
  verbatim, landing in the namespace C11 §7.1.3 reserves for the
  implementation — which MSVCRT/UCRT populate heavily (`_write`, `_read`,
  `_open`, `_close`, `_access`, …). On Windows the build then failed with
  `conflicting types for '_write'`, pointing at generated C rather than at the
  function name; on Linux the identical program built clean, because glibc
  declares none of them.

  Codegen now renames such a function to an `ae`-prefixed symbol (`_write` →
  `ae_write`) and rewrites its call sites — the same mechanism the #1366 extern
  collision already uses. The Aether-level name is unchanged, so this is
  invisible to callers.

  `static` alone would not have fixed it: a file-scope static whose name
  matches a declared CRT prototype is still a conflicting-types error at
  compile time. The trailing-underscore file-local convention (#279) is
  untouched.

  Reported from the aeb line, where one shared `_write` test fixture broke 10
  of 118 tests on Windows only.

## [0.515.0]

### Changed

- **The release pipeline fails instead of shipping an unrecorded release**
  (#1477). The CHANGELOG rename was `if grep -q '## [current]'; then rename;
  fi`, which does exactly what it says and silently ships a release with no
  section when the condition is false. Releases 0.506.0 through 0.509.0 went
  out that way, four releases of real work recorded nowhere, including a
  Windows packaging fix. Three guards: the release now fails when there is no
  `[current]` to rename, fails rather than creating a duplicate heading (which
  is how `## [0.435.0]` and `## [0.497.0]` each came to appear twice), and a CI
  job requires a CHANGELOG entry on any PR touching shipped code, which is what
  makes a `[current]` section exist to be renamed in the first place. All three
  have an explicit `[skip changelog]` opt-out, because a genuinely empty
  release should have to say so rather than be indistinguishable from an
  oversight.

## [0.514.0]

### Fixed

- **The CachyOS nightly could report a green run whose avcodec test never
  executed.** `contrib/avcodec` landed in 0.509.0 and its audio half in
  0.513.0, and the contrib-check entry SKIPs when pkg-config cannot find the
  FFmpeg libraries — so with no entry in the dependency gate, an absent FFmpeg
  was a silent skip rather than the provisioning failure the gate exists to
  surface. Both `libavcodec` and `libswresample` are now required, since
  0.513.0 made all five FFmpeg libraries a set.

  The gate's `lib` probe also now searches `/usr/lib/<arch>-linux-gnu`. On
  Debian and Ubuntu that is the only place these libraries live, so a
  Debian-shaped box reported *every* `lib` dependency missing; Arch has no
  multiarch directory, so the extra path is a no-op on the box the nightly
  actually runs on.

## [0.513.0]

### Added

- **`avcodec.audio_pcm(url)`** — decode a file's whole audio track to PCM, the
  producer side of the `load_pcm` consumer that landed in 0.512.0. Together
  they close the loop `asks/pcm-please.md` described: an MP4's audio can now
  reach the speakers without a hand-extracted sidecar WAV.

  Returns `(pcm, n, rate, channels, err)` — interleaved **s16 stereo at the
  source rate**, in one shot. libswresample does the conversion in the same
  pass, so a 5.1 float-planar AAC track (Big Buck Bunny's, for instance) comes
  back as plain stereo s16 without the caller arranging anything. The
  whole-buffer shape deliberately matches `audio.load_pcm`'s, so the two
  compose directly:

  ```aether
  pcm, n, rate, ch, err = avcodec.audio_pcm(path)
  src, e = audio.load_pcm(pcm, n, rate, ch, audio.FORMAT_S16)
  ```

  Verified end to end: a 117.3s clip decodes to 22,523,904 bytes at 48 kHz
  stereo — exactly 117.3s — and `audio.load_pcm` then reports
  `duration_ms=117312` with `position_ms` advancing in real time.

  `contrib/avcodec` now requires **libswresample** alongside the other four
  FFmpeg libraries. All five are required together; a partial install stays a
  clean SKIP rather than a build failure.

## [0.512.0]

### Added

- **`audio.load_pcm(data, length, sample_rate, channels, format)`** — play PCM
  samples the caller has **already decoded**, rather than only encoded
  containers miniaudio can demux itself (`asks/pcm-please.md`).

  `load_wav` is better than its name — it is a format-sniffing decoder, so mp3
  and flac already work — but every entry point wanted bytes miniaudio could
  parse. That left no way in for samples a *different* decoder produced, which
  is exactly the case once `contrib.avcodec` has demuxed an MP4 and holds the
  audio packets. The workaround was pre-extracting a sidecar WAV: roughly
  doubling on-disk cost (a 20 MB sidecar for a 21 MB clip), a manual step
  before playback, and no option at all for a live source with no file to
  extract from — the same intermediate-file problem `contrib/avcodec` was
  written to remove, reappearing on the audio side.

  Backed by `ma_audio_buffer` fed to the same `ma_sound_init_from_data_source`
  the encoded path uses, so the whole transport surface works unchanged: `play`,
  `pause`, `position_ms`, `duration_ms`, `seek_ms`, `volume`. `position_ms`
  keeps working as the A/V-sync master clock, which is the reason the ask
  matters. Sample formats are exposed as `audio.FORMAT_U8` / `_S16` / `_S24` /
  `_S32` / `_F32` so callers never hardcode miniaudio's numbering.

  `length` must be a whole number of frames (bytes-per-sample x channels); a
  partial trailing frame is refused rather than played as noise off the end.

## [0.510.0]

### Added

- **`io.fd_read_into(fd, buf, length)`** — zero-allocation incremental read
  from a raw file descriptor (#1471). Mirrors `fs.pread_into`, which was
  already the same shape for files, so a caller streaming from a pipe now gets
  the same option as one reading a file.

  Returns `(n, err)`: `n == 0` is EOF, `0 < n < length` is a normal short read.
  Unlike `io.fd_read_n` — which loops until it has filled the buffer — this
  returns as soon as *any* bytes are available, which is what makes it usable
  on a live pipe where waiting to fill would stall until the producer happened
  to send a whole buffer's worth.

  Motivated by streaming video decode: reading one frame per iteration, an
  8 MB frame at 1080p30 means ~250 MB/s of allocator churn if every read mints
  a fresh string. One caller-owned buffer reused across the loop avoids it
  entirely.

  Note it writes straight into `bytes.data(buf)`, which does not publish the
  buffer's length — call `bytes.set_length(buf, n)` before
  `bytes.to_string(buf, n)` or the conversion returns an empty string with no
  error. Callers passing the bytes to another extern never need this.

## [0.509.0]

### Added

- **`contrib/avcodec`** — in-process video decode, so a caller no longer needs
  an intermediate file. A thin FFmpeg veneer following `contrib/sqlite`: C shim
  plus `module.ae`, a catalogue entry with a pkg-config probe, and nothing
  vendored — user programs link
  `-laether_avcodec -lavcodec -lavformat -lavutil -lswscale` via `aether.toml`.
  `make contrib` builds it where FFmpeg's dev libraries are present and SKIPs
  cleanly where they are not.

  Motivated by aether-ui, which was spawning `ffmpeg` to transcode a whole clip
  to raw RGBA on disk and reading frames back with `fs.pread`: 27.6 MB for six
  seconds of 320x240, roughly 1.5 GB per minute of 1080p, and no workaround at
  all for a *live* source such as a camera or network stream, where there is no
  file to read. That intermediate is gone entirely; 300 frames of 640x480
  decode in 0.426s including compile time.

  The surface is deliberately narrow — open, take the next frame as packed
  RGBA8888, close. Video only: audio, seeking and stream selection are future
  work and none are needed to feed a renderer. Two ways to take a frame,
  mirroring sqlite's blob accessors: `next_frame` allocates a fresh owned
  string, while `next_frame_into` writes into a caller-owned buffer and
  allocates nothing per frame — at 1080p30 that is 250 MB/s of copying avoided.
  `fps()` returns a *ratio* rather than a float, because 30000/1001 does not
  survive a float round-trip and a presentation-timestamp model needs the exact
  value.

### Fixed

- **`make contrib-check` could not run any test backed by a system library** —
  the runner builds each contrib test with `ae build --extra <shim.c>`, which
  compiles the shim but has no way to pass `-l` flags, so such a test compiled
  and then died at link with `undefined reference to avcodec_receive_frame` on
  a box with every FFmpeg dev library installed. The gate was wired in but
  structurally incapable of running. The test table gained an optional fifth
  column naming the pkg-config modules an entry must link against; when set,
  the runner stages an `aether.toml` workspace (the same shape
  `tests/integration/sqlite_roundtrip` uses) so the flags reach gcc via
  `get_link_flags()`, and SKIPs the entry when pkg-config cannot find them — an
  absent system library is a provisioning gap, not a test failure. Contrib
  modules with native dependencies are now genuinely gated rather than
  nominally.

## [0.508.0]

### Fixed

- **`fs.read` / `fs.read_binary` now name the path and the cause** — both
  collapsed every failure into a bare constant (`"cannot read file"`,
  `"cannot open file"`) with no path and no errno, so six distinct causes —
  including sandbox denial and silent truncation — were indistinguishable. No
  program could treat a missing optional file as benign while treating a
  permission error as fatal. Reported from the aeb line, where a 79-target
  parallel build reported `cannot read file` with no path, making an
  intermittent content-hash failure undiagnosable.

  Failures now read `"/tmp/x: No such file or directory"`, `"/tmp: Is a
  directory"`, `"/etc/shadow: Permission denied"`. A sandbox refusal gets its
  own wording — `"blocked by sandbox policy (no fs_read grant for this path)"`
  — because it is a policy decision, not a filesystem failure, and reporting it
  as one sends whoever is debugging a grant list looking at the disk. The
  `(bytes, length, err)` arity is unchanged, so existing callers keep working.
  The error string is borrowed from thread-local storage and is valid until
  that thread's next failed read; copy it with `string.concat(err, "")` to hold
  it longer.

- **`fs.read` on a directory reported success with empty content** — the
  seekable fast path in `file_read_all_raw` never checked `ferror`, so a failed
  read returned an empty string *as success*. Reading a directory is the
  everyday trigger: `fopen("/tmp","r")` succeeds on Linux and `ftell` reports a
  positive size, so control lands there and `fread` fails with `EISDIR`. Same
  class as the #1116 silent-truncation bug, which fixed only the streaming
  branch. A genuinely short read (the file shrank mid-read) still returns what
  was read.

## [0.507.0]

### Fixed

- **Windows `make install` dropped the `.exe` suffix, leaving a toolchain that
  could not compile** — the install read the built artifacts *with* the
  extension and wrote them *without* it, but `ae` looks for its compiler as
  `aetherc.exe` next to itself. So `ae` could not find `aetherc` even though
  the two sat in the same directory. `ae --version` kept reporting a healthy,
  correctly-versioned toolchain throughout, because that path never needs the
  compiler — the failure surfaced downstream as 57 aether-ui spec suites red
  with 0 pass / 0 fail each, which reads as a harness fault rather than a
  packaging one, and the accompanying `set AETHER_HOME=...` advice is a red
  herring since the problem is a filename, not a search root.

  Fixed by installing with `$(EXE_EXT)` on both sides (a no-op on POSIX), and
  `tools/ae.c`'s `/usr/local/bin` fallback now appends it too. `make install`
  additionally **compiles a test program** with the installed `ae` rather than
  only running `ae version` — the check that could not catch this, since it
  never touches the compiler.

## [0.506.0]

### Fixed

- **The CachyOS nightly built and tested a stale toolchain** — two causes, both
  presenting as "the newer compiler on that box is broken" and neither being
  that. First, a few tests link the *installed* artifacts rather than the build
  tree, and the install had drifted three weeks behind, failing with
  `undefined reference to aether_unwind_forget` — the nightly log showed only
  `gcc --coverage link failed` without the linker's line, which reads like a
  GCC 16 problem. `make install PREFIX=$HOME/.local` now runs as a pipeline
  step (never sudo) with a freshness check, so drift is impossible by
  construction. Second, the Makefile takes the highest **git tag** as the
  authoritative version and falls back to the `VERSION` file only for tarballs,
  so fetching commits without tags left the tree at current HEAD but the
  previous tag and the build correctly stamped the *older* version; the sync is
  now `git fetch --tags --prune`.

  Note for anyone reading versions by hand on that box: `ae --version` reports
  the version *manager's* active install (`~/.aether/active_version`), not the
  version compiled into the binary, so a correctly built 0.509.0 binary can
  still say 0.417.0. Both the sweep guard and the install check use `strings`
  against the compiled-in value for this reason.

## [0.505.0]

### Fixed

- **HTTP access log no longer emits a locale-dependent month name** — the
  Combined Log Format timestamp was built with `strftime`'s `%b`, which renders
  the *locale's* abbreviated month. CLF is a parseable interchange format
  (GoAccess, AWStats, Logstash, Splunk) whose month is defined as the English
  three-letter abbreviation, so a server embedded in a host that had called
  `setlocale(LC_ALL, "")` wrote `08/Mär/2026` and broke every downstream parser
  — corrupting archived logs, where nothing round-trips to reveal the damage.
  Now formatted from a static English table, which removes the locale
  dependency rather than pinning a locale around the call. The three remaining
  `strftime` sites are numeric-only and carry a comment saying so. Follow-up to
  the `LC_NUMERIC` float fix; found by the audit in #1463.

- **Undefined behaviour in eleven `ctype` calls** — `isalpha`/`isdigit`/
  `tolower` and friends take an `int` that must be `EOF` or representable as
  `unsigned char`. Plain `char` is signed on every platform Aether ships, so any
  byte ≥ 0x80 — every UTF-8 continuation byte — arrived negative and indexed
  outside the ctype table. Reachable today: a non-ASCII identifier reaches
  `isalpha()` with a negative argument in the lexer, and `levenshtein_distance`
  runs over arbitrary identifier text for "did you mean?" suggestions. The
  codebase already cast at 24 of 35 sites; the remaining eleven (nine in
  `compiler/parser/lexer.c`, two in `compiler/aether_diagnostics.c`) now match.
  No behaviour change — glibc happened to tolerate it; musl and the MSVC CRT are
  entitled not to.

- **Float text conversion no longer follows `LC_NUMERIC`** — `std.string` and
  `std.json` produce and consume *machine* text (wire formats, JSON, config,
  round-trips), so their decimal separator must be `.` regardless of the
  ambient locale. The parse side had no locale handling at all: under a
  comma-decimal locale such as `de_DE.UTF-8`, `strtod("3.14")` stops at the
  `.`, so `string.to_double("3.14")` **rejected valid input** and every
  fractional number in a JSON document failed to parse — breaking the
  round-trip guarantee added in #1429 and emitting/rejecting JSON that
  violates RFC 8259. The emit side reformatted after the fact instead of
  controlling the conversion.

  All eight conversion sites across `std/string`, `std/json` and
  `runtime/aether_runtime_types.c` now route through a new
  `runtime/aether_locale_num.{h,c}`, which pins each call to the C locale via
  the `_l`-suffixed libc functions where available (macOS, BSD, Windows) and a
  thread-local `uselocale` bracket on glibc — never `setlocale`, which is
  process-global and would race Aether's own actor, scheduler, worker and HTTP
  threads as well as disturb an embedding host. `atof` in the runtime is
  replaced by the locale-pinned `strtod`. Platforms with no locale machinery
  (WASM, ARM newlib) degrade to a plain passthrough, which is correct there
  because `LC_NUMERIC` is structurally `C`.

  This surfaced in external review of #1429. Human-facing number formatting is
  unaffected — that is `std.number`'s job (#863 Phase 4) and correctly takes an
  explicit locale.

  New regression test `tests/regression/test_string_double_locale.ae` applies a
  comma-decimal locale in-process and checks both directions plus a JSON
  round-trip; it SKIPs visibly where no such locale is installed, and the
  CachyOS nightly's dependency gate now requires `de_DE.UTF-8` so it really
  runs somewhere.

## [0.504.0]

### Added

- **`std.cryptography.blake3`** — BLAKE3 cryptographic hash in pure Aether,
  ported from Bouncy Castle's `Blake3Digest.cs` (no OpenSSL/externs beyond
  libc `malloc`/`free`, matching the sibling digest modules). Supports the
  plain, keyed (`new_keyed`), and key-derivation (`new_derived`) modes;
  streaming `update`/`update_bytes`; arbitrary-length XOF output (`output`);
  and `final_hex`/`final_bytes` one-shots. Verified against the official BLAKE3
  known-answer test vectors (all 35 input lengths from 0 to 102400 bytes, ×
  hash/keyed/derived, plus save/reset/restore mid-output checks — the full
  Bouncy Castle `Blake3Test` suite). Leak-free under Valgrind.

## [0.503.0]

### Added

- **`contrib/i18n/collate`** — locale-aware Unicode collation (#863 Phase 5).
  Orders strings by the Unicode Collation Algorithm (UTS #10) over the Default
  Unicode Collation Element Table (DUCET, UCA 15.1.0) with canonical (NFD)
  normalization via a vendored utf8proc 2.9.0, so accents and case tie-break
  correctly (`cafe` < `café`, `apple` < `Apple`) and canonically-equivalent
  strings compare equal — rather than raw byte order. API: `compare`,
  `sort_key` (order-preserving precomputed key), and in-place `sort`. This is
  DUCET (language-neutral) collation with full three-level weighting; per-locale
  tailoring is future work (the `locale` argument is reserved for it). Lives in
  `contrib/` because it carries vendored Unicode data tables (utf8proc: MIT;
  DUCET/`allkeys.txt`: Unicode license — see `contrib/i18n/ducet/NOTICE`).
  Verified leak-free under Valgrind; gated by a dedicated `ci-contrib-i18n` job.
- **`std.number`** — locale-aware number, percent, and currency formatting
  (#863 Phase 4). Formats `float` values (and exact decimal strings) per a BCP 47
  locale: decimal-point exponent shifting, min/max fraction digits with
  round-half-up (carrying into the integer part, e.g. `9.999 → 10.00`), digit
  grouping with locale separators (including the fr-FR narrow no-break space),
  percent scaling with locale spacing, and currency symbol/fraction-digit
  resolution with regional fallbacks (base-tag then `en`). Non-finite floats
  (`NaN`, `±Inf`) are wrapped rather than crashing, and malformed tags fall back
  without a crash. Public API: `format_decimal` / `format_percent` /
  `format_currency` (plus `*_default` and `*_string` variants) and a
  `FormatOptions` record. Verified leak-free under Valgrind.

## [0.499.0]

### Added

- **`std.schema`** — declarative, typed data validation and coercion. A
  `record { field(name, TYPE) { rules } }` builder describes typed fields with
  composable validators (`min`/`max`/`len`/`present`/`optional`/`one_of`/
  `email`/`refine`); `parse(schema, input) -> (values, errors)` turns an untyped
  `string→string` map into coerced typed values or a list of `{field, code,
  message}` errors with Zod-style issue codes. HTTP-agnostic (depends only on
  `std.map`/`list`/`string`), so the same declaration validates a request body,
  a config file, CLI args, or any untyped input. Includes a README, a runnable
  showcase (`std/schema/example.ae`), and a leak-clean regression test.
  Inspired by Zod, Pydantic, io-ts, and Ash (all MIT; credited in-module).
- **`contrib/tinyweb/schema_api`** — declarative JSON APIs backed by
  `std.schema`. `json_api(prefix, schema) { create/index/show/update/destroy }`
  mounts a tinyweb path plus a validating filter: write requests are JSON-parsed
  and validated before the handler runs; invalid bodies get a JSON:API `422`
  with per-field errors, malformed JSON a `400`. The Ash-style "declarative
  resource" idea resting on tinyweb (router) + std.schema (validator).

### Fixed

- **`contrib/tinyweb` DSL serving path** was non-functional and is now runnable.
  Three pre-existing breakages in `tw_start`: `tcp.listen` and `server_bind` had
  drifted to tuple/string returns the caller still compared against `int`
  (the latter a segfault via `aether_string_data`); and the builder stored
  handlers as boxed closures with no trampoline to invoke them (registering an
  unboxed `_AeClosure` value as a C `@c_callback` → segfault), with filters
  never dispatched at all. Added a `@c_callback dsl_dispatch` trampoline that
  runs the matched filter chain (honoring `STOP`) then the endpoint handler, so
  every builder-DSL route and its filters now work.

## [0.497.0]

### Fixed

- **The self-updater pointed at a repository path the project no longer owns.**
  `AE_GITHUB_REPO` was still the pre-rename `nicolasmd87/aether`, which the
  GitHub API answers with a 301. It kept working only because `curl -fsSL` and
  `wget` follow redirects silently, and `ae upgrade` / `ae install` /
  `ae version list` download release binaries from that path and install them.
  A rename redirect lasts exactly as long as nobody claims the freed name: the
  moment someone does, the redirect stops resolving here and the self-updater
  installs whatever is at the old address. Repointed to
  `aether-lang-dev/aether`, which resolves directly with no redirect, along
  with the twelve other stale references (the download hint in `ae`, the
  diagnostics wiki URL, `apkg`, the VS Code extension manifest and README, the
  benchmark UI). The historical changelog archive is deliberately left alone:
  those entries record the links as they were, and rewriting them would be
  fiction rather than a fix. `tests/integration/canonical_repo_url` fails the
  build if the stale path returns.


- **The cross-language benchmark suite excluded Aether, and said so quietly.**
  Every run printed `Aether... [SKIP] Build failed` for all five benchmarks
  while the other ten languages reported numbers, and then published a results
  file anyway. Two separate faults. First, `benchmarks/cross-language/aether/`
  compiles the runtime with its own hand-written `-I` list, which did not have
  `include/` on it, so `runtime/libaether_caps.c` could not find the public
  header it now includes by name (#1420). That Makefile already reads the
  MANIFEST for its source list, after a hand-maintained copy of that went stale
  with this identical symptom; the include set is now taken from `ae cflags`
  for the same reason, so neither list can drift again. Second, the runner
  treated an Aether build failure as a skip. That is right for a third-party
  toolchain that may not be installed and wrong for Aether in its own suite: a
  results file missing the language the suite exists to measure still reads as
  a complete comparison. It is now a hard error with the reproduction command,
  and the runner exits non-zero. Aether builds and reports again (15.5M
  msg/sec on ping_pong, the fastest entry).

### Changed

- **Flint survey leftovers decided and recorded** (#1096). The issue kept two
  ideas open and asked for a decision, not a plan; both are declined, with the
  reasoning written into `docs/cross-references/flint.md` rather than left in a
  thread. The substantive finding: the C-header interop candidate lost most of
  its motivation to the C-interop batch (#1239-#1244), which removed the
  extern-disagrees-with-header and layout-drift problems by deferring to the
  header (`@c_import`, `@c_verify`) instead of parsing it, and `ae bindgen
  consts` had already taken the constants slice. What remains is bulk
  convenience, at month scale. The in-grammar `test` block candidate is
  declined as a stylistic gain that would add a second in-tree test story
  alongside 321 working regression tests.

- **Fork-join over `std.worker` measured, with the result recorded** (#1297).
  The issue proposed building a `parallel_map`; `worker.map` had shipped in the
  meantime, so the open question was whether the layer earns its place and what
  is still missing. On 8 cores it reaches 5.1x on CPU-bound work and costs
  nothing over a hand-written `worker.run` fan-out (943 us vs 952 us for the
  same batch), and it is leak-clean. The proposed automatic sequential fallback
  below a threshold N is deliberately NOT added: the measurement shows the
  crossover is a function of work per item, not of N (at 8 items the layer is a
  100x loss for trivial work and a 5x win for heavy work), so an N-based cutoff
  would key on the one variable that does not decide the answer. The crossover
  (~20 us of work per item) is documented instead, next to the API. A parallel
  fold remains worth adding and now has numbers to design against. Harness in
  `benchmarks/fork-join/`, findings in `docs/cross-references/bend.md`.

### Added

- **Message tracing** (#1333). `ae build --trace` compiles a per-message
  delivery trace into a binary; `AETHER_TRACE=<file>` then records every step a
  message takes (which send path, which queue, when it was received, which step
  processed it) as JSONL, ordered by timestamp across cores. Message names come
  from a table codegen emits, because the runtime only ever sees the integer id
  the registry assigned, and a trace of bare ordinals is barely a trace.

  It is compiled out by default rather than switched off at runtime, and that
  is verifiable rather than asserted: building the scheduler before and after
  the hooks were added yields a **byte-identical object file**, same size and
  same disassembly. Message send is the runtime's core loop, so nothing less
  than "the machine code is unchanged" is worth claiming. When enabled, the
  cost is one timestamp and a 32-byte append to a per-core ring buffer, with no
  locks or atomics on the hot path: a core writes only its own buffer, and the
  merge runs after `scheduler_shutdown()` has joined every thread.

  This replaces the tracing capability the README advertised until PR #1330
  removed it as dead code that had no call sites, no activation surface and no
  overhead story. The claim is back because it is now true. See
  `docs/message-tracing.md`.
- **Secure streaming POSIX ustar archives.** Added `std.tar` readers and writers
  for regular files, directories, symlinks, modes, mtimes, prefix/name paths,
  checksum validation, and bounded payload streaming. The high-level extractor
  rejects traversal, unsafe symlinks, unsupported entry kinds, and configurable
  entry/size limit violations. Compression remains a separate layer.

## [0.496.0]

### Fixed

- **The HTTP client and server public headers can be included together**
  (#1433). Both published a type named `HttpRequest`, and they were different
  structs: `std/net/aether_http.h` an opaque handle for an *outgoing* request,
  `std/net/aether_http_server.h` a full struct for an *incoming* one. Any
  translation unit including both failed to compile, so a C consumer could not
  serve HTTP and make HTTP calls. The proxy had been working around it by
  hand-declaring the client prototypes under different type names, which is
  worse than the collision it dodged: duplicated signatures the compiler can no
  longer check against the real definitions, the same hazard `@c_import`
  (#1239) exists to remove for user code. The client type is now
  `HttpClientRequest`, the name the workaround had already chosen; the server
  keeps `HttpRequest` (its tinyweb-compatible surface, and the far more widely
  used of the two, mirroring how its response type is already
  `HttpServerResponse`). Both workarounds are deleted in favour of including the
  header. The Aether-level API is untouched: `std.http.client` passes these as
  `ptr`.

### Removed

- **`std/aether_std.h`**, an umbrella header that shipped in every install and
  was included by nothing. It listed 7 of the 44 std headers, appeared in no
  documentation, and had not been touched since it was added. Anyone who found
  it got a misleading tenth of the standard library. It could not simply be
  completed, either: including all std headers in one translation unit is a
  compile error (#1433 is why), which is how that bug was found. The public
  entry points remain the per-module headers, all of which are self-contained,
  and `include/libaether.h` for embedders.

### Added

- **A guard that every public header compiles standalone**
  (`tests/integration/public_headers`). Nothing checked this: the MSVC job
  probes a handful of runtime headers under `cl.exe` and stops there. All 45
  std and embedder headers pass today, and the check now also pins the
  client/server pair from #1433 in both include orders, since an
  order-dependent fix would not be one.

## [0.495.0]

### Added

- **Lossless binary64 text-conversion prerequisite** (#863 Phase 3.5). Implemented `string.from_double(value: float) -> string` in `std.string`. This function converts every finite IEEE-754 binary64 value to a lossless decimal representation (using `"%.17g"` format), ensuring exact bit equality after parsing back with `string.to_double()`. It handles positive/negative normal and subnormal values, signed zeros, NaN, and positive/negative infinity cleanly and locale-independently.

### Fixed

- **`string.to_float` / `string.to_double` rejected subnormal values.** `strtof` and `strtod` may set `ERANGE` for a representable underflow result, but the wrappers treated every `ERANGE` as failure. They now reject range overflow while accepting correctly rounded subnormal and zero results.

## [0.491.0]

### Fixed

- **Cross-compiling failed for every target from an installed toolchain**
  (#1420). `runtime/libaether_caps.c` included the public header as
  `../include/libaether.h`, a path that only exists in the source tree: an
  install puts the runtime at `share/aether/runtime/` and headers under
  `include/aether/`, so the hop resolved to a directory that does not exist.
  Worse, `include/libaether.h` was never installed at all, because both
  installers walk only the `runtime/` and `std/` trees for headers. Native
  builds never noticed, since they pass the include set from `ae cflags` and
  never rely on the relative path, which is how this shipped. The header is now
  installed by both paths and included by name, and the directory holding it is
  on the include path in both layouts. Verified end to end from a real install:
  `x86_64-linux` and `aarch64-linux` both produce working ELF binaries.

- **`ae build` served a stale binary after an imported module changed**
  (#1421). The cache key covered the entry file's content and the lib dirs, but
  not the project's own sibling modules: `import helper` next to `src/main.ae`
  resolves to `src/helper.ae`, which is in no lib dir. Editing it left the key
  unchanged, so the build reported `Built (cache hit)` and ran code from the
  previous version, and deleting `target/` did not help because the cache lives
  under `~/.aether/cache`. The key now covers the entry file's whole directory
  tree, content-hashed with the same caps the lib-dir walk uses. Unchanged
  rebuilds still hit the cache.

- **`return (a, b)` compiled to garbage instead of returning a tuple** (#1421).
  `(a, b)` is a tuple literal, so the parenthesised spelling reached codegen as
  one return value where the bare `return a, b` gives two, and the literal was
  flattened into the return slot. The C compiler then failed on identifiers
  invented from the flattened text (`NULL0` from `return (null, 0)`, `bufw` from
  `return (w.buf, w.off)`), with nothing pointing at the parentheses. Both
  spellings now produce the same AST, so they cannot disagree.

- **Multi-element `*StringSeq` literals leaked their inner cons cells**
  (#1417). `string_seq_cons` takes its own retain on the tail, so a builder must
  drop each intermediate handle. As a nested expression the intermediates were
  anonymous temporaries nothing ever dropped, leaving every cell but the head at
  refcount 2; a correct `string_seq_free(head)` then stopped at the first cell
  that stayed above zero and the rest leaked, 24 bytes per element past the
  first. The literal now folds into a local, dropping each handle once the next
  cell has retained it, with elements still evaluated in source order.

- **Cross-compiling failed with a longer-than-usual install prefix.** The
  command line is dominated by the include set, which scales with the prefix
  length and the module count, and it was assembled in a fixed 24576-byte
  buffer: past that, `ae` reported `cross-compile command exceeded the
  24576-byte buffer` with nothing the user could shorten. The command and object
  list now grow on demand, the same shape the include set itself already used.
  Found while reproducing #1420 from an install under a long path.

## [0.490.0]

### Added

- **Differential testing across lowering paths** (#523). Every
  `tests/differential/cases/*.ae` is built and run under both `--emit=exe` and
  `--emit=lib` and the outputs compared, because per-path correctness is weaker
  than cross-path agreement: a bug that miscompiles one path passes today, since
  that path's expected output was captured from the same build. A divergence is
  a hard failure naming both paths with the diff. Carved-out cases are reported
  with their reason on every run rather than silently skipped, and a carveout
  naming a case that no longer exists is an error, so the file cannot rot. Wired
  in as step 9 of `make ci` and available as `make test-differential`. See
  `docs/differential-testing.md`.

### Changed

- **Actor pooling measured and rejected** (#1332), with the numbers recorded in
  `docs/runtime-optimizations.md`. On the skynet benchmark constructing 11.1M
  actors, malloc/free is 2.9% of non-idle CPU during the tree-construction phase
  and 0.3-0.5% across a whole run, against `scheduler_spawn_actor`'s own 10.1%
  spent re-initializing state a pool would still have to re-initialize. The
  ceiling is too small to justify a size-class bucketed, NUMA-aware pool with a
  cross-core release path. A side finding is recorded with it: `tlv_get_addr` is
  6.7% of non-idle time, roughly twice the allocator, making thread-local access
  a bigger cost in the actor hot path than actor allocation.

## [0.489.0]

## [0.488.0]

### Added

- **`@c_struct ... @c_verify` checks overlay offsets against the C header**
  (#1242). A `@c_struct` overlay's field offsets are written by hand, because
  Aether never reads the C header. Nothing validated them, so adding a field
  upstream shifted the layout while the overlay kept reading the old offsets:
  the types still matched, the program still ran, and it silently read the wrong
  field. `@c_verify` emits a `_Static_assert` per field comparing the declared
  offset against `offsetof` and the declared width against `sizeof` of the real
  member, so drift is a build error that names the field. Opt-in, since
  `offsetof` needs the header in scope, and free at runtime.

- **Aether functions can be stored in C callback tables** (#1240). Declare the
  fields of a C-owned struct with their signatures (`hashFunction: fn(ptr) ->
  int`) and assign a top-level function to them. The slot is typed, so a
  function with the wrong arity or parameter types is a compile error rather
  than a corrupt call later. This is the `dictType` / `rio` / vtable shape that
  previously had to stay in C.

- **`va_list` parameters forward a variadic tail to C** (#1244). A function with
  a trailing `...` can open its tail with `va_start()` and pass it to the `v*`
  half of a printf-style pair (`vprintf`, `vsnprintf`), which is what logging
  and reply-formatting boundaries need.

### Fixed

- **Assigning an Aether function into a C struct's function-pointer field no
  longer crashes** (#1240). The field was given an `_AeClosure` **box**, which
  is a heap pointer, so the first callback from C jumped into a malloc'd struct
  and took SIGBUS. It compiled without a warning, and the `fn(...)`-typed
  spelling of the same field was rejected at typecheck, so there was no correct
  way to write it. C-owned struct fields now receive the function's real
  address, cast to the member type the header declared. Boxing is unchanged for
  Aether-owned structs, where the field holds a closure and must keep its
  captures.

- **A forwarded `va_list` is dereferenced at the call boundary** (#1244).
  `va_start()` yields a cookie pointing at the function's `va_list`, and
  `va_arg` / `va_end` unwrapped it but the call site did not, so a `v*` callee
  received a pointer where the argument list belonged. No warning, no crash,
  just garbage in the formatted output. Spelling the parameter `va_list` now
  emits the unwrap.

- **`docs/c-interop.md` no longer claims Aether cannot define varargs.** It
  stated that "an ordinary Aether wrapper cannot forward a `...` tail (Aether
  has no varargs-defining syntax)", which stopped being true once the
  `va_start` / `va_arg` / `va_end` intrinsics landed.

## [0.487.0]

### Added

- **`extern ... @c_import` lets a C header own the prototype** (#1239, #1241).
  Aether normally emits its own forward declaration for an `extern`, spelled
  from the Aether types: `int` where the header says `uint8_t` or `size_t`,
  `void*` where it names a type. The two are ABI-compatible but not identical,
  so the translation unit ends up with two disagreeing declarations of one
  function, a hard `conflicting types` error in the cases that matter and, under
  LTO, type-mismatch warnings that read exactly like real porting mistakes.
  `@c_import` after the signature emits no declaration at all, leaving the
  header's spelling as the only one present, so the two cannot disagree. Call
  sites are still checked against the header, so a wrong Aether signature is
  still caught. This is also the only correct shape for a `static inline` header
  helper, which has no linkable symbol for a non-static prototype to refer to:
  the call inlines directly and the one-line C bridge functions such a helper
  used to require are no longer needed. Stacks with the existing extern
  attributes (`-> string @heap @c_import`, variadic `@c_import`).

- **`ae build --emit=obj` compiles a `.ae` straight to a relocatable object**
  (#1243). Previously the only way to feed Aether into an existing C link line
  was `--emit=csrc` plus a hand-written rule to compile the generated C, which
  in practice meant checking that C into the tree: a stale-artifact trap, since
  editing the `.ae` and restoring the `.c` to keep a diff clean leaves a plain
  `make` linking the old code silently. `--emit=obj` produces the `.o` directly,
  so a `%.o: %.ae` rule makes the `.ae` the only source and the compared
  timestamp the right one. The object carries the same catalog as `--emit=lib`,
  exporting both the bare names and the `aether_<name>()` C-ABI aliases. Honours
  `$AE_CC` / `$CC` with the same resolution order as the other build paths.

### Fixed

- **`install.sh` no longer rejects the GNU make it just found.** The probe was
  `make --version | head -1 | grep -qi 'gnu make'`, and the script runs under
  `set -o pipefail`: `head` exits after the first line, the producer then dies
  of SIGPIPE, and the pipeline reports that failure even though the read
  succeeded. Whether it happens depends on the producer finishing before `head`
  closes the pipe, so it failed intermittently and only under load, printing the
  self-contradicting `Error: 'gmake' is not GNU make` immediately followed by
  `Found: GNU Make 4.3`. The banner is now captured once with no pipe to break,
  and the test and the error message read that same value, so they cannot
  disagree. Applied to the three other `| head -1` probes in the script, which
  had the same latent race, including the one guarding `set -e`.

- **A trailing extern attribute no longer discards an earlier one.** `extern
  printf(fmt: string, ...) -> string @heap` overwrote the annotation slot that
  already held the variadic marker, so the extern quietly stopped being
  variadic and its call sites lost variadic arity checking. Markers now
  accumulate through one shared `;`-delimited helper, which also replaces the
  four hand-rolled copies of the marker test that had drifted apart across the
  parser, typechecker and codegen.

## [0.486.0]

### Added

- **`std.fs` reports the raw OS code behind its portable error kind** (#1378).
  `KIND_*` is deliberately coarse so it means the same thing on every platform,
  which leaves no way to tell `EAGAIN` from `EWOULDBLOCK` or to put the exact
  number in a log. `fs.last_os_error()` carries it, recorded at the single
  errno-to-kind translation site so the code and the kind cannot drift apart. It
  is 0 after a success, and a call reports only its own code, never a stale one
  from an earlier failure.

### Changed

- **Panic categories are a stable, greppable vocabulary** (#1378).
  Unrecoverable failures now lead with a fixed token: `precondition_violation:`,
  `postcondition_violation:`, `forced_unwrap_none:`, followed by the human
  detail and location. Previously each site invented its own wording, so CI and
  downstream triage could not match on the failure class. The canonical list is
  documented in the language reference. A failure that forwards an existing
  error value still prints that error's own message, since it did not originate
  in one of these classes.

### Fixed

- **The `-I` list no longer silently drops directories.** `ae` built it in a
  fixed 16 KB buffer, which the tree walk outgrew once the install prefix was
  long: 153 directories under a `/var/folders/.../T/tmp.XXXX/` path overflow it,
  and the overflow dropped entries with only a warning, so a build could fail to
  find headers that are present. The buffer grows now. The install smoke test
  used to print that warning on every run and no longer does.

## [0.485.0]

### Added

- **`std.bytes` exposes `data`, `capacity` and `set_length`** (#1399). All
  three existed in C but were never declared or exported, so anything handing a
  buffer to a foreign runtime had to copy it byte by byte through `get()`, or
  redeclare the extern itself against an internal name with no compatibility
  promise. For a 1 MB buffer crossing into JavaScript that is one `HEAPU8.set`
  against a million calls. `std.cryptography.aes` had made exactly that private
  redeclaration while being wired to its native core, and now uses the public
  surface.

### Fixed

- **The release archive-export gate now covers the FreeBSD cross leg** (#1402).
  The Unix and Windows release legs run it through `test-release-archive`; the
  cross leg built and packaged without it, so a cross-built archive could ship
  missing a symbol its own sources define and fail at the user's link step.

## [0.484.0]

### Changed

- **`std.cryptography.aes` runs on a native core** (#1394). The cipher was
  written in Aether and reached every byte of state through `bytes.get` and
  `bytes.set`, roughly 15 million calls per megabyte for AES-256, measured at
  4.4 MB/s. The same cipher now lives in `std/cryptography/aes/aether_aes.c`
  and works on a raw pointer: 1 MB of AES-256-CBC goes from 227 ms to 12 ms,
  about 21x, and `cbc_encrypt` / `cbc_decrypt` / `ctr_xor` drive the loop in C
  rather than one call per block. Every mode built on `process_block` (GCM,
  CCM, EAX, OCB, CMAC, key wrap) inherits it.

  There is no second implementation: the byte-by-byte Aether cipher is deleted
  rather than kept as a fallback. It is plain C, so every target Aether
  compiles for has it, and a fallback would only be a copy to keep in step.
  Correctness is pinned where it already was, the FIPS-197 Appendix B and C
  known-answer vectors in `tests/regression/test_aes.ae`.

## [0.483.0]

### Fixed

- **A closure env now owns the strings it captures, and frees them** (#1398).
  A `-> string` captured by a closure could be read after free once the job ran
  on a `std.worker` pool thread: `aether_str_capture` called `string_retain`,
  which is a documented no-op on a magic-less pointer, so a plain malloc'd
  string was captured BORROWED and the enclosing scope's loop-carried
  reassignment freed it out from under the closure. Integer captures in the
  same closure were fine, which made it look like a threading bug. The env now
  retains the strings it captures and reclaims them through its destructor;
  five owners (scope-exit defer, the argument drain, `list_free`,
  `fs_closure_free`, and the worker pool) route env cleanup through that
  destructor instead of a plain `free()` that reclaimed the struct and leaked
  every captured string. Caught by the macOS leak gate (`test_worker` leaked 2
  per run); `test_worker`, `test_worker_pool`, and `test_worker_wait_map` now
  report 0 leaks.

## [0.482.0]

### Fixed

- **A heap string passed to a `std.bytes` reader no longer leaks.** The
  compiler's non-storing-callee allowlist covers the string readers
  (`aether_string_data`, `string_seq_join`, `println`, ...) but never included
  `std.bytes`, so passing a heap string to `bytes.length` / `bytes.get` marked it
  escaped and suppressed its scope-exit free. The caller leaked unless it added
  an explicit `release()`, and nothing signalled that. The scalar-returning
  readers (`length`, `capacity`, `get`, `get_le16/32/64`, `get_be16/32/64`) and
  `copy_from_string`, which copies out and retains nothing, are now listed.
  `aether_bytes_data` is deliberately excluded: it returns an interior view
  rather than a scalar. Verified on the emitted C (0 scope-exit frees before, 1
  after) and through the macOS leak gate.

## [0.481.0]

### Added

- **The release archive is checked against the sources it ships** (#1395).
  0.467.0 shipped `std/worker/aether_worker.c` defining `aether_worker_wait`
  next to a `libaether.a` built from an older tree that did not export it, so
  `worker.wait()` and `worker.map()` failed to link. Nothing failed on our side;
  it failed at the user's link step, and only for the function added last.
  `make check-archive-exports` (run as part of the release-archive smoke test)
  now fails when a symbol a std module declares `extern`, and a std C source
  defines, is absent from the archive. Externs with no std definition are libc
  or optional-dependency symbols and are skipped, which keeps it free of false
  positives. Verified both directions: it passes on a freshly built archive and
  flags six stale symbols on the installed 0.467.0-era one, including the
  `aether_worker_wait` from the report.
- **The install smoke test reports why it failed.** `install.sh` ran with its
  output sent to `/dev/null`, so a genuine break printed `[FAIL] Install smoke
  test` and nothing else; diagnosing it meant reproducing locally. Its output is
  now captured and the last 30 lines are printed when it fails.

## [0.480.0]

### Fixed

- **A module-qualified call inside a `return` struct literal is no longer given
  a doubled module prefix** (#1383). `intarr.intarr_new_raw(4)` in
  `return Box { a: ... }` was emitted as `intarr_intarr_new_raw`, which does not
  exist, so it surfaced as a C compiler error rather than an Aether diagnostic.
  The same call resolved correctly in every other position, which is why it went
  unnoticed. The emitted callee is now resolved against the declared extern
  instead of assuming the C symbol is `<module>_<name>`: substituting `_` for the
  dot is wrong both when the export already carries the module name
  (`intarr.intarr_new_raw`) and when it carries none
  (`os.aether_args_count`). Any module following the `<module>_<verb>` export
  convention was exposed, including `std.fs`, `std.zlib` and `std.bytes`.

## [0.479.0]

## [0.478.0]

### Fixed

- **`make release` / `make install` failed on a fresh tree** with
  `fatal error: aether_stdlib_symbols.h: No such file or directory`. The
  generated stdlib-symbols header (added with #1366) was a prerequisite of the
  `compiler` target but not of `release`, so `make install` compiled codegen.c
  before generating it. `release` now depends on the header. Also (from the
  FreeBSD toolchain ask): `install.sh` prefers `gmake` and verifies it is GNU
  make (bare `make` is BSD make on the BSDs and cannot parse this Makefile),
  and resolves + passes `CC=` through; and `ae build`/`ae run` default the C
  backend to `gcc` when present, else the POSIX `cc`, so a box with no gcc
  (FreeBSD/macOS) builds out of the box.

## [0.477.0]

### Fixed

- **Indexing a `string` reads its payload, not a struct header** (#1380). A
  `string` value is either a plain `char*` or a refcounted `AetherString*`, and
  the length-bearing producers (`fs.read_binary_tuple`, `string.concat`) return
  the latter. `println` and `string.length` already detected the magic header
  and unwrapped; the `[]` operator did not, so `d[0]` on a binary file read
  returned -34, the first byte of the header, rather than the first byte of the
  file. Silent and plausible-looking rather than an error. Indexing now goes
  through the same payload accessor, so every `string` indexes identically
  whichever representation it carries. Note `string.concat` was not at fault as
  the issue supposed: it already read its inputs correctly through the
  dispatching accessors, and only its indexed *result* was misread.

### Added

- **Unreachable `match` arms are reported** (#1377), warning `W1004`. Arms are
  tried in source order, so an arm an earlier one already covers is dead code
  the compiler used to accept silently: a mis-ordered `_`, or two arms meant to
  differ that a typo collapsed onto the same case. Three shapes are reported: a
  duplicate case, an arm below a `_` catch-all, and a `_` on a sum or enum
  match where every case is already handled. That last one is worth removing
  rather than silencing, because without it adding a variant later becomes a
  compile error from the exhaustiveness check instead of quietly falling
  through.

  Reported only where the shadowing is certain. A bare identifier arm binds the
  value rather than naming a case, so it is never compared, and a `_` over a
  partially-handled enum is left alone. Verified against every `.ae` file in
  the tree: one genuine dead arm, in `tests/regression/test_enum_basic.ae`,
  now removed. No other file warns.

## [0.476.0]

### Fixed

- **An entry-file function no longer collides with a standard-library C
  symbol** (#1366). `libaether.a` exports a couple of thousand bare C symbols
  and grows every release, so a program with its own top-level
  `string_replace_all` stopped linking the moment `std.string` gained a
  function lowering to that name, with no change to the program. A colliding
  definition is now renamed to the `ae_` spelling already used for libc
  clashes, and its call sites follow. The symbol set is generated from the
  `std` module files at build time rather than hand-maintained, because a
  hand-kept list is exactly what let this reach a release. Covers three shapes:
  a matching signature (a link error), a differing signature (raw C
  conflicting-declaration errors), and a collision with a module the program
  never imports (which previously linked and silently ran the standard
  library's function).
- **The lexical path helpers are correct on Windows** (#1369). `path.clean`,
  `path.rel` and `path.is_within_base` split on a hardcoded `/` and knew
  nothing about drive letters or UNC prefixes, so on Windows `clean` left `..`
  unresolved in a backslash path, `is_within_base` rejected legitimately
  contained paths, and `rel` returned nonsense like `../C:\x\y\z`. All three
  now accept either separator, carry a `C:` or `\\server\share` prefix
  through untouched, compare case-insensitively as the filesystem does, and
  emit the platform separator. POSIX is unchanged: a backslash stays an
  ordinary filename byte. `path.join_clean` is now reachable from `std.path`
  rather than only `std.fs`, and the whole set (`clean`, `join_clean`, `rel`,
  `is_within_base`, `separator`) is documented in the stdlib reference, which
  still listed only the original five path functions.
- **`ae build` rejects an unknown option** instead of ignoring it. A typo such
  as `--targt=x86_64-linux` was silently dropped, so the build quietly produced
  a host binary and reported success.
- **The constant folder no longer applies standard-library semantics to a name
  the program defines.** `string.concat` / `string_concat` and the
  `string.from_*` conversions folded to a literal keyed on the callee name
  alone, so a program defining its own `string_concat` had calls to it replaced
  at compile time by the standard library's result: the wrong answer, silently,
  with no diagnostic. Folding now skips any name the program defines as a
  top-level function. Found while fixing #1366.

## [0.475.0]

### Changed

- **The FreeBSD native build-and-test job now runs on every pull request**,
  not only on merges to main. It was put behind the merge on the assumption
  that a VM boot plus a from-scratch build would cost tens of minutes; measured
  on its first real run it is 1.6 minutes end to end, including 229/229 unit
  tests. There is no reason to find out after merge what can be known before.

### Fixed

- **The build no longer assumes gcc exists.** `CC` was hardcoded to `gcc`, so a
  native build on FreeBSD, whose base ships clang as `cc` and carries no gcc at
  all, died with `gcc: No such file or directory` before compiling anything.
  `CC` now prefers gcc, then the system `cc`, then clang. Platforms that build
  today are unaffected: they all have gcc, macOS included, via its clang shim.
  Caught by the new FreeBSD native CI job (#402) on its first real run.

## [0.474.0]

### Added

- **FreeBSD is a real CI target** (#402), split by what each check can catch.
  Every pull request cross-compiles the toolchain for FreeBSD (`FREEBSD=1`,
  zig cc against a pinned FreeBSD base sysroot): a compile break can only be
  caught by compiling, and this is fast and deterministic. Every merge to main
  additionally builds the tree and runs the C unit suite inside a native
  FreeBSD VM, which is what covers runtime divergence (the kqueue poller) and
  costs too much to put in front of every pull request. Both hard-fail. Until
  now the only FreeBSD compile in the tree lived in the release workflow, so
  nothing read the FreeBSD branch of any `#ifdef` before code landed. `make ci`
  cannot cover this on its own: it compiles only the branch of each platform
  conditional that matches the host it runs on.

### Changed

- **A failed FreeBSD build now fails the release** instead of silently shipping
  without the FreeBSD asset. The leg was `continue-on-error` and deliberately
  left out of the publish gate, so a broken FreeBSD build produced a green
  release with a platform quietly missing. That is what kept the `MNT_NODEV`
  break invisible after it merged.
- **Mount options are built from a table of the flags the platform actually
  defines**, rather than a fixed format string with an empty-string substitute
  for whatever is missing. A flag absent from the OS is absent from the table,
  so FreeBSD (which removed `MNT_NODEV` in 10, where nodev became a no-op)
  reports no nodev state instead of reporting it as off.

### Removed

- **The optional-macro portability probe** (`make ci-optional-macros`), added
  one release ago to simulate a missing `MNT_NODEV` by preprocessing the source
  and recompiling it. Compiling for FreeBSD in CI supersedes it: the probe
  approximated one platform through a hand-maintained list of file/macro/anchor
  triples that every future guard had to be added to by hand, and its awk
  line-insertion and its hardcoded `-std=` both diverged from the real build
  before it caught anything. `make ci` is back to 9 steps.

### Fixed

- `fs_is_socket` and `os_user_id_raw` (#1368) were defined without declarations
  in `std/fs/aether_fs.h` and `std/os/aether_os.h`, the only functions in
  either module missing a prototype. Documented the new stat kinds,
  `fs.fs_is_socket` and `os.user_id()` in `docs/stdlib-reference.md`, where the
  kind encoding still described only kinds 1 through 4.

## [0.473.0]

### Added

- **`fs` stat now distinguishes sockets, FIFOs and devices** (#1368). Previously
  all three collapsed into `STAT_KIND_OTHER` (4), so an AF_UNIX socket was
  indistinguishable from a FIFO. `fs.fs_get_stat_kind` / `dir_list_kind` now
  return `STAT_KIND_SOCKET` (5), `STAT_KIND_FIFO` (6), `STAT_KIND_DEVICE` (7)
  on POSIX (named constants exported from `std.fs`; Windows keeps `OTHER`).
  Adds `fs.fs_is_socket(path)` (mirrors `fs_is_symlink`) and, in `std.os`,
  `os.user_id()` (POSIX `geteuid`; -1 on Windows). Together these let e.g.
  aeb's podman-socket auto-detect move out of a bash trampoline into Aether.
  Regression: `tests/regression/test_fs_stat_kind_socket_fifo.ae`.
- **`std.path` surfaces the lexical path helpers and a separator accessor**
  (#1369). `path.clean` (lexical normalize — no filesystem access, works on
  paths that don't exist yet), `path.rel`, `path.is_within_base`, and the new
  `path.separator()` ("/" POSIX, "\\" Windows) are now reachable via
  `import std.path` (they previously lived only under `std.fs`, so callers
  reaching for a normalize couldn't find it). Also exported from `std.fs`.
  Regression: `tests/regression/test_path_clean_separator.ae`.

### Fixed

- **`fs.glob` dropped the directory prefix for a simple glob on Windows**
  (#1367). A non-`**` pattern like `dir/*.c` returned bare `foo.c` from the
  `FindFirstFile` backend, where POSIX `glob(3)` returns `dir/foo.c`; the
  prefix is now reattached so both platforms agree (the recursive `**` path was
  already correct). Regression: `tests/regression/test_glob_dir_prefix.ae`.

## [0.472.0]

### Fixed

- **FreeBSD build break in `fs.mounts`.** `MNT_NODEV` was used unguarded in
  the BSD `getmntinfo` backend. FreeBSD deprecated that flag and removed the
  macro in FreeBSD 10, while macOS and OpenBSD still define it, so the
  fallback path had never been compiled anywhere and the break only appeared
  on FreeBSD. The flag is now probed with `#ifdef` rather than keyed on the
  platform, so a future removal elsewhere degrades to omitting the option
  instead of failing the build.
- **NetBSD was claimed but never buildable.** It sat in the same
  `getmntinfo` branch, but there the call fills a `struct statvfs` and the
  flag field is `f_flag`, not `f_flags`, so that body could not have
  compiled. NetBSD now falls through to the unsupported branch and reports
  the error, matching the module's convention of degrading rather than
  fabricating, instead of shipping a shape nobody has built.

### Added

- **`make ci-optional-macros`, a portability probe, now step 10 of `make
  ci`.** It recompiles the affected sources with each guarded platform macro
  forced absent, so both sides of every `#ifdef` are built on every run.
  This reproduces the FreeBSD failure above on any host: verified by
  reintroducing the bug and watching the probe fail, then restoring the fix
  and watching it pass. Registering a new guarded macro is one line.

## [0.471.0]

### Added

- **`std.http.proxy` gains a trailing-block "config IS code" DSL** for pool
  setup, alongside the existing positional API. `proxy.pool(algo, ...) { ... }`
  runs first and returns the pool ptr, which becomes the block's
  `builder_context()`; the child calls (`upstream`, `health`, `breaker`,
  `rate_limit`, `cookie_name`, `drain`) configure that live pool. Body-first
  ordering means the pool already exists when the children run — thin,
  allocation-free sugar over the same functions, no recording/replay. Both
  surfaces drive the identical pool. Example:
  `examples/stdlib/http-reverse-proxy-pool-dsl.ae`; regression:
  `tests/regression/test_proxy_pool_dsl.ae`.

### Fixed

- **Repository URLs pointed at the pre-rename organisation.** Every
  `github.com/aether-lang-org/...` reference (57 across the README, LLM.md,
  docs, `get.sh`, the release workflow, Docker scripts and the Makefile) now
  names `aether-lang-dev`. They had been resolving only through GitHub's
  rename redirect, a silent dependency that breaks the install one-liner,
  the release pipeline's crossbuild checkout and every documented clone
  command the day it lapses. Each rewritten target was verified to resolve,
  including the `raw.githubusercontent.com` one-liner and the workflow's
  `repository:` field, neither of which is a `github.com` URL. The
  `servirtium-vcr` links were wrong independently of the rename: that repo
  lives in the `servirtium` org and never existed under Aether's (LLM.md
  already said so in one place), so those five now point at
  `servirtium/servirtium-vcr`. `CHANGELOG-archive.md` keeps its historical
  URLs as written.
## [0.470.0]

### Fixed

- **Benchmark runner reported a negative `cv_pct`** (#1352). The
  coefficient of variation was computed as `(best - worst) * 50 / mean` in
  32-bit int, so for the fastest patterns (skynet and counting run in the
  hundreds of millions of msg/sec) the multiply overflowed before the
  divide and the result came out negative, on exactly the numbers most
  likely to be quoted. Span, mean and CV are now 64-bit, the run
  accumulator is too (five runs at 250M already approach the int32
  ceiling and `BENCH_RUNS` is user-settable), and a negative value is
  refused rather than published, since a coefficient of variation cannot
  be negative. `docs/performance-benchmarks.md` states the guarantee.
- **Constant folding did not preserve runtime semantics for `int`**, which
  is what let the overflow above hide during development. The folder
  evaluates in `double`, so `(250000000 - 200000000) * 50 / 225000000`
  folded to `11` while the same expression over `int` variables evaluated
  to `-7`: the literal form looked correct. The fold now wraps exactly as
  the runtime does and reports the overflow (new `warning[W1003]`, with
  the exact value, the wrapped value, and how to widen). No code in the
  tree trips it.
- **`benchmarks/http/baseline_results.txt` was committed containing only a
  header** (#1353). The harness writes the header, starts the server, then
  measures; the server had failed to start, `set -e` exited, and the stub
  was committed. The generated file is removed from the tree and
  gitignored (it is machine-specific), and both HTTP harnesses now
  preflight `wrk` before building anything, write to a temp file and
  publish only on success (so an interrupted run leaves no truncated
  artifact), and report the actual cause when the server cannot start
  instead of a bare "failed".

## [0.469.0]

### Added

- **Per-symbol aliasing in selective imports**: `import vg (rect, path as
  vgpath)` binds the exported symbol under the alias and frees the original
  name for local use, the standard resolution when exactly one name in an
  otherwise-convenient selective import collides. Works for constants as
  well as functions, inside a module's own imports (aliases resolve when
  that module's bodies merge into a consumer), and alongside module-level
  `as`. The selective-import shadow guard now fires on the alias, the name
  the program actually binds, so a local `path` beside `path as vgpath` is
  legal while a local `vgpath` is still rejected. Closes #1345.
- **`worker.wait()` and `worker.map(items, f)`** (#1350). `wait()` blocks
  until every submitted job has completed and been delivered, running the
  completions on the calling thread and leaving the pool reusable: the
  headless batch join that previously had to be hand-rolled as a
  `pending()`/`drain()`/`sleep()` poll, and that `pool_shutdown` could not
  provide because it tears down the process-global pool (a second batch
  then returned nothing). It blocks on a condition variable, not a poll, and
  returns -1 rather than deadlocking when a main-thread poster is installed.
  `map` is the bounded parallel map over a list, results index-aligned with
  the input, concurrency bounded by the pool. Regression:
  `tests/regression/test_worker_wait_map.ae`.
- **`string.join(seq, sep)`**: concatenate a `*StringSeq`'s elements with a
  separator, the complement of `string.split_to_seq` and a linear-cost
  escape from the O(n^2) self-append accumulation trap (two passes, one
  exact-size allocation, binary-safe on elements and separator). The trap
  itself, and both escapes (`std.strbuilder` for piece-by-piece appends,
  `join` for an existing sequence), are now documented in the Standard
  Library Reference. Closes #1346.

### Fixed

- **Containers holding one string value could not both be freed** (#1349).
  `list_add_string_owned` / `map_put_string_owned` are documented as
  acquiring their own reference, but they adopted the caller's instead, so
  a string literal added through them was libc-freed from `.rodata` at free
  time (malloc's error path aborts, which the reporter saw as a hang), and
  two containers each "owning" one pointer freed it twice. The owning
  entries now genuinely own: a refcounted string is retained, a plain
  pointer is copied. Codegen's escaping-value path moved to new
  `*_string_adopted` siblings, so its zero-cost ownership transfer and the
  no-per-add-leak property are unchanged. Regression:
  `tests/regression/test_list_shared_string_free.ae`.
- **Closures returning a parameter truncated pointers.** A block closure's
  return type is inferred from its body, but the inference only looked in
  the parent function's scope, so `|x: ptr| { return x }` fell back to `int`
  and the emitted C signature truncated a 64-bit pointer. The closure's own
  parameters are now consulted first.
- **`call(f, ...)` on a `fn` parameter truncated pointers.** The `call`
  builtin is typed `int` by default and resolved to the real type only when
  the callee is a known local closure. In `-> ptr fn(...) { return call(f,
  v) }` the enclosing function's declared return type now supplies it, which
  is the one case the closure-body resolution cannot see.
- **Heap-producing calls inside string interpolation leaked their
  temporary**: `"[${string.join(parts, ",")}]"` allocated a result that
  nothing owned, once per interpolation, in both the printf (print/println)
  and the heap-building forms. Interpolation segments that are
  heap-producing calls now bind to a drained temp that is freed after the
  segment is consumed; bare identifiers keep their owner's free.
- **A sequence accumulator later passed to `join` leaked its whole spine.**
  The seq escape walk allowlists `string_seq_*` callees as pure readers, but
  the `string.join` wrapper normalises to `string_join`, so a
  `s = seq_cons(x, s)` loop feeding a later join was conservatively marked
  escaped and every intermediate spine ref leaked. Both `string_join` and
  `string_seq_join` are now recognised as non-storing readers.


## [0.467.0]

### Fixed

- **A caught panic leaked every allocation made since the `try`** (#1301).
  `longjmp` skipped the deferred scope-exit frees, so guarded blocks (and
  every scheduler-wrapped actor step) leaked whatever they had allocated
  before panicking. A thread-local allocation journal now mirrors the armed
  deferred frees one-for-one: generated code journals a tracked local when
  its flag is armed, the single free choke point forgets on every normal
  free, ownership handoffs (return, container/actor/message adoption)
  forget at the transfer, and `aether_panic()` drains the innermost frame's
  still-live entries before the jump, freeing exactly the frees the jump
  would have skipped. Escaped values are never journaled, so the drain is a
  leak-fix by construction, never a use-after-free. Nested `try` drains
  stay frame-local; a panicking actor's step-scoped allocations are
  reclaimed before the actor is marked dead; the no-panic hot path shows no
  measurable cost on the ping-pong benchmark. Verified 405 leaks to 0 on
  the issue's alloc-then-panic matrix. Regressions:
  `tests/regression/test_panic_unwind_cleanup.ae`,
  `tests/integration/panic_unwind_no_leak/` (50000 caught panics, RSS
  flat), `tests/integration/panic_actor_step_drain/`.

### Added

- **`std.string.join(seq, sep)`** — linear-cost join of a `*StringSeq` with a
  separator, the complement to `string.split` / `split_to_seq` (issue #1346).
  One pass to size, one allocation, one pass to copy — the guaranteed-O(n)
  escape from the `d = "${d}${piece}"` accumulation trap (which re-copies the
  whole prefix every iteration, O(n²)). For incremental building where the
  pieces aren't already a sequence, `std.strbuilder` (amortized-O(1) append)
  covers the builder half of #1346; `std.string` now points at both up front.
  Regression: `tests/regression/test_string_join.ae`.
- **`fs.mounts()` and `fs.block_info(dev)`** (#1118). Mount enumeration
  with per-entry source/point/fstype/options accessors: Linux
  `/proc/self/mountinfo` (octal escapes decoded), macOS and the BSDs
  `getmntinfo(3)`, Windows drive letters. Block-device size/removable/
  transport via the Linux sysfs backend (partitions resolve the removable
  flag through their parent disk); other platforms report unsupported
  through the error slot rather than fabricating an answer. Regression:
  `tests/regression/test_std_fs_mounts.ae`.

## [0.463.0]

### Added

- **TLS 1.3 client: OCSP stapling now verifies the responder signature**
  (`std.cryptography.tls13_cert.verify_ocsp_signature`, RFC 6960 §4.2.2.2). A
  stapled OCSP response is authenticated against the leaf's issuer (direct
  signing) or a stapled delegate certificate that is itself issuer-signed and
  carries the `id-kp-OCSPSigning` EKU. `connect()` fails the handshake closed
  only on an *authentic* REVOKED; a staple whose signature does not verify is
  ignored (fail-open), so a forged REVOKED cannot DoS the handshake. Supports
  RSA-PKCS#1-SHA256 and ECDSA-P256/P-384 responder signatures. Verified against
  real DigiCert (direct) and GoDaddy (delegated) staples;
  `tests/integration/crypto_tls13_ocsp/`.
- **TLS 1.3 client: ECDSA-P384-SHA384 server CertificateVerify** (SignatureScheme
  `0x0503`), unblocking P-384-leaf sites (e.g. Wikipedia). Advertised in the
  ClientHello signature_algorithms; `tests/integration/crypto_tls13_cert_p384/`.
- **TLS 1.3 client: mutual TLS** — `connect_mtls()` presents an ECDSA-P256 client
  certificate + CertificateVerify on a server CertificateRequest; `connect()`
  otherwise declines with an empty Certificate.


- **`string.replace(s, old, new)` and `string.replace_all(s, old, new)`**
  (#1331). Non-overlapping left-to-right matches, byte-exact and binary-safe;
  `new` may be empty (deletion) or longer than `old`; empty `old` returns a
  copy unchanged (Go's `strings.Replace` guard). Single exact-size allocation
  regardless of match count; results are heap-tracked like `substring`.
  Regression: `tests/regression/test_std_string_replace.ae`.
- **`ae fmt` CI gate** (#1302). `tests/integration/fmt_gate/` enforces the
  formatter's documented safety properties on every CI run: all checked-in
  `.ae` sources under `std/`, `examples/`, and `tests/` are canonically
  formatted (`ae fmt --check`), formatting is idempotent, and a formatted
  file's generated C is byte-identical to the original's (modulo `#line`).
  The whole tree was formatted in this change (595 files, whitespace-only);
  the IR-preservation property was verified on all 443 compiling program
  files before and after: zero differences.

### Fixed

- **Actor `?` ask answered 0 and leaked a 5s timeout when the handler used
  `reply <expression>`** (#1324). `reply count` parsed but codegen silently
  dropped it (an ERROR comment in the generated C), so the ask waited out its
  timeout and read 0; the fallback also cast the reply pointer to `intptr_t`,
  emitting a `-Wformat` warning. `reply <expression>` is now a first-class
  scalar reply: the handler sends a typed copy through the reply slot, the ask
  site derefs it as that type (int, long, float, bool, ptr, string), and the
  GCC statement-expression path now matches the MSVC helper's deref semantics.
  An unknown message name in `reply Name { ... }` is a compile error instead of
  silently generated nothing. Regression:
  `tests/regression/test_ask_scalar_reply.ae`.
- **Heap-producing calls in bare `print`/`println` argument position leaked
  per call** (`println(string.concat(a, b))`). The interpolation form already
  freed its temporaries; the direct-argument form never did, for every heap
  producer. Both forms now route through owned-print helpers that print and
  free in one step; bound identifiers keep their scope-exit free.
- **String-literal emission: hex-escape maximal munch corrupted bytes, and
  binary bytes made the generated C a binary file.** A C hex escape has no
  length limit, so an emitted `"\x01a"` re-lexed as the single byte 0x1A
  whenever a hex-escaped byte preceded a hex-digit character; and bytes above
  0x7F (decoded `\x` escapes, e.g. CBOR/MsgPack test vectors) were written
  raw, producing invalid-UTF-8 output that text tools mishandle
  platform-dependently (surfaced as phantom checksum mismatches on Windows
  CI). The emitter now writes zero-padded octal (`\001`, munch-proof by
  construction) and keeps only valid UTF-8 sequences raw, so generated C is
  always valid text. Regression:
  `tests/regression/test_string_escape_bytes.ae`.

## [0.462.0]

### Fixed

- **Heap tracker: tracked-empty error string leaked when dropped outside `main`**
  (#1311). The return-heap classifiers took bare-identifier evidence from the
  tracker table of whichever function happened to be emitting when a callee's
  memo was first computed, so a std `(value, error)` tuple destructured inside a
  helper function classified its error slot non-heap and leaked one allocation
  per call (1 byte per asn1 `read_*`). Identifier evidence now resolves
  structurally against the analysed function's own body, classification is
  order-independent, and the asn1 error chain settles on the non-allocating
  literal path. Regression probe:
  `tests/integration/heap_tracker_nested_tuple_err_no_leak/`.
- **Actor destroy under libnuma freed with the wrong size.** Release freed every
  actor with `sizeof(ActorBase)` while spawn allocated the full derived-struct
  size; `numa_free` unmaps exactly the given range, so each destroy leaked the
  derived tail on NUMA builds. The allocation size is now stored on the actor
  and used at release.
- **Latent wrong-allocator free in actor teardown.** The release path
  reinterpreted every `ActorBase*` as a pool struct and, when the overlaid bytes
  landed in range, routed NUMA-allocated memory to plain `free()`. The
  never-wired pool machinery is removed (below); teardown frees through
  `aether_numa_free` unconditionally.

### Removed

- **Inert actor-pool machinery.** Per-core `ActorPool`s were allocated and
  initialized on every core, but `actor_pool_acquire` had zero call sites, so
  actor pooling never existed at runtime; the pools cost memory and the
  release-path cast was the corruption hazard above. Removed both pool headers
  (one was a duplicate included nowhere), the dead `AETHER_ACTOR_POOL_SIZE` env
  knob and its profile constants, the never-incremented `actors_pooled` counter,
  the false "Actor Pooling [ON]" config print, and the isolated unit tests that
  exercised the unused data structure. `scheduler_spawn_pooled` /
  `scheduler_release_pooled` are renamed `scheduler_spawn_actor` /
  `scheduler_release_actor` (nothing pools); all docs updated.
- **Dead message-tracing TU.** `runtime/utils/aether_tracing.c` compiled into
  every build and generated C included its header, but no code path ever called
  it; the README advertised message tracing that did not exist. Removed the TU,
  the `#include` emission, the manifest rows, and six orphaned generated-C
  snapshots under `tests/integration/` that nothing compiled.
- **`scheduler_enable_features`.** Zero callers; its only live effect duplicated
  `aether_enable_opt(AETHER_OPT_LOCKFREE_MAILBOX)`.

### Added

- **Emitted-C determinism gate and documented guarantee** (#1299). The
  byte-identical-output property (same source + same compiler build) is now
  stated in `docs/architecture.md` with its invariants and scope boundary, and
  enforced by `tests/integration/emit_c_determinism/`, which compiles a
  seven-program corpus twice, byte-compares, and rejects timestamp macros.
- **`docs/http-server.md` static-file section**: `http.serve_file` (zero-copy
  `sendfile(2)` fast path) and `http.serve_static` (traversal-rejecting,
  Range-aware).

### Changed

- **README repositioned** (#475): leads with the capability sandbox,
  config-IS-code, and polyglot hosting instead of "another compiled language";
  the duplicated Runtime Features and Optimization Tiers walls are gone, every
  removed detail now lives in the doc it belongs to (tiers in
  `docs/runtime-optimizations.md`, embed flags in `docs/c-embedding.md`,
  sendfile in `docs/http-server.md`), and Core Features is seven one-line
  pillars with links. GitHub repo description updated to match.

## [0.454.0]

### Added

- **`std.clapae`** — a command-line argument parser for Aether modelled on
  Rust's clap, as a builder-style DSL:
  `command("app") { about(...) arg("count") { long_("count"); int_arg() }
  arg("debug") { short(100); flag() } arg("input") { positional(); required() }
  subcommand("run") { ... } }`.
  Key clap-grain features:
  - **Typed arguments** — `flag()` / `string_arg()` / `int_arg()` / `positional()`
    describe an argument's kind (replacing an ad-hoc takes_value/is_flag pair).
  - **Validation at the boundary** — an `int_arg` value is parsed and checked
    during `parse()`, so a non-numeric value is a parse *error* up front, not a
    surprise when read. Typed getters: `get_string`, `get_int` (returns
    `(int, error)`), `get_flag`.
  - **Caller-owned control flow** — `parse` returns
    `(RESULT_OK | RESULT_HELP | RESULT_ERROR, matches, error)`; `-h`/`--help`
    yields `RESULT_HELP` rather than the library calling `exit()`.
  Also: long/short options, inline `-cvalue` and `--opt value`, compound short
  flags (`-dv`), subcommands, required-arg enforcement, and generated `--help`.
  `parse` reads the process argv; `parse_list` parses an explicit list. Pure
  Aether over `std.list`/`std.map`/`std.string`; leak-clean under valgrind.
  Regression test in `tests/regression/test_clapae.ae`.

- **`std.cryptography.tls13_client`** (#1298) — a pure-Aether TLS 1.3 client
  that drives a full handshake over `std.net` TCP, composing the six TLS
  building blocks (x25519, tls13_kdf/ks/hs/cert/record). `connect()` performs
  ClientHello → ServerHello → X25519 ECDHE → the key schedule → decrypting and
  reassembling the server's encrypted flight → **verifying the server Finished
  MAC** → sending the client Finished; `conn_send`/`conn_recv`/`close_conn`
  then exchange encrypted application-data records. Verified end-to-end against
  a live OpenSSL `s_server` (TLS_CHACHA20_POLY1305_SHA256 / X25519): completes
  the handshake and decrypts a real `HTTP/1.0 200 ok` response. The offline
  pieces (transcript accumulator, key derivation, Finished) are validated
  against the RFC 8448 §3 trace in CI.

  `connect()` also **verifies the server's CertificateVerify signature**
  (RFC 8446 §4.4.3) against the leaf certificate's public key — extracting the
  leaf DER from the Certificate message, parsing its SPKI via
  `tls13_cert.parse_certificate`, and checking the signature over the
  transcript hash through Certificate. This proves the peer holds the leaf
  private key and stops a basic key-exchange MITM. The server cert must be
  ECDSA-P256 (the only CertificateVerify scheme wired so far; others fail
  closed).

  **⚠️ Partial authentication:** the CertificateVerify signature is checked,
  but the certificate CHAIN is not validated against a trust store and the
  HOSTNAME is not checked against the cert SAN — a valid-but-untrusted or
  wrong-host cert is still accepted. Chain + hostname validation is the next
  increment; do not rely on this to authenticate a specific server identity
  until it lands.

## [0.453.0]

### Changed

- **Public-key crypto and ciphers moved from `contrib.cryptography` to
  `std.cryptography`** (#1298). The elliptic-curve, RSA, cipher, and
  encoding families — `aes`, `asn1`, `chacha20poly1305`, `des3`, `ed25519`,
  `ed448`, `p256`, `p384`, `p521`, `pem`, `rsa`, `secp256k1`, `sm4`, `x448`
  — now live under `std.cryptography.*`. Update imports from
  `import contrib.cryptography.X` to `import std.cryptography.X`; the APIs
  are unchanged. These are pure-Aether ports with no OpenSSL dependency.

### Added

- **`std.cryptography.tls13_record`** (#1298) — the TLS 1.3 record
  protection layer (RFC 8446 §5.2): per-record nonce (`write_iv` XOR the
  right-aligned big-endian sequence number), TLSCiphertext AAD assembly
  (`0x17 0x0303 len16`), inner content-type append + trailing-zero strip, and
  a directional `RecordCtx` with an advancing sequence number, over
  ChaCha20-Poly1305. `seal_record` / `open_record` / `free_ctx`. Validated
  offline: nonce vectors (seq 0/1/255/256 + a high 64-bit value), a full
  seal→open round-trip recovering content type + plaintext, tamper
  rejection, and multi-record sequence advance; leak-clean under valgrind.
  AES-GCM record support and the socket/transport wiring are later
  increments.
- **`std.cryptography.tls13_cert`** (#1298) — TLS 1.3 CertificateVerify
  (RFC 8446 §4.4.3): builds the signed content (`0x20`×64 || context || 0x00
  || transcript-hash) and dispatches the signature check to ECDSA-P256 /
  Ed25519 (DER ECDSA sig split into `(r,s)` via `std.bignum`). Plus an X.509
  leaf structural parse (subjectPublicKeyInfo extraction) over the `asn1`
  reader. Validated by signing CertificateVerify content with generated
  p256 + ed25519 keys and confirming accept-valid / reject-tampered /
  reject-wrong-transcript. Chain building, hostname/SAN matching, validity /
  EKU policy, and revocation are explicitly out of scope for this brick.
- **`std.cryptography.tls13_ks`** (#1298) — the TLS 1.3 key-schedule
  *driver* (RFC 8446 §7.1): the full secret chain (Early → Handshake →
  Master secrets, per-direction traffic secrets, and record `write_key` /
  `write_iv`) on top of `tls13_kdf`. PSK-less first-cut subset. Validated
  **byte-for-byte against the canonical RFC 8448 §3 trace** — Early/Handshake
  secrets, `c hs traffic` / `s hs traffic`, and the server `write_key` /
  `write_iv` all reproduce the RFC's published values; leak-clean.
- **`std.cryptography.tls13_hs`** (#1298) — the TLS 1.3 handshake message
  codec (RFC 8446 §4): encodes the subset ClientHello (supported_versions
  0x0304, ChaCha20-Poly1305 + AES-128-GCM suites, x25519 supported_groups +
  key_share, signature_algorithms) and parses ServerHello (version / cipher
  suite / key_share, with malformed-input rejection). Validated by
  structural assertions + a full ServerHello round-trip; leak-clean. No
  socket I/O or record layer yet (that is the transport-wiring brick).
- **`std.cryptography.tls13_kdf`** (#1298) — the TLS 1.3 key schedule (RFC
  8446 §7.1): `HKDF-Expand-Label` and `Derive-Secret`, pure-Aether on top of
  `std.cryptography.hkdf`. Validated against the canonical RFC 8448 §3 early-
  secret chain (`Early Secret` and its `derived` secret) and a non-empty-
  context expand-label; leak-clean. The second brick of the pure-Aether TLS
  1.3 subset, after `x25519`.
- **`std.cryptography.x25519`** (#1298) — a **constant-time** X25519 (RFC
  7748) Montgomery ladder over a 10-limb GF(2^255-19) field, ported from
  Bouncy Castle's `X25519` / `X25519Field`. Replaces the previous
  variable-time `contrib.cryptography.x25519` (which routed field math
  through the variable-time `std.bignum` and was explicitly not
  side-channel-hardened). API: `base_point(scalar)`, `scalar_mult(scalar,
  u)`, `agree(scalar, u)` (with a contributory-behaviour zero-check).
  Validated against the RFC 7748 §5.2 scalar-mult vectors and §6.1
  Alice/Bob key-agreement vectors; leak-clean under valgrind. This is the
  first primitive for the pure-Aether TLS 1.3 subset.

### Fixed

- **Struct-name collision `Pt` between `std.cryptography.p256` and
  `.ed25519`** (#1298). Both defined an internal `struct Pt`; Aether structs
  share one global namespace, so importing both modules together (which any
  real TLS client must, to verify both ECDSA and Ed25519 certs) failed to
  type-check. Renamed to `EcPt` / `EdPt`. No API change.
- **`std.cryptography.chacha20poly1305` — Poly1305 tag is now computed in
  constant time** (#1298). The final modular reduction ("freeze") selected
  between `h` and `h - p` with a secret-dependent `if` branch; since the
  accumulator depends on the one-time Poly1305 key, that branch was a MAC
  timing side-channel. Replaced with a branchless masked select. Behaviour
  is unchanged (all RFC 8439 vectors still pass); added a reduction-edge KAT
  (`r=2, s=0, msg=16×0xFF` → tag `03…`) that exercises the freeze path. The
  AEAD tag *comparison* in `aead_open` was already constant-time. Audited as
  part of qualifying ChaCha20-Poly1305 as the TLS 1.3 record cipher.

## [0.450.0]

### Fixed

- **Module consts now respect C scoping** (#1256). A module-level `const`
  lowered to a bare `#define`, so a function parameter or local spelled
  like the const was textually rewritten into the literal (`int SCALE`
  became `int (99)`), and the documented shadowing guarantee failed to
  compile. Consts now lower to file-scope `static const`, which inner
  declarations shadow naturally. Const-of-const initializers, 64-bit and
  string and float consts, match patterns naming consts, and the
  `--emit=lib` const catalog all verified unchanged. Also removed the
  redundant parentheses the match if-chain emitted around equality tests,
  which tripped clang's default -Wparentheses-equality on every match a
  user compiled.

### Added

- **Modules declare their own native link deps with `@link("...")`**
  (#1259). Codegen unions declared flags across the resolved import
  closure into the `// aether-link:` header comment, first-seen order,
  deduplicated, absent when nothing declares. The hardcoded
  `contrib.sqlite` row in the compiler's link table moved into
  `contrib/sqlite/module.ae` itself: the module owns its deps, the
  compiler owns only the mechanism.

## [0.448.0]

### Added

- **Ascon AEAD (authenticated encryption) in `std.cryptography`, pure Aether** —
  the first pure-Aether AEAD in `std`, completing the Ascon port so a
  cross-built binary with no OpenSSL (where AES-GCM stubs out) has a real
  encrypt+authenticate channel. Two modules, both ported from Bouncy Castle
  (same provenance as the shipped Ascon hashes), no externs to OpenSSL or any
  C crypto:
  - `std.cryptography.ascon_aead128` — **Ascon-AEAD128 (NIST SP 800-232**, the
    finalized standard; little-endian, rate 16). `aead_seal` / `aead_open`.
  - `std.cryptography.ascon_aead` — **Ascon v1.2 AEAD (Ascon-128 + Ascon-128a**,
    the NIST LWC winner; big-endian, rate 8/16) for interop with v1.2
    deployments. `aead_seal(algo, …)` / `aead_open(algo, …)` plus `a128_*` /
    `a128a_*` variant-pinned wrappers.
  The seal/open API mirrors `contrib.cryptography.chacha20poly1305`. Both are
  verified against official Known-Answer Test vectors — SP 800-232 KATs from
  the `ascon/ascon-c` reference, and the NIST LWC KATs shipped in bc-csharp —
  spanning every block boundary (empty, partial, full, multi-block AAD), with
  round-trip and tamper-rejection checks, and are leak-clean.

## [0.443.0]

### Added

- **Five more pure-Aether hash/XOF submodules ported from Bouncy Castle** —
  `std.cryptography.ascon_xof128` (Ascon-XOF128, NIST SP 800-232),
  `std.cryptography.dstu7564` (Ukrainian DSTU 7564 / Kupyna, 256/384/512),
  `std.cryptography.isap` (ISAP-Hash), `std.cryptography.photon_beetle`
  (PHOTON-Beetle Hash), and `std.cryptography.sparkle` (Esch-256 / Esch-384).
  No externs to OpenSSL or any C crypto. Verified against Bouncy Castle's
  `LWC_HASH_KAT` vectors (present for ISAP/PHOTON/Sparkle) and BC's DSTU 7564
  digest vectors across all three widths; Ascon-XOF128 is pinned to the
  SP 800-232 reference output (BC's own XOF128 KAT is absent from the upstream
  checkout) and anchored by its verified IV plus a variable-length XOF-prefix
  check. Tests cover rate-boundary inputs and multi-part streaming for each.

### Fixed

- **`std.cryptography.sparkle` (Esch) hashed one block short at every rate
  multiple.** A message that filled the 16-byte rate exactly was absorbed
  eagerly with slim steps in `update`, so `final` then ran on an empty padded
  block with the wrong domain constant — e.g. a 16-byte input produced
  `889f75ad…` instead of the correct `acff841e…`. `update` now holds a
  rate-filling block until more data arrives (matching Bouncy Castle), so the
  last data block gets the big-step + domain-separation treatment. Inputs
  shorter than the rate were unaffected; the bug only appeared at 16, 32, …
  byte lengths. Regression vectors at the rate boundary added.

- **Cross-built binaries now compute a working `std.cryptography` HMAC (was a
  silent stub → fail-open).** The string-API `cryptography.hmac_sha256_hex` /
  `_bytes` were OpenSSL-backed, and on the zig cross path OpenSSL is never
  compiled in, so they stubbed to an empty digest — which made a wrong (and an
  empty) auth token compare equal to a correct one on cross-built agents.
  These now delegate to the **pure-Aether** `std.cryptography.hmac`
  implementation, which needs no libcrypto and produces a byte-identical
  digest, so HMAC works on every target with no sysroot required. Verified on
  a real aarch64 (Raspberry Pi 5) cross build: correct RFC-vector output,
  no OpenSSL linked.
- **`CROSSBUILD_SYSROOT` now enables the *real* OpenSSL/zlib/nghttp2/PCRE2 code
  paths on cross builds, not just links them.** The Tier-2 probe appended
  `-lssl -lcrypto` etc. when a sysroot staged the libs, but never defined the
  matching `-DAETHER_HAS_*` macros or added the sysroot's include dir — so the
  sources still compiled their "unavailable" stub and the `-l` referenced
  nothing. The probe now also adds `-DAETHER_HAS_OPENSSL/_ZLIB/_NGHTTP2/_PCRE2`
  (per lib actually staged) and `-I<sysroot>/include`, so e.g. `sha256_hex`
  returns a real digest on a cross target with a sysroot. Verified on aarch64.
- **The cross "built without OpenSSL" note is now precise.** It distinguishes
  the sysroot-present case (features that the sysroot stages link for real)
  from the no-sysroot case, and no longer implies HMAC is unavailable (it
  isn't — HMAC is pure-Aether).

## [0.442.0]

### Added

- **`os.spawn_proc` / `os.wait` / `os.wait_any` — cross-platform non-blocking
  spawn + reap, including Windows.** The fan-out/fan-in pair a native parallel
  build scheduler needs (spawn up to N ready nodes, wait for whichever finishes
  first, reap it, unblock dependents). Unlike `os.run_pipe` these create no IPC
  back-channel pipe and set no `AETHER_IPC_FD` — which is exactly why they work
  on Windows, where the pipe was the only part that needed the
  `_open_osfhandle`/std-handle-inheritance work that kept `run_pipe` POSIX-only.
  On Windows, `win_launch` is split into a non-blocking `win_spawn`
  (`CreateProcessW`, reusing the existing argv escaping) plus the reap half, with
  spawned handles held in an int-token→HANDLE table (tokens never recycled, so
  Windows PID reuse can't misattribute a reap); `wait_any` uses
  `WaitForMultipleObjects` for a true wait-any. The spawn half of
  `os.run_pipe`/`os.wait_pid` is now wired on Windows too (pipe fd is `-1`
  there); `run_pipe_drain_and_wait` stays POSIX-only. `spawn` is the wrapper name
  everywhere except the reserved actor keyword forced `os.spawn_proc`. Verified
  on Win11/MSYS2: 4×sleep-2 finishing in ~2s (concurrent), completion-order reap,
  exit-code fidelity with spawn-failure distinguishable, `C:\…\a b\c.txt` argv
  round-trip, flat handle count across 300 spawns, and clean coexistence with a
  `run_supervised` Job Object.

## [0.441.0]

### Added

- **Four more pure-Aether hash submodules ported from Bouncy Castle** —
  `std.cryptography.gost3411_2012` (GOST R 34.11-2012 / Streebog, 256- and
  512-bit), `std.cryptography.haraka256`, `std.cryptography.haraka512`
  (Haraka v2 short-input hashes), and `std.cryptography.xoodyak` (Xoodyak
  hash mode over the Xoodoo permutation). No externs to OpenSSL or any C
  crypto. Verified against Bouncy Castle: Streebog's canonical M1/M2 and
  RFC 6986 empty vectors for both widths; Haraka's Appendix-B known-answer
  vectors plus BC's 1000-iteration Monte-Carlo tests (including Haraka-512's
  alternating-halves feedback); Xoodyak against BC's `LWC_HASH_KAT_256`
  vectors. GOST and Xoodyak add split-update streaming tests that cross the
  block/absorb boundary. The Haraka modules reject wrong-length input
  (returning null / "") to mirror BC's exact-32/64-byte contract, rather
  than silently zero-padding a short input or truncating an over-long one.

## [0.440.0]

### Changed

- **`tools/ae.c` split into cohesive translation units** (#1221). The driver
  was one 8,514-line TU, so any edit recompiled all of it, the dominant cost
  in the edit-`ae`-rebuild loop that the cross-compile work touches
  constantly. Three command clusters moved into their own sources reached
  through a new `tools/ae_internal.h`: `ae_cross.c` (the zig cross-compile
  backend), `ae_version.c` (list/install/switch releases), and `ae_repl.c`
  (the interactive REPL) plus `ae_cache.c` (content-hashed build cache,
  publish, GC, and `ae cache`), taking `ae.c` to 6,480 lines. Pure code motion:
  `ae` is already a multi-TU link (`ae_help.c`, `ae_fmt.c`, `ae_bindgen.c`),
  and it spends its wall-clock in `zig cc` / `posix_run` / disk rather than
  its own driver code, so there is no runtime cost. Behavior is unchanged.
- **The `ae` driver now builds per translation unit** (#1221). It was linked
  from one `gcc` invocation over all `tools/*.c`, so the split above would
  not have helped incremental builds: every edit still recompiled the whole
  driver. Each `tools/*.c` now compiles to its own object with `-MMD -MP`
  dependency tracking, so editing one `ae_*.c` recompiles only that object
  and relinks, and editing `ae_internal.h` rebuilds exactly the units that
  include it. The linked binary is identical.

### Added

- **Five pure-Aether hash submodules ported from Bouncy Castle** —
  `std.cryptography.md5`, `.md4`, `.sha1` (classic digests) plus
  `.ascon` (ASCON v1.2 Ascon-Hash / Ascon-HashA) and `.ascon256`
  (Ascon-Hash256, NIST SP 800-232). No externs to OpenSSL or any C
  crypto — the permutations, IVs, endianness (big-endian for v1.2,
  little-endian for the SP 800-232 variant), and padding are ported
  faithfully from BC's `MD5Digest`/`MD4Digest`/`Sha1Digest`/
  `AsconDigest`/`AsconHash256`. Streaming (`new`/`update`/`update_bytes`/
  `final_hex`/`final_bytes`) and one-shot helpers are provided.
  Test coverage matches Bouncy Castle's vectors and extends them to the
  full RFC 1320/1321 / HAC suites, with multi-part streaming that crosses
  the 64-byte block boundary and one-shot-vs-split equality checks; the
  ASCON KATs are pinned to BC's `LWC_HASH_KAT_256` vectors and the
  Ascon-Hash256 empty-message digest to the published SP 800-232 value
  (BC's Hash256 KAT resource is absent from the upstream checkout).

## [0.436.0]

### Added

- **`ae bindgen consts <header.h>`, import C macro constants** (#1245).
  Object-like macros that expand to integer constant expressions, string
  literals, or float literals become Aether `const`s in a generated module,
  with `-I` include dirs, `--match PREFIX` narrowing, and `-o` output.
  The C preprocessor does the evaluation (discovery via `-dM`, full nested
  expansion via a probe), so flag algebra like `(SRI_S_DOWN|SRI_O_DOWN)`
  folds to exactly what C sees; nothing is executed. Macros that are not
  scalar constants are skipped and listed in a comment at the end of the
  generated file, never silently dropped. Ports that hand-copy C flag
  constants (the Aedis/Redis case that motivated the issue) can generate
  them instead.

### Fixed

- **Format bugs in printf-family extern calls are caught again** (#1252).
  The interop lowering cast literal format strings to `void*`, which
  stripped the constant the C compiler's `-Wformat` check reads, so a
  `%s`-vs-int bug compiled silently even against libc's own attributed
  prototype. String literals now pass into `ptr` parameters bare (they are
  `char[]` in C and convert implicitly), `ae` passes `-Wformat` when
  compiling generated C, and `ae build` surfaces compiler warnings the way
  `ae run` already did. The `#line` mapping points the diagnostic at the
  offending `.ae` line; `-Wno-format` via aether.toml cflags opts out.
- **Struct fields named after libc symbols work again** (#1251). A field
  spelled `read` or `write` was renamed to `ae_read` at the member-call
  site but kept its own name in the struct definition, so the emitted C
  referenced a member that does not exist. Fields are struct members, not
  linker symbols: the libc-collision rename no longer applies to them, and
  definition and call site agree. Redis-style vtables (`rio.read`,
  `rio.write`) now port cleanly.
- **Rebuilding `ae` itself invalidates the build cache.** The key hashed
  aetherc's mtime but not the driver's, and the flags ae passes to the C
  compiler are part of the output, so upgrading ae could serve binaries
  built with the old flags until `ae cache clear`. The running executable's
  mtime is now folded into the key.
- **`ae.c` compiles warning-free under MinGW GCC's full `-Wall -Wextra
  -Werror`** (15 findings on the previous release, zero now): misleading
  indentation twice, two POSIX-only globals unused on Windows, nine
  format-truncation sites fixed by sizing derived buffers past their
  sources, and one cross-compile source-path join that now reports and
  skips an overlong path instead of silently truncating it, which could
  have compiled the wrong file.
- **Editing a module under `lib/` invalidates the build cache on Windows**
  (#1235). The lib-dir content walk that feeds the cache key was compiled
  out on Windows, leaving only the directory's own mtime, which does not
  change on an edit-in-place, so every module edit served a stale cached
  binary until `ae cache clear`. The walk now has a native
  FindFirstFileA implementation with the same bounded-depth,
  content-hashing semantics as the POSIX one, and a cross-platform
  integration test guards the behavior end to end.

## [0.435.0]

### Fixed

- **The compiler no longer leaks per parse.** Five leak classes made
  `aetherc lsp`, which reparses on every keystroke, grow without bound: the
  scope-restore sites in codegen truncated `declared_vars` without freeing
  the names declared inside the scope; the postfix parser dropped its
  working copy of every call's function name after `create_ast_node` took
  its own; `parse_binary_expression` leaked the half-built left operand
  when the right side failed to parse; sixteen sites in type inference
  overwrote `node_type` without freeing the previous type; and the extern
  registry's `param_full` arrays were never freed at generator teardown.
  A clean parse and a failing parse now both run leak-free under leaks(1),
  and an import-heavy compile dropped from 287 leaked blocks to 151.
- **`ae build` output no longer stalls on first run on macOS.** The
  Gatekeeper mitigation (ad-hoc re-sign plus quarantine clear) existed but
  was never called; it now runs after every successful executable build,
  before the cache copy, so cached clones are covered too.
- **The profiler's event API paginates.** `profiler_events_to_json`
  accepted an `offset` parameter and ignored it, so every page repeated
  the same events; it now pages back from the newest event.


- **String-literal argument to a call inside `${...}` interpolation.**
  `${id("hi")}` used to be a parse error (the `"` ended the *outer*
  string), and the workaround developers reached for instead,
  `${id(\"hi\")}`, compiled clean but silently evaluated to `""` — no
  error, wrong value. The lexer now tracks interpolation depth and
  treats a `"` (or `\"`) inside `${...}` as opening a real nested
  string literal, so both spellings parse and evaluate correctly.
  `ae fmt` updated to match, so formatting a file using this no longer
  mangles the string. (#1237)
- **The toolchain now compiles on musl (Alpine Linux).** Two portability
  fixes surfaced by the first native aarch64 Alpine build of the toolchain:
  `lsp/aether_lsp.c` captured parser errors by assigning to `stderr`, which
  is not an assignable lvalue on musl (glibc and macOS merely tolerate it);
  the capture now uses fd-level redirection (`dup`/`dup2` onto stderr's fd,
  read back from a `tmpfile`), same behavior on glibc, macOS, and musl, with
  the Windows gating unchanged. `std/net/aether_net.c` used `struct timeval`
  without including `sys/time.h`, which glibc leaks via other headers and
  musl does not. Unblocks static musl builds of downstream binaries such as
  aeo-agent on aarch64.
- **A failed write of generated C now fails the compile.** The write-failure
  guard added in the cleanup sweep printed its error but returned
  compile_source's success code, so a full disk still handed the truncated
  .c file to the C compiler; the guard now returns failure like every other
  error path in that function.
- **`std.pqueue` priorities are 64-bit on every platform.** The C entry
  points took `long`, which is 32-bit on Windows while Aether `long` is 64,
  an ABI mismatch that truncated priorities past 2^31 and only round-tripped
  small test values by calling-convention luck. The C side now uses
  `long long`, matching the `string_to_long_raw` convention; the
  Aether-facing API is unchanged.

### Changed

- **`std.list`'s owned-flag lazy allocation is one helper again.** The
  helper existed but its logic had been open-coded four times at the two
  owned-add call sites; they now call it. Also dropped a dead djb2 hash
  twin and two rwlock-init shims left over after the lazy-lock-init
  removal, and cleaned the last hidden unused-variable and unused-parameter
  warnings in the profiler tools.

## [0.434.0]

### Added

- **`std.set`, an unordered collection of unique strings.** Backed by the
  `std.map` hash table rather than a second one, so lookups are O(1) on
  average and items are copied on insert (the caller's string lifetime does
  not matter). `set.add` reports whether the item was new, and `set.items`
  snapshots the members. Calls on a null set report empty instead of
  crashing. See `examples/stdlib/set-and-pqueue.ae`.
- **`std.pqueue`, a priority queue over `(priority, item)` pairs.** Binary
  heap: push and pop are O(log n), peek and size are O(1). The lowest
  priority value comes out first, so negate the priority for highest-first.
  The queue stores item pointers without taking ownership, it never frees
  them, so heap items you push remain yours to release. Calls on a null
  queue return null rather than crashing.

### Fixed

- **Packages installed by `ae add` are now importable** (system cleanup sweep).
  `ae add <host>/<owner>/<repo>` and `apkg install` clone into
  `~/.aether/packages/<host>/<owner>/<repo>/`, but the module resolver only
  ever probed the flat `~/.aether/packages/<name>/` path, so every package
  installed through the documented workflow failed to resolve with "module not
  found". The nested-layout scan the code intended (a `char search[1024]` that
  was declared, never written, and tombstoned with `(void)search`) is now
  implemented: the resolver walks `<host>/<owner>/` and probes the same
  candidate set it already used for flat packages. Flat layouts keep working.
- **`--emit=lib` no longer fails to link when the module imports `std.config`**.
  `std/config` exported C symbols (`aether_config_get`, `_has`, `_put`, ...)
  that collided with the identically-named embed ABI in
  `runtime/aether_config.c`, which takes a different signature. Because
  `ae build --emit=lib` appends `runtime/aether_config.c` to the link *and*
  links `libaether.a`, any library using `std.config` died with
  `duplicate symbol '_aether_config_has'`. The store's C symbols are now
  `aether_config_store_*`; the documented embed ABI is untouched, and the
  Aether-facing `config.put` / `config.get` / `config.has` API is unchanged.

### Removed

- **The message-batching subsystem** (`runtime/memory/aether_batch.{c,h}`).
  `batch_send()` looped over the batch calling a placeholder `actor_send()`
  that was a no-op, so every batched message was silently discarded, and that
  non-static `actor_send` symbol shipped in `libaether.a` for any translation
  unit to link against by accident. It had no production caller; its only
  consumer was a test that is wired into no build target and that never called
  `batch_send()` at all (it simulated the send with a counter increment, so the
  "1.78x speedup" advertised in the header measured nothing). Removed along
  with that orphaned test.
- **`aetherc run`**, which could never succeed. It gated on `runtime/actor.c`,
  a file that has not existed since the runtime was split into
  `runtime/actors/`, so every invocation failed with "Could not locate Aether
  runtime files" after already writing a stray `<input>.ae.c` next to the
  user's source. `aetherc` now explains that it is the compiler front end and
  points at `ae run`, which is the working, documented entry point. The dead
  `compile_c_to_exe` helper it was the sole caller of is gone.
- **`runtime/io/`**, an orphaned poller hub duplicating the active pollers in
  `runtime/scheduler/`. Nothing included it, and the install step carried an
  `rm -rf` to hide it from consumers that scan for linkable sources; deleting
  the sources removes the need for that workaround.
- **The duplicate collection implementations** `aether_hashmap`, `aether_set`
  (the old vtable-based one) and `aether_vector`. They shipped in every build
  and defined a second `HashMap` type with the same name as the live one in
  the same directory, while `std.map` and `std.list` already covered their
  jobs. No Aether module bound to them and no C caller used them; their only
  consumers were tests under `tests/stdlib/`, which no build target ever ran.
  The genuinely missing capabilities they hinted at, Set and PriorityQueue,
  are now shipped as real modules (see Added) built on the live hash table
  instead of a parallel one.

## [0.428.0]

### Changed

- **`std.worker` now runs jobs on a bounded pool instead of a thread per job**
  (#1205). `worker.run` previously spawned (and detached) a fresh OS thread for
  every job, so a UI app firing 30 concurrent requests spawned 30 threads. It
  now submits to a lazily-started pool of reusable worker threads (size defaults
  to the core count, clamped to [2, 32]; set it with `worker.pool_size(n)`
  before the first `run`): N concurrently-blocking jobs queue job N+1, the
  standard SwingWorker-style trade. `worker.run_detached` keeps the fresh-thread
  behavior as the escape hatch for a job that must never queue, and the
  cooperative / `AETHER_NO_THREADING` synchronous fallback is unchanged. Process
  exit abandons in-flight and queued jobs (instant, as the pre-pool
  thread-per-job model behaved), so a job blocked in user work never hangs exit;
  `worker.pool_shutdown()` joins and frees the pool for deterministic teardown
  when the jobs are known to finish.
- **The HTTP/2 concurrent-dispatch pool is now the shared `std.worker` pool**
  (#1205). Per-stream h2 dispatch (`server.set_h2_concurrent_dispatch(n)`) used
  to run on a second, h2-private thread pool duplicating the worker pool's job.
  It now submits stream handlers through `std.worker`, so one process-wide pool
  serves both `worker.run` and every h2 connection on every server, keeping the
  OS thread count constant instead of standing up two independent pools. The h2
  worker count still sizes that shared pool; behavior and the empirical
  parallelism guarantee are unchanged.

## [0.424.0]

### Fixed

- **`std.json` now reads 64-bit integers exactly** (#1204). `json.get_int`
  returned a 32-bit `int`, so any JSON number above 2^31-1 (a 10-digit ID, a
  large byte-count) was silently corrupted on read with no diagnostic, even
  though construction (`json.from_int`) already accepted the full int64 range.
  The parser now stores integer-valued numbers in a dedicated int64 slot
  (previously they were parsed into a `double`, lossy past 2^53), a new
  `json.get_long(value) -> long` reads the exact int64 value, and
  `json.get_int` now clamps to `+/-2147483647` on overflow instead of
  truncating. Large integers also round-trip through parse/stringify exactly.

## [0.421.0]

### Added

- **`ae build --target=<triple>` now cross-compiles for FreeBSD** (extends the
  zig cc backend, #1105). Adds `x86_64-freebsd` / `aarch64-freebsd` (+ `amd64` /
  `arm64` aliases) as the first Tier B targets: unlike the self-contained Tier A
  targets (macOS/Linux, whose libc zig bundles), FreeBSD needs a version-matched
  base sysroot — supplied via `AETHER_SYSROOT` (a `bases/<cpu>-freebsd<ver>/`
  tree from aether-crossbuild). Without it, the build reports a guided error
  naming the fetch script. The link is done explicitly against the base's CRT
  objects and real `libc.so.7` (zig's bundled FreeBSD-14 libc can't satisfy a
  15 base's `__libc_start1`, and the sysroot's `libc.so` is an absolute-path
  linker script). Verified end-to-end: a `println` program cross-built on Linux
  ran on a FreeBSD 15.0 box. Scope matches #1105 — dependency-free / libc-only
  programs; a program pulling `std.http` / `std.cryptography` additionally needs
  those third-party libs built into the sysroot (aether-crossbuild's
  `provision.sh`), untested through this path yet. Requires zig on `PATH`.

## [0.420.0]

### Added

- **Swappable allocator convention (`std.alloc`) + tracking allocator
  (`std.tracking`)** (#1045, #1049). An allocator is now an explicit handle,
  never an implicit ambient context: `alloc.system()` is the default,
  `alloc.of_arena(a)` allocates through a `std.arena`, and `alloc.raw` /
  `resize` / `release` allocate raw bytes through any handle. Collections gain
  an `_in` constructor that routes their own memory through a given allocator;
  `std.list` (`list_new_in`) is the first, with `map` / `bytes` / `strbuilder`
  to follow. On top of this, `std.tracking` wraps any allocator and records
  every live allocation, so a leak becomes a deterministic in-test assertion
  (`tracking.count(t) == 0`) rather than something only a coarse external CI
  gate can catch, which matters because Aether has no GC backstop. Existing
  code is unchanged: the default constructors keep the cap-aware system path.
  See `docs/allocators.md`.

## [0.419.0]

### Added

- **`aetherc` emits a `// aether-link:` header from the resolved import graph**
  (#1202). The first line of the generated C now names the native libraries the
  program's imports pull in, so a downstream build linking the `.c` recovers
  them generically (`AE_LINK="$(sed -n 's|^// aether-link:||p' out.c)"`) instead
  of rediscovering the list by `undefined reference` per platform. Written by
  the same resolution that compiled the file (can't disagree with what was
  compiled) and travels with the artifact. A truthful `{module → libs}` table
  covers openssl (`std.net`/`std.http.client`/`std.cryptography`), pcre2
  (`std.regex`), nghttp2 (`std.net` + h2), zlib (`std.zlib` + `http/middleware`),
  sqlite (`contrib.sqlite`), audio (`std.audio`); matched against the transitive
  import closure, de-duplicated, stable order. Only import-introduced libs
  appear — the runtime baseline stays with `ae cflags --libs`. Answers
  `emit-link-requirements-from-import-graph.md`.

### Fixed

- **Windows: `-DPCRE2_STATIC` when linking static libpcre2-8** (#1200). Without
  it, MinGW builds of `std.regex`-using programs failed to link against the
  static PCRE2 import symbols.

## [0.417.0]

### Fixed

- **`list.get` / `list_get_raw` no longer segfaults on an invalid list
  pointer.** The accessor read `list->size` / `list->items[index]` without
  validating the pointer, so a dangling, type-confused, or freed list — a
  struct with the wrong `_kind_magic`, a reused struct, or a small int
  intptr-cast to `ptr` — crashed deep inside the accessor instead of
  returning a safe NULL. It now applies the same `_kind_magic` +
  low-address discriminator `aether_value_is_list` uses, so a bad pointer
  yields `(null, "")` (out-of-range index behaviour is unchanged). Surfaced
  by an aeb build whose generated code passed such a pointer to `list.get`.

## [0.416.0]

### Fixed

- **Imported `enum` and `sum` types are now emitted** (#1194). The module-merge
  pass cloned imported `struct` / `bitstruct` / distinct definitions into the
  consumer's program AST but not `enum` or `sum` ones, so an imported enum used
  by name failed (`Undefined variable 'Color'`) and an imported sum type failed
  (`unknown type name 'Shape'` in the generated C). Both are now merged (with
  dedup), mirroring the struct/bitstruct arms.
- **A local variable named the same as its own module resolves correctly**
  (#1194). The member-access typechecker took its namespace-qualified-constant
  branch whenever the base matched a visible namespace, with no precedence for a
  same-named in-scope value — so a module named `flags` whose body had a local
  `flags` mis-resolved `flags.field` as a `flags_field` const lookup
  (`module 'flags' has no export 'field'`). A local now shadows the namespace.
  Not bitstruct-specific (reproduced with a plain struct); surfaced by the
  imported-bitstruct ask's verbatim repro.

## [0.415.0]

### Fixed

- **Imported-module `bitstruct` typedefs are now emitted** (#1192). A
  `bitstruct` declared in an imported module was referenced by the consumer's
  generated C (accessor prototypes/return types) but its backing typedef was
  never emitted — `error: unknown type name 'PropertyFlags'` — because the
  module-merge pass cloned imported `struct`s but not `bitstruct`s. Reported
  while porting MicroQuickJS, whose packed property-flags word wanted a
  layout-exact bitstruct in the module that owns the layout. Answers
  `asks/imported-module-bitstruct-emission.md`.

## [0.414.0]

### Added

- **Version-stamped SDK: a compile-time header macro + include-tree sidecar**
  (#1189). An installed SDK tree could not identify itself. Now a generated,
  dependency-free `runtime/aether_version.h` exposes `AETHER_VERSION` (string)
  plus `AETHER_VERSION_MAJOR/_MINOR/_PATCH` and `AETHER_VERSION_NUM`
  (`MAJOR*1000000 + MINOR*1000 + PATCH`) for `#if`-based gating
  (`#if AETHER_VERSION_NUM < 390000 → #error`), and the install writes an
  `include/aether/VERSION` sidecar mirroring `lib/aether/VERSION`. All derive
  from the Makefile's `$(VERSION)` in lockstep with `aetherc --version` — no
  hand-maintained constant. Answers the version-stamp ask.

## [0.413.0]

### Added

- **`std.worker` — run blocking work off the loop thread, deliver the result
  back on it** (#1184). The primitive every GUI toolkit has (Qt `QThread`+signal,
  GTK `g_thread` + `g_idle_add`, Swing `SwingWorker`), made toolkit-agnostic:
  `worker.run(work, done)` runs the `work` closure on a background thread (a
  blocking `send_request` / `fs.read` / subprocess is fine there) and, when it
  returns a `ptr` result, delivers that result to the `done` closure **back on
  the thread that owns the app's event loop** — so a GUI callback no longer
  freezes the window. Getting onto the loop thread is the host's job: a GUI host
  installs a poster once (`set_main_poster`, wrapping `g_idle_add` /
  `dispatch_async` / `PostMessage`); with none installed, completions queue and
  the app pumps them with `worker.drain()` on its own loop thread (the headless /
  test path). Blocking IO runs on an off-scheduler OS thread by necessity — a
  blocking actor handler starves its cooperative scheduler core, the same reason
  `std.http`'s h2 server runs handlers on its own pthread pool; on the
  cooperative / `AETHER_NO_THREADING` build `work` runs synchronously while the
  same completion contract holds. Surface: `run`, `run_detached`,
  `set_main_poster`, `deliver`, `drain`, `pending`. Answers
  `asks/ui-async-worker-for-blocking-io.md`.

- **`std.audio` — audio playback backed by vendored miniaudio** (#1180, #1183).
  A playback-tier audio API mapping Go beep's pull-based model: a source is the
  unit of playback; `load_wav` decodes bytes into a source (wav / mp3 / flac via
  miniaudio's built-in decoders) and `play` / `pause` / `stop` / `seek_ms` /
  `volume` / `position_ms` / `duration_ms` / `channels` / `sample_rate` operate
  on it. Real device output (ALSA / PulseAudio / CoreAudio / WASAPI, auto-selected
  by miniaudio), with automatic fallback to miniaudio's null backend when no
  device initialises (headless CI) — behaviour stays deterministic and testable
  either way, and `is_null_backend()` reports which path won. The device/decode
  layer is C by necessity (a real backend pulls samples on a realtime thread no
  Aether code may run on); the vendored single-header `std/audio/miniaudio.h`
  (public domain / MIT-0) is compiled once behind the FFI. See
  `docs/cross-references/audio.md`.

## [0.409.0]

### Added

- **`ae build --target=<triple>` cross-compiles via a `zig cc` backend** (#1105).
  Builds a foreign-OS/arch binary using zig as a self-contained cross toolchain:
  zig bundles each target's libc, headers, and linker, so the Aether runtime and
  standard library compile straight from source for the target with no cross-gcc
  or sysroot. The platform backend (`epoll` vs `kqueue`, `spawn_sandboxed_linux`
  vs the BSD/stub path) is chosen by the `__linux__` / `__APPLE__` macros zig
  predefines, so one source set serves every target. Supported triples:
  `aarch64-macos`, `x86_64-macos`, `aarch64-linux`, `x86_64-linux`. The runtime
  and stdlib are compiled from source, archived, and linked on demand (so a user
  function may share a name with an unreferenced runtime global, exactly as a
  native `-laether` link allows). Cross binaries are built without OpenSSL / zlib
  / nghttp2 / PCRE2, so features needing them (HTTPS/TLS, hashing, base64, regex,
  compression, HTTP/2) report errors at runtime like a native build lacking those
  libraries; `ae build` prints a note and builds anyway. Executables only for now
  (`--emit=lib`/`--emit=both` are rejected), POSIX host. Native builds are
  unchanged. See `docs/build-system.md`.

### Fixed

- **`MANIFEST` now lists the collections and reactor sources.** The authoritative
  link-suitable source list (`build/MANIFEST`, #329) was generated from
  `RUNTIME_SRC` + `STD_SRC` only, silently omitting `COLLECTIONS_SRC` and
  `STD_REACTOR_SRC`, which are part of `libaether.a`. A downstream consumer
  linking from MANIFEST would fail to resolve `std.collections`
  (hashmap/vector/set/...) symbols. Both source groups are now emitted.

### Documentation

- **Iterative traversal/free for deep `*Struct` chains** (#1070). The language
  reference taught recursion for walking and freeing self-referential `*Struct`
  chains (the `*ErrChain` example). Aether does not turn tail calls into loops,
  so a recursive walk/free spends one C stack frame per cell and overflows the
  stack on a long chain (verified: a 300k-cell chain segfaults recursively).
  The reference now documents the O(1)-stack iterative spine walk for both
  traversal and free (capturing each successor before the free), with a note on
  the overflow risk, mirroring what `docs/sequences.md` already says for
  `*StringSeq`. Locked by a regression test that builds and iteratively
  walks/frees a 300k-cell chain.
## [0.408.0]

### Added

- **`std.hash` — SipHash-2-4 (keyed, hash-flood resistant)** (#1174). Adds the
  keyed PRF alongside the module's non-cryptographic hashes: a 128-bit key over
  arbitrary bytes yields a 64-bit tag, the standard defence for hash tables
  exposed to adversarial keys (hash-flooding DoS). Ported from C3's
  `std::hash::siphash` and verified against the reference test vectors.

## [0.406.0]

### Added

- **Five foundational modules ported from the C3 standard library** (#1167,
  #1169). Implements the high-value gaps from
  `docs/cross-references/c3-stdlib-gaps.md` by porting from C3 *source* (not
  transplanting its generated C), staying pure Aether, with test vectors taken
  from C3's own unit tests. Re-expressed in Aether's idiom — free functions +
  structs/tuples rather than C3's generics/methods/operators.
  - **`std.encoding`** — `hex` (RFC 4648 §8), `base32` (§6), `base64` (§4), and
    `csv` field-splitting. `base64` moved here from `std.cryptography` (its
    correct home — an encoding, not cryptography); `base64_encode_url` renamed to
    the accurate `base64_encode_padded`, and `cryptography.random_base64` rebuilt
    on top of it.
  - **`std.time`** — `DateTime` over Unix-epoch seconds (UTC) with exact,
    dependency-free civil↔epoch math (Hinnant's algorithms, no libc timezone
    state): `now`, `from_civil` / `from_unix`, ISO-8601 format / parse,
    `add_*` / `diff` / ordering, leap-year and day-of-week.
  - **`std.sort`** — in-place ascending sort (shell sort, Ciura gaps) + binary
    search over the concrete numeric array types (`intarr` / `longarr` /
    `floatarr`). Concrete-types-first by design, not C3's generics.
  - **`std.deque`** — fixed-capacity ring buffer / double-ended queue of `long`:
    O(1) push/pop at both ends, overwrite-oldest-on-full (sliding window), value
    semantics.
  - **`std.hash`** — non-cryptographic FNV-1a 32/64 and MurmurHash3 x86 32-bit,
    verified against canonical reference vectors.

### Fixed

- **`T!` auto-wrap: a single-child `return <heap-expr>` in a result function is
  now wrapped correctly** (surfaced by the C3 ports, #1169). A bare
  `return <heap-expr>` in a `T!` (result) function was mis-classified as a
  tuple pass-through instead of the `(<expr>, "")` success auto-wrap, so a
  heap-string result could cross a module boundary without its ownership
  tracked — a cross-module leak. Fixed in the codegen return-heap classifier.

- **`std.longarr.get()` no longer truncates 64-bit values to 32 bits.** An
  inferred `-> {` return whose error path used an int literal (`return 0, "err"`)
  pinned the value slot to 32-bit `int`, so a stored `long` came back with its
  high 32 bits lost. The signature is now explicitly `-> (long, string)`. Found
  during the C3 ports (the sibling `floatarr` was unaffected — its error paths
  use `0.0`).

## [0.404.0]

### Added

- **Error-unification arc: the `T!` result type** (#1155, #1156, #1161, #1162,
  #1163 — landed in phases over 0.402–0.404; design in
  `docs/error-unification.md`). Unifies Aether's two-slot `(value, err)` fallible
  convention into a single first-class result type, `T!`:
  - A `T!` function returns a bare `return v` for success (auto-wrapped to
    `(v, "")`) or `return v, "err"` for failure; `expr!` propagates a failure
    from a callee, and `or { ... }` handles it. The stdlib's two-slot fallible
    signatures were migrated to `T!` (P1, #1155).
  - **Unconsumed `T!` results are a compile error** (P2, #1161): a fallible call
    whose error slot is ignored is rejected with guidance, so a failure can't be
    silently dropped.
  - **Tuple-payload `T!` is rejected at parse time** with guidance (P1.5, #1156):
    the result carries a single payload, keeping the boundary shape unambiguous.
  - **`fault` declarations** (P3, #1162): declared fault values over `const char*`,
    so an error can be a named, comparable constant (`err == fs.NotFound`) rather
    than a bare string.
  - **`??` accepts a fallible `T!` left side** (P1.3, #1163): the coalescing
    operator now takes a `T!`, yielding the value on success or the right-hand
    fallback on failure.

### Fixed

- **`s = f() or { … }` now heap-tracks its value and frees the discarded error
  slot.** Two leaks on the `or`-handled path: the bound success value wasn't
  registered with the heap tracker (so a heap string leaked at scope exit), and
  the handled error slot was never freed. Both fixed in codegen. (Earlier in the
  arc, 0.402.0, heap-backed `string?` locals also gained scope-exit frees.)

## [0.401.0]

### Fixed

- **`string? == string?` now compares content, not pointers.** Two optionals
  wrapping distinct string objects with equal bytes compared **unequal** — a
  silent wrong answer (and, since a string's `.val` carries an AetherString
  header, the raw `==` was not even a meaningful C comparison; in some shapes
  it failed to compile). Now dispatched through `_aether_safe_str` + `strcmp`,
  exactly like an ordinary string comparison; the scalar case (`int?`, …) is
  unchanged. Found while scoping the error-unification design
  (`docs/error-unification.md` §2.2).

### Changed

- **Nested optionals `T??` are now rejected at parse time.** A double optional
  parsed (as `ae_opt_ae_opt_<T>`) but the rest of the compiler reasons only one
  presence layer deep — `none`/wrap coercion, `== none`, and narrowing all
  assume a single layer — so `int??` miscompiled silently. It is now a clear
  error (`nested optional \`T??\` is not supported …`) in return, `let`, and
  parameter positions, matching C3, whose `type_add_optional` likewise refuses
  to nest. The `??` null-coalescing operator, `?.` chaining, and single `T?`
  are unaffected. No in-tree code used `T??`.

## [0.400.0]

### Added

- **`fs.pread_into` and little-endian `std.bytes.cursor` readers** (#1102), for
  copy-free fixed-size block reading of binary files. `fs.pread_into(file, buf,
  len, offset)` reads up to `len` bytes at `offset` straight into an existing
  `std.bytes` buffer (clamped to its capacity), sets the buffer's length to the
  count read, and returns `(n, err)` with the same EOF (`n == 0`) / short-read
  (`0 < n < len`) / I/O-error (`err != ""`) distinction as `fs.pread`. A
  fixed-size block reader reuses one buffer across the whole file instead of
  allocating a fresh string per block; the packed integers are then read in
  place (`bytes.get_le64`) or walked with a cursor. `std.bytes.cursor` gains
  `read_le_u16` / `read_le_u32` / `read_le_u64` alongside the existing big-endian
  readers (same end-of-buffer contract: returns `-1` with the cursor unchanged),
  so little-endian on-disk formats stream as cleanly as big-endian wire formats.
  Exercised by `tests/regression/test_fs_pread_into.ae`.

## [0.398.0]

### Fixed

- **`x = f() or { ... }` no longer yields an uninitialized value — the block's
  last statement is now the handler's value.**

  ```aether
  b = f(true) or { -1 }          // was: silent garbage (observed 1396619984)
                                 // now: -1

  c = f(true) or {
      println("recovering: ${err}")
      -2                          // multi-statement blocks work too
  }
  ```

  Block handlers had been designed as must-exit (`or { return … }`) and the
  value-yielding form was neither implemented nor rejected: the trailing
  expression was emitted as a discarded statement and the result local was
  read **uninitialized** on the error path — a silent miscompile. Found while
  scoping the `T?`/`(value, err)` error-unification design, the same way
  `defer catch`'s groundwork found the `expr!` defer leak.

  The typechecker now also **rejects** a block that neither yields a value of
  the right type nor exits (`return` / `panic` / `break` / `continue`), since
  that shape can only ever produce the uninitialized read. The early-return
  form and the bare-expression default (`or -1`) are unchanged.

### Upgrade notes

If an `or { }` block previously ended with something that is neither a value
nor an exit (a trailing `if`, an empty block), it now fails to compile instead
of silently reading garbage — end the block with the fallback value or a
`return`. No in-tree code needed changing (the only two block uses both end in
`return`); code that compiled into the uninitialized read could not have been
relied upon.

## [0.397.0]

### Added

- **Contracts now fold at compile time — a provably-violated contract is a
  build error, not a deferred panic** (design: `docs/contract-folding.md`).

  ```aether
  divide(a: int, b: int where b != 0) -> int { return a / b }

  divide(10, 0)     // NEW: compile error — "precondition violation at compile
                    // time: b != 0 in divide — this call's constant arguments
                    // can never satisfy it"
  divide(10, n)     // n is runtime → runtime check, exactly as before
  ```

  Two tiers. At a **definition**, a `requires`/`where`/`ensures` predicate that
  is decidably false with no arguments substituted (`requires false`, or
  `requires MIN <= MAX` after a const refactor staled it) can never be
  satisfied, so it errors at the clause. At a **call site**, the constant
  arguments are substituted for the parameters and a decidably-false predicate
  errors at the call — trait-bound/concepts-like checking from the contract
  syntax you already wrote, with no macro system.

  The evaluator is deliberately narrow and conservative: literals, top-level
  `const` names, enum members, arithmetic (**exact in int64** — a double-based
  fold would mis-judge `x == 9007199254740993`-class comparisons), comparisons
  and `&& || !`. It **never evaluates calls** — the const layer is
  whitelist-only precisely so compile-time evaluation can't synthesize
  fs/net calls past the `--emit=lib` capability gate — and anything it cannot
  decide keeps today's runtime check with no diagnostic. `when` arms are
  pruned before the typechecker runs, so platform-dead calls cannot
  false-positive.

  Check **elision** got smarter as a side effect: the constant-true fold now
  resolves `const` names and enum members (`requires cap > MIN_CAP` elides),
  where it previously handled only literals. Short-circuit folding is
  asymmetric on purpose: `true || x` elides (the runtime would skip `x` too),
  but `x || true` with unknown `x` keeps the runtime check, since evaluating
  `x` may carry a side effect — the documented pre-existing guarantee, now
  load-bearing in the evaluator.

### Upgrade notes

Code that compiled and panicked at runtime — or never executed — now fails to
build if a contract violation is provable from constant arguments:

```aether
if never_true() { r = divide(x, 0) }   // compiled before; rejected now
```

The tree contains ~32 contract clauses total, so the practical blast radius is
approximately zero — this window is exactly why the change ships now rather
than after contracts proliferate. If a provably-violating call is genuinely
intended to be unreachable, route the constant through a runtime variable
(`z = 0; divide(x, z)`); only constant arguments participate in folding.

Exactly one in-tree caller needed that treatment: the `where_clause`
integration probe, which deliberately calls `divide(10, 0)` to assert the
runtime panic message. It now routes the zero through a runtime variable (with
a comment saying why) — a worked example of both the break and the fix.

Compile-time contract errors fire even under `--no-contracts`: that flag
removes runtime *checks*; it does not suppress compile-time correctness
findings.

## [0.396.0]

### Added

- **`defer try` / `defer catch` — cleanup that runs on one outcome only** (#1140).

  ```aether
  defer       cleanup()    // always — every exit (the pre-existing form)
  defer try   commit()     // only when the function returns SUCCESSFULLY
  defer catch rollback()   // only when the function returns an ERROR
  ```

  "Error" means a non-empty error slot — Aether's `(value, err)` convention, and
  `T!`, which is the same shape. Together they give the transactional shape in
  three lines: acquire, register the rollback, register the commit, and let any
  error path bail without the acquire leaking and without a half-built result
  being published:

  ```aether
  acquire(path: string) -> (ptr, string) {
      p = malloc(SIZE)
      defer catch free(p)               // bailed — release it
      cfg, err = parse(path)
      if err != "" { return null, err }  // ...and `p` is freed on the way out
      return p, ""                       // succeeded — the caller owns it
  }
  ```

  The alternative is an `if err != "" { free(p); return }` at every early return,
  and a leak at the one you forget.

  LIFO ordering is unchanged, and the conditional defers **interleave with the
  unconditional ones by registration order** rather than being hoisted into
  separate groups. Cost is one predictable compare on the return path — and zero
  where the outcome is statically known, since an `expr!` propagation is always an
  error exit and a bare `return v` is always a success exit, so in a `T!` function
  no guard is emitted at all. There is still no runtime defer stack: bodies are
  emitted inline at each exit, exactly as a plain `defer` already was.

  Using either form in a function that **cannot** fail is a warning rather than
  silence — a `defer catch` there would never fire, and a `defer try` is just a
  plain `defer`, so in both cases the code does not do what it says.

## [0.394.0]

### Fixed

- **`expr!` propagation no longer skips the enclosing function's `defer`s — a
  silent memory leak on every error path through a `T!` function.**

  In a function returning `T!`, `expr!` propagates a failure by emitting a
  `return` from inside a GCC statement-expression. That `return` ran **none** of
  the cleanup that every other `return` site runs: not the user's `defer`s, and
  not the *synthetic* cleanup carriers the compiler pushes itself (heap-string,
  `*StringSeq`, and struct-destroy exit frees). So:

  ```aether
  outer(fail: bool) -> int! {
      p = malloc(65536)
      defer free(p)          // ran on the success path — NOT on the `!` path
      v = inner(fail)!       // propagates: `p` leaked, silently
      return v
  }
  ```

  The leak was invisible in two ways. It only occurred on the **error** path, and
  the *synthetic* half of it needed no `defer` in the source at all — a `T!`
  function that merely built a heap string and then propagated an error leaked
  that string, with nothing in the code to suggest cleanup was owed. Measured on
  the regression test: **6.3 MB lost across 97 blocks** before the fix, zero
  after (`valgrind --leak-check=full`).

  The propagation path now drains the full defer stack and the in-flight try
  frames (issue #501), exactly as the ordinary `return` path does.

  Latent rather than actively burning anyone: nothing in `std/` or `contrib/`
  uses `T!` yet, and there was no test combining `T!` with `defer` — which is
  precisely why it survived. `tests/regression/test_expr_bang_defer_drain.ae`
  now covers all three shapes (one defer, several defers, and compiler-synthesised
  cleanup with no user `defer` at all).

### Upgrade notes

This release makes `expr!` propagation run the cleanup it always should have run:
a `defer` (and the compiler's own heap-tracked cleanup) now fires on the `!`
error path, where previously it was skipped entirely.

If your project **worked around the leak by manually releasing the resource on
the error path** — for example an `or { free(p); ... }` handler, or a manual
`free` in the caller — that release is now a **double free**, because the callee
frees it too. This is the only way the fix can break code that previously worked.

**Recommended pre-upgrade play:**

1. Grep for `T!`-returning functions that both hold a resource (`malloc`, an fd,
   a C handle) and use `expr!` to propagate: `grep -rn '\->.*!' --include=*.ae`.
2. In each, check whether the *caller* or an `or { … }` handler also releases
   that resource. If so, delete the manual release — the `defer` now owns it.
3. Run the suite under ASan/Valgrind (`make test-asan`, `make docker-ci`); a
   double free shows up immediately and loudly.

- **`contrib/host/tcl` now builds against Tcl 9.0** (Homebrew's `tcl-tk` on macOS; Linux distros still ship 8.6, which is why this only broke locally). Tcl 9.0 removed `Tcl_Eval` as an exported function and left behind a function-like macro over `Tcl_EvalEx`, so the bridge's `g_tcl.Tcl_Eval(...)` dlsym-table calls expanded into references to a non-existent `g_tcl.Tcl_EvalEx` member (`error: no member named 'Tcl_EvalEx' in 'struct (unnamed…)'`). Same shape as the `Tcl_GetStringResult` / `Tcl_GetString` breakage already handled in that file, so it takes the same fix: `#undef` the macro, resolve the lowest-common-denominator export that exists in **both** 8.6 and 9.0 (`Tcl_EvalEx`), and recompose `Tcl_Eval` from it in a local helper. An `#undef`-only fix would have compiled but failed at *runtime* on 9.0, where the `Tcl_Eval` symbol genuinely no longer exists to dlsym. Also widened the dlsym prototypes for `Tcl_EvalEx` / `Tcl_NewStringObj` / `Tcl_WrongNumArgs` from `int` to `Tcl_Size`, matching the 8.7+/9.0 headers (a latent call-ABI mismatch: `ptrdiff_t` params were being passed 32-bit `int` args), with an `int` fallback typedef for 8.6, which has no `Tcl_Size`.

## [0.392.0]

### Added

- **`bitstruct` — a layout-exact, endianness-independent replacement for C
  bitfields** (#1132).

  ```aether
  bitstruct DnsFlags : uint16_t {
      qr:     bool 15          // one bit
      opcode: int  11..=14     // inclusive range
      rcode:  int  0..<4       // exclusive range — same bits as 0..=3
  }
  ```

  A bitstruct is a named bit layout over one unsigned integer. It **never lowers
  to a C bitfield**; it lowers to shift-and-mask on the backing word. That is the
  point: a C bitfield's signedness, allocation order, and straddling are all
  implementation-defined, and gcc in particular gives `int x : 3` a *signed*
  representation — so a stored `0b111` reads back as `-1`. Aether's extern-struct
  bitfields (`name: type : N`) have exactly that flaw today and require every
  unsigned read to be hand-masked. A bitstruct field cannot have it: the backing
  word is unsigned and the mask is applied after the shift, so there is nothing
  to sign-extend from.

  The rules, each of which keeps the layout exact: the backing type is
  **mandatory** and must be `uint8_t`/`uint16_t`/`uint32_t`/`uint64_t` (naming the
  storage is what fixes its width and signedness); bit positions are explicit;
  ranges may be spelled inclusively (`1..=3`) or exclusively (`1..<4`) using the
  same tokens as match-range labels, so the source says which it means rather than
  the reader having to remember a convention; overlapping fields are an error
  unless the bitstruct is annotated `@overlap`; a range that overruns the backing
  integer is an error; writing a field never disturbs its neighbours; and a
  bitstruct is strictly nominal — crossing to or from the backing integer is an
  explicit `as`.

  Bit layout and byte order stay **separate concerns**: a bitstruct says which
  bits, and `std.mem`'s endian-explicit accessors (`mem.get_u16_be`, …) say which
  byte order. There is deliberately no `@bigendian` annotation and no hidden
  byte-swapping — the swap is always visible in the source.

### Changed

- **FFI: a tuple-typed value is now accepted at a tuple-typed extern
  parameter** (#1062), not only a parenthesized tuple literal. A variable
  holding a tuple, or the result of a tuple-returning extern passed straight
  through, crosses the boundary by value because it already is the synthesized
  `_tuple_*` struct in the generated C. This lets FFI pass-through chains like
  `export_image(load_image(path))` skip the destructure-and-re-parenthesize
  boilerplate that scaled with the struct's field count at every call site. A
  value whose tuple shape does not match the parameter, or a non-tuple value,
  is still rejected at type-check. Exercised by
  `tests/integration/extern_tuple_var_passthrough/`.

### Documentation

- **c-interop.md: bind a C `bool` return with `-> byte`, not `-> bool`.**
  Aether's `bool` maps to C `int`, so declaring a C function that returns C's
  one-byte `_Bool` as `-> bool` reads a full `int` and picks up three bytes of
  stack garbage past the result (a success can read back as `-255`). `-> byte`
  reads exactly the one byte the ABI wrote.

### Fixed

- **FFI tuple-parameter type matching is now exact, and struct-pointer tuple
  elements name a valid C type.** Two follow-ups to the #1062 tuple-value
  parameter support, both surfaced by an adversarial review of that change:
  - The value-form match compared only `TypeKind`, so a tuple value whose
    element was an aliased scalar of the same kind (for example `(int, int)`
    into an `(int8_t, int8_t)` parameter) type-checked and then failed with an
    opaque C compile error. It now matches on the element's emitted C type name,
    the same key codegen uses to name the `_tuple_*` struct, so the mismatch is
    reported cleanly at type-check; matching aliases still pass.
  - A tuple containing a struct-pointer element (`(*Node, int)`) generated the
    invalid C identifier `_tuple_Node*_int`, so any use of such a tuple (extern
    parameter, extern return, or a tuple literal) produced uncompilable output.
    The tuple-typedef namer now sanitizes non-identifier characters the same way
    the optional-type namer already does, yielding `_tuple_Node__int`.

## [0.389.0]

### Fixed

- **A closure created inside another closure's body now captures.** A
  closure/callback written lexically inside another closure's body failed to
  capture that enclosing closure's locals *and parameters*. `aetherc` accepted
  the program and the emitted C then failed to compile (`'x' undeclared` inside
  the inner closure's hoisted function). Closure discovery treated only
  *functions* as scope boundaries, so an inner closure's captures were resolved
  against the enclosing **function** — where the outer closure's locals do not
  exist. A hoisted closure is now its own lexical scope: captures resolve
  against it, chain outward one env hop per nesting level, and a name a nested
  closure needs is carried out to every enclosing closure whose C frame the
  inner env is built from. Writes work too — an inner closure mutating an
  enclosing closure's local shares one heap cell, promoted at every level from
  the writer up to the declaring scope. This is the load-bearing shape for
  list/repeater UI (`ng-repeat`/`ForEach`): the per-item render closure can now
  attach a handler closing over that item.

- **A string first declared inside a loop body is no longer captured as an
  `int`.** The capture's C type was resolved by a scan of the enclosing scope's
  *top-level* statements only, so a name declared one block deeper (`while … {
  nm = string.concat(…) }`) fell through to the `int` default. Capturing it
  produced a `-Wint-conversion` warning and a segfault at run time — a silent
  miscompile, not a compile error. The type lookup now recurses into nested
  blocks, in lockstep with the analysis that decides the name is a capture.

## [0.386.0]

### Fixed

- **`io.read_file` / `fs.read` no longer silently return `""` for `/proc`,
  `/sys`, pipes, and sockets** (#1116). Both sized their buffer from
  `fseek(SEEK_END)` / `ftell`, which reports `0` for any `/proc` or `/sys`
  seq-file (and is meaningless for unseekable fds), so they returned an empty
  string with **no error** — silent data loss on a common operation (reading a
  pseudo-file). They now keep the fast size-based path for regular seekable
  files and fall back to a grow-and-read-to-EOF loop when the size is 0 or the
  fd isn't seekable. A genuine read error surfaces as an error/NULL, not `""`.

### Added

- **`fs.statvfs(path) -> (total, free, avail, err)`** (#1117). Exact filesystem
  byte counts for the filesystem containing `path`, via POSIX `statvfs(2)`
  (portable across Linux/macOS/BSD). `avail` is `f_bavail` — the space usable by
  an unprivileged process, the value you want for "how much can I actually write
  here" (e.g. auto-filling a write range: `end = avail / file_size`). Replaces
  shelling out to `df` and parsing columns; sits alongside `fs.size` /
  `fs.file_stat`. Windows (no `statvfs`) returns the error branch.

## [0.385.0]

### Fixed

- **Release build (`make install`) on GCC 16 / glibc 2.43.** glibc 2.43's
  const-preserving `strstr()` returns `const char*` for a `const char*` argument,
  so assigning the result to a plain `char*` trips `-Werror=discarded-qualifiers`
  on GCC 16 (in `lsp/aether_lsp.c` and `std/net/aether_http_server.c`). Both
  results are read-only (pointer arithmetic and comparisons, never written
  through), so they're now `const char*`. Backward-compatible: assigning the
  plain-`char*` return of older glibc's `strstr` to a `const char*` is warning-
  free on every compiler (verified on GCC 12.2 / glibc 2.36, Clang, and
  mingw-w64).

## [0.384.0]

### Fixed

- **`std.http.client.set_cafile` now actually pins the CA as the trust anchor**
  (#1110, follow-up to #1107). The CA loaded fine (`set_cafile` returned `""`)
  but `send_request` with verification on could still fail
  `certificate verify failed` against a server whose chain the couriered CA
  verifies cleanly via `openssl -CAfile` — because the pin was wired with a
  per-`SSL` `SSL_set1_verify_cert_store`, which is not reliably the *trust*
  store consulted during verification on every TLS library (it worked on
  OpenSSL 3.x but not universally). Reworked to build a **dedicated per-request
  `SSL_CTX`** whose trust store is loaded via `SSL_CTX_load_verify_locations` —
  the portable, version-agnostic idiom that mirrors `openssl s_client -CAfile`
  and behaves identically across OpenSSL 1.1/3.x and LibreSSL. Also fixed
  hostname verification for IP-literal hosts (e.g. `https://192.168.0.204:8006`)
  to use `X509_VERIFY_PARAM_set1_ip_asc` rather than `set1_host`, so the IP SAN
  is checked correctly on older OpenSSL that didn't auto-detect IP literals.
  A pinned CA that doesn't cover the presented cert still fails the handshake
  (fails closed). The regression test now uses a real CA-signs-a-separate-leaf
  chain (the Proxmox-VE topology) rather than a self-signed cert, so it actually
  exercises the trust-anchor path.

## [0.383.0]

### Added

- **`std.http.client.set_cafile(req, path)` — per-request custom CA pin** (#1107).
  Verify the peer certificate against a specific PEM CA/cert bundle instead of the
  system trust store, while **keeping peer and hostname verification on** — the
  "verify, but against THIS cert" knob for machine-to-machine calls to a host with
  a private or self-signed CA (e.g. a Proxmox VE API's `pve-root-ca.pem`). It is
  strictly stronger than `set_insecure`: courier the CA out-of-band once, then pin
  it instead of blind-trusting. Applied per-connection via a per-`SSL`
  `X509_STORE`, never on the shared `SSL_CTX`, so other requests are unaffected; a
  certificate the pinned CA doesn't cover fails the handshake (fails closed, never
  open). Passing `""` clears the pin.

## [0.382.0]

### Added

- **`ae fmt`, a source formatter.** Rewrites Aether source into a canonical
  layout: 4-space structural indentation, normalized spacing around operators
  and punctuation, at most one blank line between constructs, no trailing
  whitespace, and a single final newline. `ae fmt` reads stdin and writes
  stdout; `ae fmt <path>...` formats files (recursing directories) in place;
  `ae fmt --check` writes nothing and exits non-zero if anything is unformatted
  (for CI). It is whitespace-only and comment-preserving: the significant-token
  sequence is never reordered, dropped, or fused, and string literals, `${...}`
  interpolation, heredocs (whose body indentation is significant), backtick raw
  identifiers, and comments are copied verbatim, so it cannot change program
  behavior. User line breaks are preserved (no expression reflow yet). Verified
  semantics-preserving (byte-identical generated C, modulo `#line`) and
  idempotent across every program in `examples/` and `tests/`. See
  [docs/formatter.md](docs/formatter.md).

## [0.381.0]

### Added

- **`std.bits.wrapping_add64` / `wrapping_mul64`** — defined modulo-2^64 add and
  multiply. Aether's `long` is signed `int64_t` and native `a * b` overflow is
  undefined behaviour (a `-fsanitize=undefined` build traps on it) even though
  2's-complement wrap "happens to work" at `-O2`; these compute in the unsigned
  domain so the wrap is defined and optimiser-proof. They join the existing
  unsigned 64-bit helpers (`udiv64` / `urem64` / `ucmp64`). Motivated by ports
  of C tools whose on-disk / wire format depends on defined unsigned overflow —
  e.g. F3's fill/verify LCG `x = x * 4294967311 + 17`.

## [0.380.0]

### Fixed

- **Selective import of a stdlib module no longer suppresses instantiation of
  wrappers used transitively by an imported library** (#1097). When the
  top-level unit did `import std.tcp (connect)` — a *selective* import that
  omitted a tuple wrapper (`poll2` / `read_n` / `write_n`) — and an imported
  library used that wrapper internally, the omitted wrapper was never
  code-generated: its call site degraded to an undefined `tcp_poll2` and the
  build failed at the *library's* source location. The cross-module merge's
  transitive-dependency pass skipped any module that was also a direct import,
  on the assumption the main loop had fully merged it; but a *selective* direct
  import merges only its named subset. The transitive pass now recognises a
  module that is both a direct import and a transitive dependency, and merges
  the remaining exports the library needs (dedup guards keep the already-merged
  subset a no-op). This makes a partial `std.tcp` import behave like the
  no-import case, which already merged the full surface transitively. The
  bare-name selective restriction on user code is unchanged.

## [0.379.0]

### Added

- **`std.tcp` readiness primitives — `tcp.poll` / `tcp.poll2`** (#1092). Thin
  `poll(2)` wrappers that wait for a socket (or two sockets at once) to become
  readable with a caller-supplied timeout, without reading and without touching
  the socket's connected flag. `poll2` is the primitive a full-duplex relay (a
  CONNECT tunnel / TCP splice) needs to service whichever direction speaks
  next; blocking `read_n` from one thread of control cannot express that.

### Fixed

- **`std.tcp` read-timeout no longer masquerades as a connection close**
  (#1092). `tcp_receive_raw`/`tcp_receive_n_raw` collapsed every `recv <= 0`
  into a single "closed or failed" branch that also marked the socket
  permanently dead — so a quiet-but-alive direction (a peer idle for the 30 s
  `SO_RCVTIMEO` window, normal on a long-lived tunnel) tore the connection down
  mid-stream. Would-block / timeout (`EAGAIN`/`EWOULDBLOCK`/`WSAETIMEDOUT`) is
  now distinguished: `read_n` returns a distinct `"timeout"` sentinel and
  leaves the socket connected for a retry; only an orderly FIN or a hard error
  is treated as a terminal close.

## [0.378.0]

### Fixed

- **Two memory leaks on the normal (non-OOM) path.** `http_route_matches`
  allocated fresh `param_keys`/`param_values` arrays on every call and only the
  last call's pair was ever freed, so a server with more than one route leaked
  the two arrays (plus their strings) for every candidate route tried before the
  match, and for every route on a 404, on ordinary traffic. It now frees the
  previous call's params first (which also clears stale params an earlier failed
  pattern left behind). `scheduler_release_pooled` never freed an actor's
  lazily-allocated same-core `spsc_queue`, leaking a multi-KB buffer for every
  actor that had flushed a same-core batch; it is now reclaimed on teardown (a
  reused pooled slot re-allocates lazily).

- **Allocation-failure hardening across the runtime and standard library.** A
  sweep for the "store a failed allocation, report success, crash later" class
  (a delayed fault far from the failed alloc, worse than a clean out-of-memory
  failure) plus self-overwriting `realloc`s that leak the original and then
  dereference NULL. Fixes span: the cooperative and multicore schedulers (actor
  table, per-core I/O map, send-batch buffer with a direct-send fallback), NUMA
  init (falls back to single-node instead of a NULL cpu-to-node map), the
  cooperative message send (fails loudly like the threaded path rather than
  dispatching a NULL payload), actor tracing; the HTTP/1.1 server (request
  header arrays, response create, `set_header`/`add_header`, route params and
  bound, route and middleware registration, accept-thread context), the HTTP
  client redirect follower, the HTTP/2 request builder, the middleware factories
  (session-auth, rate-limit bucket, static-file opts, request-header add), the
  proxy Prometheus exporter (an OOM-path escape-buffer leak), and runtime type
  conversion. The normal path is unchanged; every fix degrades gracefully or
  fails cleanly under memory pressure.

### Documentation

- **Closure capture semantics corrected.** `closures-and-builder-dsl.md` and
  `closures-and-lifetimes.md` claimed closures capture by value and that a
  mutation like `count = count + 1` is not visible to the enclosing scope. The
  compiler actually heap-promotes a captured variable a closure assigns to, so
  the outer binding and the closure share one cell and writes are visible both
  ways (the Ruby/Groovy model, asserted by
  `tests/syntax/test_closure_mutable_capture_probe.ae`). The docs now describe
  this and scope ref cells to state that isn't a plain captured local.

- **Corrected several doc claims contradicted by the compiler/stdlib** (each
  reproduced against a freshly built compiler): the `as` primitive cast is
  documented as not parsing, but the #480 value cast means `n as int` and other
  numeric casts compile and run (non-numeric casts like `buf as string` parse
  and are rejected at type-check with `E0200`), `language-reference.md`;
  `io.stderr_write` / `io.stdout_write` take one argument, not two (length is
  computed internally), `stdlib-reference.md`; and the `std.tcp` write function
  is `tcp.write`, not `tcp.send` (`send` is a reserved keyword),
  `stdlib-api.md`.

## [0.377.0]

### Added

- **HTTP server CONNECT tunnel takeover** (#1086). A handler can now call
  `http.response_accept_tunnel(res)` after setting an accepting response
  (typically `200 Connection Established`) to send the response head
  immediately and take ownership of the underlying cleartext HTTP/1.1
  connection as a `std.tcp` socket. The normal HTTP response lifecycle then
  stops for that connection, so the handler can relay length-aware binary data
  with `tcp.read_n` / `tcp.write_n` and close the stream deterministically.
  Rejected CONNECT requests still use the ordinary response path. New
  integration: `tests/integration/http_server_connect_tunnel/`.

## [0.376.0]

### Fixed

- **Allocation-failure handling in a few registration helpers.** Several
  functions stored a `strdup`/`malloc` result and reported success without
  checking it, so under memory pressure they left a NULL in a live structure and
  crashed later (a delayed fault, worse than a clean out-of-memory failure). Now
  they allocate up front, store nothing on failure, and signal it:
  `aether_vhost_register_host`, `aether_middleware_rewrite_add`, and
  `aether_middleware_error_page_add` (a NULL host / rewrite prefix / error-page
  body would crash the request or error path); `aether_shared_map_put` (a NULL
  key crashes the next `strcmp`); and `aether_convert_type` (a NULL result was
  dereferenced immediately). The normal (non-OOM) path is unchanged.

## [0.375.0]

### Fixed

- **Release builds use parallel LTO**. The `release` target, and therefore
  `make install`, now chooses a parallel link-time-optimization mode when the
  compiler supports one: `-flto=thin` for clang, `-flto=auto` for GCC 10+, and
  plain `-flto` as the compatibility fallback. This avoids the previous
  single-core LTO link that could make optimized installs look stalled, and the
  release-build status line now calls out the LTO mode and expected delay.

## [0.374.0]

### Added

- **Flow-sensitive optional narrowing** (#1068). A none-check on an optional
  variable narrows it in the guarded branch: inside `if x != none { ... }` (and
  the `else` of `if x == none { ... } else { ... }`), `x` is its inner type `T`
  and is used directly, without the `!` force-unwrap, and the runtime none-check
  is elided (presence is proven by the guard). It is a pure compile-time analysis
  with zero runtime cost, turning a class of `!`-unwrap panics into
  statically-guaranteed-safe accesses. The narrowed value flows through
  expressions, field access, and function arguments; nested guards narrow their
  own innermost block. Narrowing is refused, soundly, when the branch rebinds the
  variable or uses it with an optional-only operator (`== none`, `!= none`, `!`,
  `??`, `?.`). New regression: `tests/regression/test_optional_narrowing.ae`;
  docs in `language-reference.md`.

- **Length-aware TCP I/O** (#1078). `std.tcp` now exposes
  `tcp.write_n(sock, data, length)` and `tcp.read_n(sock, max)` on top of
  `tcp_send_n_raw` / `tcp_receive_n_raw`, so TCP relays can send and
  receive byte buffers with embedded NULs without strlen truncation. The
  read side returns `(bytes, length, err)` using a length-bearing
  AetherString, matching the binary-safe stdlib pattern used by
  `fs.read_binary`.

## [0.373.0]

### Added

- **Enum-indexed arrays, `[E]T`** (follow-up to #1044). A fixed array with one
  slot per member of enum `E`, indexed by an `E` value instead of a raw integer:
  `const LABELS: [Dir]string = ["north","east","south","west"]; LABELS[Dir.E]`.
  Sized at compile time to the enum's member range (`0 ..= max value`) and
  lowered to a plain C array, so there is zero runtime cost and no bounds check
  is needed (the index is a sealed enum value). A positional literal supplies one
  value per member in declaration order and the count must match; indexing with a
  raw `int`, a mismatched value count, or a non-enum index type are compile
  errors. Supported for local variables and top-level `const`; array-typed
  parameters and empty `[]` initialisers share the pre-existing fixed-size-array
  limitations and are a separate follow-up. New regression:
  `tests/regression/test_enum_indexed_array.ae`; docs in `language-reference.md`.

## [0.372.0]

### Added

- **Implicit enum member selector** (follow-up to #1044). Where the expected
  type at a site is already a known enum, a member may be written bare, without
  the enum prefix: a function argument (`paint(North)`), a typed initializer
  (`c: Direction = North`), an assignment (`c = South`), a return (`return
  West`), and either side of an enum comparison (`c == North`, `North == c`).
  The bare member is lowered to the enum constant, matching the qualified
  `Direction.North`. Non-breaking: a real binding named like a member always
  wins, and a bare name that is not a member of the expected enum stays an
  ordinary "undefined variable" error. Implemented as a localized coercion at
  each site where the expected enum is in hand (no expected-type threading added
  to the general inference path). New regression:
  `tests/regression/test_enum_implicit_selector.ae`; docs in
  `language-reference.md`.

## [0.371.0]

### Added

- **Enum `match` completeness** (follow-up to #1044). A `match` on an enum now
  accepts bare-name arms (`Red ->`, not only the qualified `Color.Red ->`),
  resolving the member against the scrutinee's enum, and is exhaustiveness-
  checked: a match that covers every member needs no `_`, while a non-exhaustive
  match with no `_` is a compile error naming the missing members (the same
  guarantee sum types already give). Previously a non-exhaustive enum match fell
  through and yielded an uninitialized result, and a bare member name failed as
  an "undeclared identifier" at C-compile time. Both are scoped to enum-scrutinee
  matches; numeric, string, sum, optional, and ranged matches are untouched. New
  regression: `tests/regression/test_enum_match_completeness.ae`; docs in
  `language-reference.md`.

## [0.370.0]

### Fixed

- **`return match x { ... }` miscompiled** (#1054). A `match` in return position
  is value-producing, but the grammar reaches `match` only as a statement, so
  the return parsed with no operand and the match became a dead sibling: codegen
  emitted a void `return;` followed by an orphaned match whose arm bodies were
  dead expression-statements, and the function returned garbage. The parser now
  parses a `match` as the return operand, and codegen lowers it via the same
  result-variable path the working `v = match x { ... }` form uses (declare a
  temp, arms assign it), then returns the temp through the normal return
  machinery so contracts, defers, and escape drains all still apply. New
  regression: `tests/regression/test_return_match.ae`.
## [0.369.0]

### Added

- **Bit sets, `bit_set[E]`** (#1046). A set of members of an enum, backed by a
  single unsigned 64-bit word (one bit per member, at the member's enum value),
  so every operation is a bitwise op with zero runtime cost. Construct with a set
  literal, `bit_set[Perm]{ Perm.Read, Perm.Write }` (bare member names and the
  empty set `bit_set[Perm]{}` also work); operate with `in` (membership), `+`
  (union), `-` (difference), `<=` / `>=` (subset / superset), `==` / `!=`
  (equality), and `card(s)` (cardinality, a `popcount`). A bit set is nominal and
  strictly typed: it never implicitly converts to or from an integer, and two
  bit sets interoperate only when they are over the same enum; members must lie
  in `0..63`. Usable as a local, parameter, return type, and struct field. `in`
  is now also an expression operator (the range-`for` header still consumes its
  own `in` first, so loops are unaffected). New regression:
  `tests/regression/test_bit_set.ae`; docs in `language-reference.md`.

### Fixed

- A parametric type used as a **function return type**, `-> Name[T] { ... }`
  (e.g. `bit_set[E]`, `Isolated[T]`), was mis-parsed: the return-type
  disambiguator only recognized a bare or dotted name before the body brace, so a
  `[...]` group hid the block body and the signature fell through to the arrow-
  expression path, producing a spurious top-level parse error. The disambiguator
  now scans the balanced bracket group, fixing bracketed return types generally.

## [0.368.0]

### Added

- **Struct field injection via `using`** (#1048). A struct field declared
  `using embed: Sub` embeds a sub-struct and promotes its fields into the outer
  struct's namespace: `f.x`, when `x` is not a direct field, resolves to
  `f.embed.x` at compile time, for both reads and writes. Composition without
  vtables or method sets, a pure member-access rewrite with zero runtime cost;
  the outer struct just holds the embedded struct as an ordinary field, and the
  explicit `f.embed.x` path still works. A name no direct or `using` field
  provides is still a "no field" error. Only the field form is adopted (Odin's
  `using` *statement* form is deliberately omitted as a readability footgun).
  `using` is a contextual keyword (no lexer change). New regression:
  `tests/regression/test_using_field_injection.ae`; docs in
## [0.367.0]

### Added

- **First-class `enum` types** (#1044). `enum Direction { North, East, South,
  West }` (implicit `0..`) and `enum Errno { Ok = 0, NotFound = 2, Perm = 13 }`
  (explicit values; a bare member is the previous value + 1, matching C). Members
  are referenced by qualified name (`Direction.East`), used like any type on
  parameters / returns / locals, compared nominally (only the same enum), and
  matched with qualified arms (`match d { Direction.North -> ... _ -> ... }`).
  An enum is integer-backed, so its members interconvert with integer scalars
  (`x: int = Errno.Perm`), but two different enums are never compatible. Lowers
  to a C `typedef enum` with zero runtime cost. This is the foundation for
  `bit_set`, enum-indexed arrays, and cleaner C-enum FFI. Deferred to follow-ups
  (they need context-type propagation): the implicit `.North` selector,
  bare-name match arms, enum-indexed arrays, and enum-match exhaustiveness.
  New regression: `tests/regression/test_enum_basic.ae`; docs in
  `language-reference.md`.

## [0.366.0]

### Added

- **Ranged and multi-value `match` / `switch` cases** (#1047). A case label can
  now be an inclusive range `lo..=hi`, a half-open range `lo..<hi` (consistent
  with the exclusive `for i in 0..5`), or a comma-list of values and ranges in
  one arm: `match score { 90..=100 -> "A"  80..<90 -> "B"  60, 61, 62 -> "D"  _
  -> "F" }`. Ranges are over integer ordinals; a ranged arm lowers to a plain
  `x >= lo && x <= hi` comparison in the branch chain (no runtime, no
  allocation). In a C-style `switch`, a comma-list lowers to several `case`
  labels sharing a body, and a switch containing any range is lowered to an
  equivalent if-else chain (safe because Aether's `switch` has no fall-through).
  Existing single-literal cases are unaffected. New operators `..=` / `..<`;
  new regression `tests/regression/test_ranged_match_cases.ae`; docs in
  `language-reference.md`.

## [0.364.0]

### Added

- **FFI: tuple-typed extern parameters — by-value C struct arguments**
  (#1033). The parameter-position mirror of #271's tuple returns: an extern
  param typed `(T1, T2, ...)` lowers to the same synthesized `_tuple_*`
  typedef, passed by value, and call sites pass parenthesized tuple
  literals — `img_triangle(dst, (10.0, 10.0), (60.0, 10.0), (35.0, 50.0),
  (255, 0, 0, 255))`. Codegen packs each literal into a compound literal
  with per-element casts; no hand-written flat-scalar C shim (or its extra
  call frame) per bound function. New **`f32`** type (C `float`, 32-bit)
  legal in both parameter and return tuples — raylib's `Vector2`/`Color`
  family is now expressible in both directions (Aether's own `float` stays
  double). Conservative slice: scalar/`byte`/`f32`/`bool`/`ptr` elements,
  no nesting, no strings; the typechecker enforces element count and
  rejects tuple literals aimed at non-tuple params. Byte/longdouble tuple
  elements also stopped producing invalid typedef names (space in
  identifier). docs/c-interop.md gained "binding struct-returning C
  functions" (the `LoadImage` zero-glue pattern from the issue) and the
  tuple-parameter section. Test: `tests/integration/extern_tuple_param/`.

### Fixed

- **std.os argv API: the documented qualified forms resolve** (#1035).
  `os.aether_args_count()` / `os.aether_args_get(i)` — the exact spellings
  in language-reference.md — died with E0301 because qualified resolution
  only joined `<module>_<name>` and the argv externs are exported under
  their raw unprefixed names. The resolver now falls back to the bare
  exported name, gated on the module explicitly exporting it (so
  `anything.foo` can never reach an unrelated global), with the call-site
  name rewritten so codegen emits the real C symbol. Std modules register
  under full paths (`std.os`), so the gate matches module names by their
  leaf component too. Also added ergonomic wrappers `os.args_count()` /
  `os.args_get(i)` mirroring the existing `args_seal`/`args_sealed`
  pattern (`args_get` returns an owned copy, "" when out of range). Test:
  `tests/regression/test_issue1035_qualified_argv.ae`.
- **`ae` exe cache: `AETHER_CACHE_DIR` override + crash-proof concurrent
  publishing** (#1032). The cache location was hard-wired to
  `$HOME/.aether/cache`, unusable for runners with a read-only `$HOME`
  (agent sandboxes, hermetic CI) — `AETHER_CACHE_DIR` now redirects it
  per-process (`AETHER_HOME` deliberately still doesn't: toolchain root
  and artifact dir are different concepts). Concurrent same-key
  invocations also raced on shared slots: `ae run` pointed the *linker*
  at the final slot and the hit path was exists→exec, so a second
  invocation landing mid-link exec'd a truncated binary; `ae build`
  populated the slot with a non-atomic copy. Both writers now produce
  `<slot>.tmp.<pid>` and publish with an atomic rename (`MoveFileEx` on
  Windows), so readers see a complete file or a miss — never a partial.
  Orphaned temps from killed writers are swept after an hour. Two new
  integration tests: the read-only-`$HOME` override scenario, and an
  8-way parallel cold-cache hammer.

## [0.363.0]

### Added

- **`--emit=csrc` now also emits a machine-readable JSON catalog** (#996). Building
  `ae build --emit=csrc foo.ae -o foo` writes `foo.catalog.json` alongside `foo.c`
  and `foo.h`: a faithful JSON serialization of the same `aether_lib_meta()` symbol
  catalog the `.c` carries in `.rodata` (functions, closures, constants), plus a
  `capabilities` array recording the `--with` grants the artifact was built with,
  so a consumer can inspect the syscall surface before compiling the source. The
  JSON is driven by the identical codegen tables as the C struct (they can't
  drift), is deterministic and human-diffable (so the source artifact is
  content-addressable), and lets any language's binding generator consume the ABI
  without dlopening a native lib. This completes the source-distribution primitive:
  the remaining #996 follow-ups are single-file amalgamation and standalone
  runtime-source bundling. New coverage in `tests/integration/emit_csrc/`
  (well-formedness, functions/constants, capability provenance).

## [0.362.0]

### Added

- **`Isolated[T]`: move-only actor message payloads** (#479). A compile-time
  -only, zero-cost wrapper (Nim/Pony-inspired) for transferring ownership of a
  heap-bearing value exactly once. `isolate(x)` wraps a value move-only;
  `consume(iso)` unwraps it; every other use is rejected by a new forward move
  checker, so a value used after `send` / `consume`, a heap source reused after
  `isolate`, or a loop-external Isolated consumed inside a loop is a compile
  error (`use of moved value`), while single-use, both-`if`-branch consume,
  fresh-per-iteration, and copyable-scalar sources are accepted. `Isolated[T]`
  is nominal (never implicitly convertible to or from bare `T`) and lowers to
  `T`'s C type with no runtime cost, exactly like a `distinct` type;
  `isolate` / `consume` are the identity at runtime. Works today for scalar,
  string, and struct payloads and ownership transfer into a function; wiring an
  isolated `message` constructor through the actor mailbox with auto-unwrap in
  `receive` is a documented follow-up. Design and scope: `docs/isolated.md`.
  New coverage: `tests/regression/test_isolated_basic.ae` and
  `tests/integration/isolated_move_reject/`.

## [0.359.0]

### Fixed

- **`ae` build cache invalidates on lib-module edits** (#1025). Two gaps let
  `ae run` / `ae build` serve a stale binary after a module was edited: (A) the
  default `lib/` directory the compiler searches when no `--lib` /
  `$AETHER_LIB_DIR` is set was never part of the cache key, so an edit to a
  module in the canonical `src/main.ae` + `lib/<name>/module.ae` layout was
  invisible; (B) the explicit-`--lib` walk keyed on mtime(seconds)+size, so a
  same-second, same-size edit (a one-character constant flip in an editor-save
  loop) was missed. The cache key now walks the default lib dir too, and
  content-hashes every lib-module file (`.ae`/`.c`/`.h`, recursively) instead of
  keying on mtime+size, so any content change invalidates and a bare `touch`
  does not. The default-lib name is now a shared `AETHER_DEFAULT_LIB_DIR`
  constant referenced by both the compiler's import resolver and the cache-key
  builder, so the searched dir and the invalidated dir can't drift apart. The
  lib-dir walk is POSIX-only (`hash_lib_dir_entries` is `#ifndef _WIN32`, as
  before this change); wiring it for Windows is a follow-up. New regression:
  `tests/integration/cache_lib_invalidation/` (skips on Windows).

- **std.fs file sizes and mtimes are 64-bit end-to-end** (#1021). Every size
  surface was a 32-bit C `int`, so files >= 2 GiB reported wrapped-negative
  sizes (a disk-usage tool under-counts exactly the files that dominate disk
  usage). Widened in place: `file_size_raw`, `fs_get_stat_size`, and the
  `fs.size` / `file.size` / `fs.file_stat` wrappers now speak `long`
  (C `int64_t`); mtimes (`file_mtime`, `file_mtime_raw`, `fs_get_stat_mtime`,
  `fs.mtime`, `file_stat`'s slot) widened in the same pass (Y2038). On
  Windows the stat calls moved to `_stati64` — plain `_stat` carries a
  32-bit `st_size` — and the positional-I/O family (`fs_pwrite_raw` /
  `fs_pread_raw` / `fs_ftruncate_raw`) now defines its offsets/returns as
  `int64_t` with `_fseeki64`, matching the `int64_t` prototypes the compiler
  emits for Aether `long` externs (plain C `long` is 32-bit on LLP64, so the
  old definitions were an ABI mismatch there). Regression test creates a
  sparse 2 GiB + 5 file and asserts every surface reports the true value.
  Wrapper note: the Go-style tuple wrappers keep their `(value, err)` shape,
  but their success arm now returns first — the first `return` statement
  pins the inferred tuple slot types, and the int-literal error arm would
  otherwise narrow the size slot back to 32-bit.

## [0.358.0]

### Added

- **stdlib descriptor accessors for Capsicum plumbing** (#1003). The opaque
  stdlib handle types now expose their OS-level file descriptors:
  `file.fd(handle)` / `fs.fd(handle)` for open files, `tcp.fd(sock)` and
  `tcp.server_fd(server)` for sockets (raw externs `file_fd_raw`,
  `tcp_fd_raw`, `tcp_server_fd_raw`). Closes the gap where
  `capsicum.rights_limit()` / `fcntls_limit()` could only narrow descriptors
  obtained from raw externs — the common open-through-the-stdlib case can now
  narrow rights before `capsicum.enter()`. The fd is owned by the handle:
  never `close()` it directly. New FreeBSD enforcement test
  `tests/freebsd/rights_limit_stdlib_fd.ae` proves the flow end to end.
- **Proof that `spawn_sandboxed` auto-contains Aether children on FreeBSD**
  (#1003). The wiring itself shipped earlier (`AETHER_CAPSICUM=1` +
  `capsicum_autosandbox.c`), but stale comments in `std.capsicum` still called
  it "a later phase" and nothing exercised the composed path. Comments now
  state the contract, and `tests/freebsd/spawn_capsicum_containment.sh`
  asserts a spawned Aether child reports `capsicum.in_mode() == 1` without
  ever calling `enter()` itself (`tests/freebsd/run.sh` now drives `.sh`
  tests alongside the `.ae` ones).

## [0.357.0]

### Added

- **`--emit=csrc`: distribute portable C source instead of a native lib** (#996,
  minimal). `ae build --emit=csrc foo.ae -o foo` emits `foo.c` (the portable
  generated C) plus `foo.h` (a catalog header with the `aether_<name>()`
  prototypes) and stops — no `gcc`, no host `.so`. Same catalog codegen as
  `--emit=lib`; the artifact is *source*. A consumer compiles it wherever
  (`cc -fPIC -shared foo.c $(ae cflags)`), feeds it to WASM, or static-links it —
  the enabling primitive for compile-on-install bindings and a source-registry
  story. Follow-ups (single-file amalgamation, `catalog.json`, standalone
  runtime-source bundling) are noted in #996.

### Fixed

- **`--emit=lib` on Windows exports the catalog symbols reliably** (#993). The
  MinGW `-shared` link now passes `-Wl,--export-all-symbols` under `--emit=lib`,
  so the `aether_<name>` / `@c_callback` catalog exports are visible in the
  `.dll` regardless of GCC's auto-export heuristic (which silently flips off the
  moment any symbol carries an explicit `__declspec(dllexport)`, e.g. an
  `--extra` C shim). ELF/Mach-O are unaffected (default visibility). Unblocks the
  servirtium-vcr Windows fat-package.

### Documentation

- Document the `std.http.client` TLS + forward-proxy builder knobs (`set_insecure`,
  `use_env_proxy`, `use_http_proxy`, `ignore_http_proxy`, plus the previously
  undocumented `set_follow_redirects`) in `stdlib-reference.md` / `stdlib-api.md`,
  and `--emit=csrc` in `emit-lib.md`.

## [0.356.0]

### Added

- **`std.http.client`: hardened forward-proxy control** (#1012, part 2). Three
  per-request builder verbs, defaulting to **DIRECT** — the client does NOT
  follow `$HTTP_PROXY` unless the program opts in, the deliberate inverse of the
  default-follow that produced the httpoxy vulnerability class (CVE-2016-5385).
  Precedence, highest first: ignore > explicit > env.
  - `client.use_env_proxy(req, 1)` — follow `$HTTP_PROXY`/`$HTTPS_PROXY`/
    `$NO_PROXY` (Go-compatible), with guards: the CGI-injectable uppercase
    `HTTP_PROXY` is refused when `$REQUEST_METHOD`/`$GATEWAY_INTERFACE` is set
    (the httpoxy vector; lowercase `http_proxy` stays honoured), and a proxy
    resolving to a loopback/link-local IP literal (127.0.0.0/8, 169.254.0.0/16
    IMDS, ::1, fc00::/7, fe80::/10) is rejected (SSRF).
  - `client.use_http_proxy(req, "http://host:port")` — pin an explicit proxy;
    env is ignored entirely, so a team-controlled proxy (recorder / toxiproxy)
    is immune to whatever the shell/CI set. No SSRF guard (code-visible grant).
  - `client.ignore_http_proxy(req)` — force direct regardless of env / any set
    proxy (the determinism escape hatch, e.g. VCR record mode).
  Plain HTTP through a proxy uses an absolute-form request line; HTTPS uses a
  `CONNECT` tunnel with TLS end-to-end to the origin. A compile-time reject of
  `use_env_proxy` under `--emit=lib` is tracked as a follow-up.

## [0.355.0]

### Added

- **Cross-module actors** (#1006). Actors defined in one module can now be
  spawned and messaged from another; also fixes a single-scalar
  message-field format warning.

## [0.354.0]

### Added

- **`std.http.client`: per-request TLS peer-verification skip** (#1012). A new
  `client.set_insecure(req, 1)` on the request builder skips TLS peer + hostname
  verification for that request only (the `curl -k` /
  `wget --no-check-certificate` equivalent) — for hosts with self-signed or
  otherwise-untrusted certs (dev/staging/appliances/CI). The relaxation is
  applied **per connection** via `SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL)`,
  never on the shared process-wide `SSL_CTX`, so one insecure request cannot
  downgrade verification for any other request in the process. Default is 0
  (verify), so existing callers are unchanged. Unblocks zsync-port's
  self-signed-cert HTTPS scenarios (its `--no-check-certificate` was a parsed
  no-op). The forward-proxy half of #1012 (`HTTP_PROXY` / `CONNECT` tunnelling)
  remains open — the issue scoped it as the lower-priority follow-up.

- **`std.http.client`: streaming response bodies** (#1004). `client.send_stream(req)`
  (or `client.set_stream(req, 1)` before `send_request`) reads only the response
  header block, keeps the connection open, and hands back a response whose body
  is pulled window-by-window with `client.response_read(resp, max)` until an
  empty chunk. Peak memory is one window instead of O(Content-Length), so a
  multi-gigabyte download never materialises whole (the buffered `response_body`
  path is unchanged and still the default). Both `Content-Length` and
  `Transfer-Encoding: chunked` bodies are decoded transparently, so the caller
  always sees payload bytes, never chunk framing. Redirects are still followed
  when enabled; only the final hop streams, and `response_free` closes the
  connection (freeing an intermediate 3xx response tears its stream down, so
  redirect-following is safe). An empty `response_read` is end-of-body or a
  mid-stream error, disambiguated by `response_error`. Implemented in the native
  client (`std/net/aether_http.c`): a shared connect/send/header-parse phase now
  feeds either the buffered read or an incremental `HttpStream` decoder; no
  request logic is duplicated. Tests: `tests/integration/http_client_stream/`
  (128 KiB Content-Length body, differential byte-for-byte vs the buffered fetch
  across many windows) and `http_client_stream_chunked/` (raw-TCP chunked server).

### Fixed

- **Cross-module actors and message types now work** (#1006). An `actor` and
  its `message` types declared in an imported module can now be `spawn`ed and
  sent to from the importing module. Previously `spawn(Worker())` failed at the
  call site with a misleading `Undefined function 'spawn_Worker'` (and
  `Undefined message type 'Ping'`), even though `Worker` was correctly spelled
  and imported. The module merge now clones imported-module actor and message
  declarations into the program under their bare name (like structs); the
  actor's handlers keep their intra-module function/constant references
  rewritten, and the per-program message registry assigns runtime type ids
  across the merge.
- **Codegen: no `-Wformat` warning when printing or interpolating a
  single-scalar message field.** Such a field rides the `intptr_t`
  `Message.payload_int` slot, so a genuine `int` field emitted with `%d`
  mismatched its `intptr_t` storage. `print` / `println` / `${...}`
  interpolation now narrow a `TYPE_INT` argument to `(int)`, mirroring the
  existing `int64` to `long long` cast. Actor-ref and pointer fields are
  unaffected (they print via `%s`), so no pointer-width value is truncated.

## [0.353.0]

### Fixed

- **Selective import in a consumer no longer breaks a dependency's qualified
  namespace** (#1009). A consumer that did `import std.os (getenv)` while a
  dependency whole-imported `std.os` (and called `os.now_monotonic_ns()`
  qualified) failed to build — `error[E0301]: Undefined function
  'os.now_monotonic_ns'` — but only when `std.os` was not the dependency's first
  import. The module merger froze its clone-loop bound at the pre-loop child
  count, so a synthetic bare-import (#870) re-injected to re-open the qualified
  surface could land past that bound and never have its wrappers cloned. The
  merger now scans the synthetic imports too; revisiting also surfaced and fixed
  two latent const-clone dedup gaps (`redefinition of 'sha2_K256'`). Broke every
  aeocha consumer that also selectively imported a module aeocha whole-imports
  (aeocha lists `std.os` 7th of 10).

## [0.352.0]

### Added

- **`ae build` honors `$AE_CC` then `$CC` for the C-backend compiler** (#994),
  mirroring the Makefile's `CC=` override. This selects the compiler that turns
  Aether's generated C into the final binary; `aetherc` (the Aether-to-C front
  end) is untouched. It unlocks the same-OS, cross-arch case with no new
  codegen, e.g. `CC=aarch64-linux-gnu-gcc ae build --emit=lib foo.ae -o
  libfoo.so` emits an arm64 `.so` on an x86_64 host. Unset `$AE_CC` / `$CC`
  keeps the current default (`gcc`, WinLibs-bundled gcc on Windows) byte for
  byte. A missing compiler now fails with a clear `C compiler '<name>' (from
  $CC) not found` instead of a downstream link error. Applies to `ae build`,
  `ae run`, and `ae build --emit=lib`.

## [0.351.0]

### Documentation

- **Documentation overhaul (docs only, no code or behavioural change)** (#1001).
  A corpus-wide accuracy and de-slop pass across the docs, followed by a
  structural cleanup:
  - Design-rationale and concurrency-pattern docs are grouped under a new
    `docs/design/` section (closure lineage, parse-don't-validate, the
    Chlipala lens, the rules-engine exploration, sharded actor map, snapshot
    cell, concurrent-cache benchmark).
  - `docs/cross-references/` is reworked from internal issue-body drafts into
    professional design-history surveys of Fir, Flint, Zym, and
    GoogleCloudPlatform/Aether, each with a status header and a public source
    URL. The Flux comparison was dropped: its source is a proprietary,
    all-rights-reserved spec that cannot be verified or safely reproduced.
  - The `docs/notes/` handoff files were retired; their still-open items are
    tracked as #1002 (release-workflow CHANGELOG guard), #1003 (std.capsicum
    follow-ups), and #1004 (std.http streaming response bodies).
  - Em-dashes are removed from all documentation prose in favour of commas.
  - The root `README.md` is the single documentation index (the `docs/design/`
    and `docs/cross-references/` subfolder READMEs were removed, and the design
    docs are listed directly in the README). Every internal doc link and
    heading anchor was re-audited and resolves clean.

## [0.350.0]

_CHANGELOG reconstruction for the 0.344–0.349 gaps + zsync-port added to LLM.md
(#999); no compiler, stdlib, or runtime behaviour change._

## [0.349.0]

_Docs only — LLM.md / CONTRIBUTING / README corrections (#997). No compiler,
stdlib, or runtime behaviour change._

## [0.348.0]

### Added

- **`@packed` extern-struct SDS-floor recipe** (#747). Documents negative-offset
  header recovery via `std.mem` (whose accessors accept negative offsets by
  construction) in `docs/c-interop.md`, backed by an end-to-end interop
  regression test (`tests/regression/test_issue747_sds_floor.ae`). No runtime
  change — a documented recipe + living-proof test that closes #747.

## [0.347.0]

### Added

- **`std.http`: streaming request bodies completed** (#644). The parse-loop
  reshape landed earlier (bodies over 16 KiB dispatch the handler at
  headers-complete and `request_body_read` pulls windows straight off the
  socket — peak RAM per upload is one window, with TCP flow control as the
  backpressure). This closes the remaining #644 items:

  - **v1 whole-body contract restored**: `http.request_body(req)` on a large
    (streaming) request now *materializes on demand* — the first call drains
    the remaining wire bytes into one buffer, so existing whole-body handlers
    keep working at the O(Content-Length) cost they asked for. Previously it
    returned an empty buffer while `request_body_length` claimed the declared
    Content-Length — a mismatch that read out of bounds if the caller
    trusted the pair. Mixing it with `request_body_read` on the same request
    returns `""` (the consumed prefix is gone; a tail-as-whole would corrupt).
  - **`http.request_body_complete(req)`** — 1 once every declared byte has
    arrived (streaming: pulled off the wire; buffered: always 1). The natural
    chunked-loop terminator.
  - **Semantics decision documented**: `Transfer-Encoding: chunked` request
    bodies remain unsupported (no `Content-Length` → length 0, no body read).

## [0.346.0]

### Added

- **`std.fs`: recursive walk + filesystem change notification** (#977). The
  building blocks real filesystem apps need beyond one-level listing:

  - `fs.walk(root, cb)` visits `root` (depth 0) and every entry beneath it,
    calling `cb(path, kind, depth)` per entry. Kinds come from readdir's
    `d_type` (#966) — one sweep per directory, zero per-entry `stat(2)`.
    The callback steers traversal: return 0 to continue, 1 to skip a
    directory's subtree, 2 to stop the walk. Symlinks are reported (kind 3)
    but never followed, so cycles are impossible.

  - `fs.watch_open(path)` / `fs.watch_wait(w, timeout_ms)` /
    `fs.watch_close(w)` — coarse change notification on a directory over the
    platform primitive: kqueue `EVFILT_VNODE` (macOS/BSD), inotify (Linux),
    `FindFirstChangeNotification` (Windows). `watch_wait` returns 1 when
    something changed (create/delete/modify/rename), 0 on timeout, -1 on
    error; changes between open and wait are queued, not lost, and a burst
    reports once. Re-list with `dir.list` + `dir.list_kind` to see what
    changed.

  ```aether
  n, err = fs.walk(root, |path: string, kind: int, depth: int| {
      if kind == 2 && string.ends_with(path, "/node_modules") == 1 {
          return 1              // skip this subtree
      }
      println("${depth} ${path}")
      return 0
  })

  w, werr = fs.watch_open(dir)
  changed = fs.watch_wait(w, 1000)   // 1 changed / 0 timeout
  fs.watch_close(w)
  ```

## [0.345.0]

### Fixed

- **Codegen mangles struct/message FIELD names that collide with C keywords**
  (follow-up to #976). #976 fixed value identifiers; this completes the class
  for field names. A field named `register`, `signed`, `unsigned`, `volatile`,
  `static`, `double`, … now compiles instead of emitting `int register;` in the
  generated struct. The AST pre-pass rewrites the whole field namespace
  consistently — the field declaration, the struct/message constructor field,
  the field read (`x.field`), and receive-pattern bindings — so declaration and
  use never diverge.

  ```aether
  struct Point { register: int  signed: int }   // was: invalid C
  message Bump { volatile: int }
  ```

## [0.344.0]

_CHANGELOG reconstruction (0.340/0.342/0.343 gaps) + zsync-port added to LLM.md
(#984); no compiler, stdlib, or runtime behaviour change._

## [0.343.0]

### Fixed

- **Codegen mangles value identifiers that collide with C reserved keywords**
  (#976). An identifier whose name is a C keyword (`short`, `register`, `signed`,
  `volatile`, `static`, `double`, …) is a valid Aether identifier but not a valid
  C one, so codegen emitted it verbatim and `int short = 3` broke the C compiler
  even though `ae check` passed — the same "front-end accepts, build breaks"
  class as #952/#953, and the deferred C-keyword half of #880. A pre-codegen AST
  pass now rewrites such value-binding / value-reference identifiers to
  `ae_<name>` once (covering declarations, references, params, match bindings,
  and derived temporaries), keeping every emit site consistent by construction.

## [0.342.0]

### Added

- **`dir.list_kind` (readdir `d_type`) + stable string-list sort** (#966, #967).
  Two stdlib gaps found building a file browser.
  - **#966 — expose readdir's `d_type`.** A directory listing now carries each
    entry's file kind (1 file / 2 dir / 3 symlink / 4 other / 0 unknown — the
    same encoding `file_stat` reports), read straight from `readdir`'s `d_type`
    (Windows' `dwFileAttributes`) via a parallel `kinds` array on `DirList`.
    `dir.list_kind` (std.dir wrapper) / `dir_list_kind` (raw extern) return it, so
    telling files from directories no longer costs an N-entry `stat(2)` sweep.
    Also completes std.dir with `list_count` / `list_get` wrappers (the listing
    API was previously un-iterable via `dir.*`).
  - **#967 — stable string-list sort.** `string_list_sort_lex(list)` sorts a
    string list in-place, lexicographically and stably; `string_list_sort(list,
    cmp)` takes a comparator closure `fn(string, string) -> int`.

## [0.341.0]

### Fixed

- **`client.response_body()` now returns an OWNED string — safe to read after
  `response_free()`.** The body was a pointer *borrowed* from the response, so a
  caller that freed the response before reading the body got garbage or a crash
  (surfaced by the aeo orchestrator's serve-and-dial agents, where an in-handler
  client call's body was read after free). `http_response_body` now retains the
  response's `AetherString` and is annotated `@heap` on the Aether side, so the
  returned string outlives `response_free` and is released automatically at
  scope exit. The borrowed C variant remains as `http_response_body_str` for the
  `_str`/reverse-proxy callers that copy-on-use. Regression:
  `tests/regression/test_http_response_body_owned_after_free.ae`.

## [0.340.0]

### Added

- **Result-type error handling: `-> T!`, `or`, and `!`** (#913). `-> T!` names
  the existing `(value, string)` result-tuple convention and adds ergonomic
  sugar for the three things you do with a fallible call, with no hidden
  machinery — `T!` *is* the `(T, string)` tuple, so the sugar and manual
  destructuring interoperate freely.

  - `return v` in a `T!` function auto-wraps to `(v, "")`; `return v, "msg"`
    reports an error.
  - `expr or default` yields the success value, or `default` on error.
  - `expr or { ... }` runs a block on error with `err` bound to the message
    (the block exits via `return`/`break`/`continue`/`panic`, like a `match`
    arm's block body).
  - `expr!` propagates: inside a `T!` function it returns `(zero, err)` on a
    non-empty error slot; elsewhere it is unwrap-or-trap (panics, catchable
    with `try`/`catch`).

  ```aether
  safe_divide(a: int, b: int) -> int! {
      if b == 0 { return 0, "division by zero" }
      return a / b
  }

  checked(x: int, d: int) -> int! {
      return safe_divide(x, d)!        // propagate on error
  }

  main() {
      q = safe_divide(10, 0) or -1     // q == -1
      r = safe_divide(x, y) or {       // `err` bound; block exits
          println("failed: ${err}")
          return
      }
  }
  ```

### Fixed

- **Thread-safe host resolution — `getaddrinfo`, not `gethostbyname`** (#974).
  The HTTP client and raw TCP connect resolved hosts with `gethostbyname`, which
  returns a pointer into a shared, process-static `struct hostent`; two client
  calls resolving concurrently on different threads (e.g. a request handler that
  dials out while serving) raced on that static buffer and could corrupt each
  other's resolved address. Both sites now use `getaddrinfo` (thread-safe,
  caller-owned memory). Regression: `tests/integration/http_serve_and_dial`.

## [0.339.0]

_Docs / tooling only (Chlipala-lens framing doc, API-doc refresh, benchmark
runtime-source fix); no compiler, stdlib, or runtime behaviour change._

## [0.338.0]

### Added

- **Sum / variant types: `type Name = A | B | C` + exhaustive `match`** (#914).
  A tagged union over existing struct variants — "a value that is exactly one
  of N named alternatives." Completes `match` (which was literal-only) with the
  structural type it can be exhaustive over, and gives ports a checked
  replacement for the hand-rolled "tag int + struct-with-all-fields" pattern.

  ```aether
  struct Circle { r: float }
  struct Rect   { w: float  h: float }
  struct Empty  {}
  type Shape = Circle | Rect | Empty

  area(s: Shape) -> float {
      let a: float = match s {    // narrows `s` to the variant in each arm
          Circle -> 3.14159 * s.r * s.r
          Rect   -> s.w * s.h
          Empty  -> 0.0
          // omitting a variant is a compile error; no `_` needed
      }
      return a
  }
  let s: Shape = Circle { r: 2.0 }   // a variant implicitly wraps into the sum
  ```

  - A variant struct value implicitly wraps into the sum at `let` / parameter /
    return / argument positions (no `some(...)`-style constructor).
  - `match` over a sum narrows the scrutinee to the variant struct inside each
    arm, so `s.field` reads the right member. Exhaustiveness is enforced —
    forgetting a variant is a compile error (or use a `_` wildcard); an arm
    naming a non-variant is rejected.
  - Lowers to a tag enum + C union (`{ Name_tag tag; union {...} data; }`) —
    no allocation, no vtable. Recursive shapes (trees, ASTs) work via explicit
    pointer fields (`left: *Tree`). v1 is monomorphic; generics are a follow-up.

## [0.337.0]

### Fixed

- **macOS arm64: `ae build` couldn't link anything off a released package**
  (#959). Three build-toolchain fixes:
  - **Flat runtime-archive fallback.** `ae build` looked for the prebuilt
    archive only at the canonical nested `lib/aether/libaether.a`. The macOS
    arm64 v0.331/0.332 packages shipped it flat at `lib/libaether.a`, so the
    lookup missed it and fell back to compiling an *incomplete* runtime source
    list — every build, even hello-world, then failed to link (`Undefined
    symbols ... _aether_io_poller_init`). `ae build` now falls back to the flat
    archive before the source path; the complete archive links.
  - **Version-agnostic homebrew link paths.** The link flags baked into `ae`
    came from `pkg-config`, which on homebrew emits versioned
    `-L/opt/homebrew/Cellar/<pkg>/<ver>/lib` paths — so `ld: library 'ssl' not
    found` the moment a formula was upgraded. The build now rewrites those to
    the version-agnostic `/opt/homebrew/opt/<pkg>/lib` symlinks homebrew keeps
    current. No-op on non-homebrew layouts.
  - **Corrupt-archive guard on install.** `ae version install` now validates
    that the extracted `libaether.a` is a well-formed `ar` archive of plausible
    size, catching the interrupted/partial extract that left a truncated
    archive (undefined symbols) and a broken install with no hint at the cause.

## [0.336.0]

### Changed

- **`LLM.md` operational additions** (#912) — rebuild/test table, build-safety
  notes, ask-first thresholds, and the codegen tag-and-grep debugging recipe.
  Documentation only; no compiler, stdlib, or runtime behaviour change.

## [0.335.0]

### Fixed

- **`ae check` now catches over-/under-applying an extern; `from_cstr` survives
  an `AetherString*`** (#952). Two "`ae check` passes but the program then
  crashes or fails in gcc" gaps:
  - **Arity of extern functions wasn't checked.** Calling the zero-arg
    `math.deg_to_rad()` constant as `math.deg_to_rad(x)` (and the sibling
    `math.pi`/`tau`/`e`/`rad_to_deg` constants) passed `ae check` and surfaced
    only as a raw gcc "too many arguments" error. Extern arity is now validated
    in Aether terms, honoring variadic externs (`f(named, ...)`, both the
    `extern` and `@extern("c")` forms) and `_ctx`-first builder externs. The
    fix also wires each imported extern's AST node into its symbol — like
    entry-file externs already were — so the existing extern arg-type checks
    apply across module boundaries too.
  - **`string.from_cstr` segfaulted on an owned-list value.** A string stored
    with `list.add_string_owned` (which keeps the 24-byte `AetherString`
    header) and read back via `list.get` was an `AetherString*`, not a raw
    `char*`; `from_cstr` read the header bytes as character data and copied
    garbage or crashed. `from_cstr` now routes its argument through the
    magic-header-aware accessor, so the round-trip is correct for either an
    `AetherString*` or a plain C string (and is NULL-safe).

## [0.334.0]

### Fixed

- **`ae build` now fails on an imported module's compile error** (#953). `ae
  build` accepted an entry program whose *imported* module did not compile —
  the parser's error recovery dropped the offending construct (e.g. an invalid
  `@` annotation lowering to a bare `return x`), so the merged AST type-checked
  clean and codegen produced a working binary from non-compiling source, while
  `ae check` correctly reported the error. The two disagreed on validity, and a
  build's exit code couldn't be trusted (it bit a mutation-testing driver that
  rebuilds an imported SUT). The entry file's own parse errors were already
  gated, but the global error count was not re-checked after module
  orchestration — which is where imported modules are parsed. Both `build` and
  `check` now fail (non-zero, no binary) when any module they pull in carries an
  error. A clean import is unaffected (the gate keys on the error count).

## [0.333.0]

### Added

- **Optionals: `T?` with `none`, `!`, `??`, `?.`, and `match`** (#340). A
  first-class optional type for "maybe a value," complementing the `(value,
  err)` result convention (which stays the tool for *fallible* operations).
  `T?` collapses the ambiguous "is the value a null pointer, or is the key
  absent?" case (`map.get`, `list.first`, a search that found nothing) to one
  type with predictable handling. Surface:
  - `let m: int? = 69` wraps a value; `let z: int? = none` is the empty
    sentinel. `none` is a reserved literal (like `true`/`false`/`null`) and
    cannot be a variable name. `== none` / `!= none` test presence.
  - Force-unwrap `m!` yields the value or panics on `none` (`forced unwrap of
    \`none\``). Null-coalesce `m ?? d` yields the value or `d`, and binds
    tighter than arithmetic. Optional chaining `v?.field` is none-propagating
    (yields `fieldT?`); chain assignment `v?.field = x` is a no-op when `v` is
    `none`.
  - `match m { none -> …  some(v) -> … }` destructures as a statement or an
    expression. A bare `T` (or `none`) is implicitly wrapped into a `T?`
    parameter, return value, or binding.
  - One uniform representation covers value and reference element types
    (`typedef struct { int has; T val; } ae_opt_<T>`), so there is no
    null-vs-absent ambiguity. Postfix `!` is polymorphic on its operand — an
    optional unwraps the value, a `(value, err)` tuple unwraps the first slot —
    so it does not collide with the actor-send `!` (which is followed by a
    message type) or with `match` pattern arms. See the
    [language reference](docs/language-reference.md#optionals).

## [0.332.0]

### Fixed

- **Heredoc closing-marker rule: no more silent truncation** (#922). A heredoc
  body line that merely read like the closing marker could close the heredoc
  early and silently drop the rest of the body. The close rule is now: a line
  closes the heredoc only when it is the marker alone on its line AND its
  indentation is at or below the shallowest body line — the terminator lives at
  the content's base level. A more-indented marker-like line is therefore body
  content (never a silent truncation); a lone marker indented *past* the body
  matches nothing and is reported as an unterminated heredoc rather than
  dropping content. The closing marker may still be indented (at/below the body
  base; column 0 always works), the marker must be alone on its line
  (`done END` / `xEND` stay content), and body dedent is unchanged (common
  leading-whitespace / least-indented line, like Ruby's squiggly `<<~`). Docs
  (`LLM.md`, language-reference) corrected — they wrongly claimed "column 0
  only," which the lexer never enforced.

## [0.331.0]

### Fixed

- **Qualified type name `mod.Type` accepted in type positions** (#946). A
  module-qualified name was accepted as a value/call (`lib.mk(...)`, #878) but
  not as a *type* — the parser stopped at the `.` (`Expected RIGHT_PAREN, got
  DOT` in a parameter, `Expected LEFT_BRACE, got DOT` in a return type). Only
  the bare exported name worked, which left no way to disambiguate when two
  imported modules export a type with the same name. `mod.Type` now parses in
  parameter types, return types, and C-style typed locals (`mod.Type name`),
  resolving to the bare exported type (the merge brings an exported struct
  into the consumer's namespace unprefixed, so the qualifier is a
  disambiguator). The type parser accepts a dotted name; the return-type
  disambiguator and the typed-local statement dispatcher were taught the
  dotted-name shape so they route to it. (Using an imported struct as a
  *struct field* remains a separate, pre-existing limitation that affects the
  bare name equally — incomplete-type in the consumer TU — and is unrelated to
  this parser asymmetry.)

## [0.330.0]

### Fixed

- **Bare top-level function used as an `fn` value inside a closure body**
  (#943, closure analogue of #940). Wrapping a bare named function as an
  `fn` value from inside a trailing-block closure (`runit(val)` inside a
  `callback { ... }`) failed to compile: the emitted closure function
  referenced an `_aether_bare_adapter_<name>` shim that was only *defined*
  later in the file, so it was undeclared in the closure's translation unit.
  The cause was emit order — closure bodies were emitted before the bare-fn
  adapters, but a closure body can itself wrap a bare fn. The adapters' C
  forward declarations are now emitted before the closure definitions (the
  full bodies still follow, since they call the user functions by name), so
  closure bodies see the prototype in scope. Works in combination with #940
  (a bare fn wrapped inside a closure whose callee is an imported function).

## [0.329.0]

### Fixed

- **Bare top-level function passed as an `fn` arg across a module boundary**
  (#940). Passing a bare named function as an `fn`-typed argument to an
  *imported* module's function failed to compile — the caller referenced an
  `_aether_bare_adapter_<name>` env-ignoring shim that was never emitted in
  the caller's translation unit. The adapter-discovery pre-walk looked up the
  call's callee by its AST name, which for a qualified `mod.fn(...)` call is
  still dotted (`runner.runit`) while the merged definition is `runner_runit`;
  the lookup missed, the `fn`-typed parameter was never inspected, and the
  bare-fn argument's adapter was never registered. The lookup now also tries
  the merged (`.`→`_`) form, so a library API that takes a caller-supplied
  callback by bare name (visitors, comparators, retry/poll predicates) works
  across the import boundary — same as it already did single-file.

## [0.328.0]

### Fixed

- **Module-level `var` (#701) now persists across the import boundary**
  (#937). A mutable module-level `var` defined in an *imported* module lost
  writes: a store inside one of the module's functions was visible to that
  function (it returned the written value) but a later call into the same
  module read the initializer back (`write-returned=7  read-back=0`). The
  module-merge's intra-module rename rewrote *reads* of the global to its
  prefixed name but not the *write target* of an assignment — and worse,
  counted a bare `name = expr` write as a function-local, which shadowed the
  global out of renaming entirely. Codegen then emitted a throwaway local
  (`int counter = n;`) instead of a store to the shared `static`, so the
  write never reached the cell. The rename now treats a bare-name write to a
  module global as the global it is (not a local declaration) and rewrites
  the assignment target, so the store reaches the shared cell — the
  "ambient context / process-global provided by a library" pattern (a config
  cell, a registry, a current-context set during init and read later) works
  across imports. Genuine same-named locals are unaffected.

## [0.327.0]

### Added

- **First-class module re-export** (#924). A module may now list, in its
  `exports`, a symbol it brought in via `import` — and that symbol becomes
  part of its own qualified surface, identically to one it defined
  (`hub.X` resolves to the defining module's symbol). Re-export is transitive
  (a facade can re-export through several layers) and visibility still gates
  it (the origin must export the name). A locally-defined export always wins
  over a same-named import, so there's no ambiguity. This dissolves the
  facade-monolith and per-consumer extern re-declaration patterns: a large
  constants/API module can be decomposed into cohesive leaves that a thin hub
  re-exports, with consumers' `import hub` unchanged — and it breaks the
  `hub → leaf → hub` import cycle that derived-constant leaves otherwise force.

- **UFCS resolves across the import boundary** (#934, follow-up to #928).
  `value.method()` now finds a `method` exported by an imported module whose
  first parameter matches `typeof(value)`, honoring the same visibility as a
  normal qualified `mod.method(value)` call — not just same-file functions.
  This is what makes library-provided fluent surfaces work: a test framework's
  `expect(x).to_equal(5).to_be_gt(0)` with the matchers in an imported module
  and the chain in the consumer's file. Same-file functions still take
  priority; a type-mismatched receiver declines cleanly.

### Changed

- **Circular-import diagnostic names the actual cycle** (#925). The error now
  reads `circular import dependency: a -> b -> a`, listing the participating
  modules in order, instead of the prior `involving module '__main__'` at a
  bogus `0:0` (the synthetic entry root, which is never part of a real import
  cycle). In a large module tree this turns "a cycle exists somewhere — go
  find it" into an actionable trace.

## [0.326.0]

### Added

- **Method-call-on-value (UFCS)** (#928). `x.f(args)` now desugars to
  `f(x, args)` when `f` is a free function whose first parameter type matches
  `typeof(x)` — the missing primitive for fluent / method-chaining DSLs
  (`expect(5).to_equal(5)`, `subject.inc().to_equal(6)`). It works on any
  receiver expression: a call result, a stored value, or a pointer
  (`c.bump()` → `bump(c)` for `c: *Counter`). UFCS is a strict **last-resort**
  fallback — module-qualified calls (`string.length(s)`), struct-field access,
  and function-pointer-field dispatch all keep priority, so nothing that
  compiled before changes meaning; UFCS only fires on a dotted call that would
  otherwise be an "Undefined function" error. A receiver whose type doesn't
  match the candidate's first parameter declines cleanly (no silent coercion).
  No new declaration syntax and no codegen change: existing free functions
  become chainable, and the rewritten call lowers like any other by-value
  call.

## [0.325.0]

### Fixed

- **Module-scope `var` now honours the silent-narrowing guard** (#929). A
  module-scope `var x = 0` infers a 32-bit `int` from its bare initializer,
  exactly like the local `x = 0` form — but the #698 narrowing guard (E0200)
  only fired on locals, so a later 64-bit assignment to the global
  (`x = os.now_monotonic_ns()`) truncated silently. The parser now marks the
  global's inferred type and the typechecker carries that marker onto the
  symbol, so the assignment raises E0200 with the same "annotate the
  declaration" suggestion. An explicit width (`var x: long = 0`) is exempt, and
  a plain int global assigned int values is unaffected.

- **Multiple `${duration}` interpolations in one string render distinct values**
  (#927). The codegen helper `_aether_duration_repr` returned a shared static
  buffer, so when several durations appeared in a single interpolated string
  (`"${a} ${b} ${c}"`) all `%s` slots pointed at the last-formatted value —
  every slot printed the same duration. The helper now hands out a small ring of
  buffers, so up to eight distinct durations coexist in one printf/snprintf.

## [0.323.0]

### Added

- **Labeled `break` / `continue`** (#893). A `while` / `for` loop can carry a
  label — `outer: while ...` — and `break outer` / `continue outer` then target
  that loop from inside a nested loop (`break` exits it, `continue` jumps to its
  next iteration). This replaces the boolean-flag emulation a faithful C port
  otherwise needs for a nested-loop early exit (the `goto cleanup` idiom). The
  label must be on the same line as the `break`/`continue`; a label naming no
  enclosing loop is a compile-time error; defers in the unwound scopes still run
  (LIFO) before the jump. Unlabeled `break`/`continue` are unchanged.

## [0.321.0]

### Changed

- **Qualified `mod.fn()` surface is available on any import form** (#878). A
  module's qualified call surface (`string.length()`, `math.pow()`) now resolves
  whenever the module is imported in *any* form — bare, selective, or glob —
  like Java's always-legal fully-qualified name. A selective import
  (`import std.math (sqrt)`) is now purely additive: it adds the bare-name
  binding `sqrt(...)` on top of the always-available qualified surface, instead
  of restricting it. Previously a selective import rejected the qualified form
  of any non-selected name (`math.pow` failed under `import std.math (sqrt)`),
  which forced real code to import a module twice (once selective, once bare).
  The per-module selective filter that enforced that restriction is removed;
  export visibility (`exports (…)`) and `hide`/`seal` still gate qualified
  access.

### Fixed

- **Imported `distinct` types now resolve across the module boundary** (#908).
  A `type X = distinct Base` defined in an imported module was never merged into
  the consumer's program, so the distinct-resolution pass never learned `X` —
  every cross-module `expr as X` / `x as Base` failed (`cannot cast X to Base`)
  and codegen emitted an unknown C type `X`. The bug surfaced via the builder-
  child (`_ctx`) path but was broader: any cross-module distinct wrap/unwrap was
  affected. The module merge now pulls imported `distinct` defs into the program
  (bare name, like struct defs), at both the direct-import and transitive-pull-in
  sites, so every reference resolves.
- **Heap double-free returning a string-field struct in a tuple** (#911). A
  `-> (StructWithStringField, err)` constructor whose field was initialized from
  a string variable double-freed at runtime (`free(): invalid pointer`): the
  struct literal hard-coded `._heap_<field> = 1`, claiming ownership even when
  the source variable held a *borrowed* string (`e = s`), so the struct's
  owned-field free ran on a pointer it never owned. The field's heap-ownership
  flag now mirrors the source variable's runtime `_heap_<v>` flag, and the
  variable is disowned (move) so its deferred free is a no-op — exactly one free,
  ASan/leak-clean for the genuinely-heap case. Unblocks the idiomatic "parse a
  record at the boundary, return `(Record, err)`" shape.

## [0.320.0]

### Added

- **`@c_struct` typed overlays — width-correct C-struct field access over a
  raw `ptr`** (#891). Declare a C struct's layout once with explicit offsets
  (`@c_struct stream { length: uint64 @8, slen: uint32 @16, last_id: streamID
  @24 }`); then `ptr as *stream` views a raw pointer through it and `s.length`
  / `s.slen` / `s.last_id.ms` read and write by name. The **accessor width is
  derived from the field type** (`uint32`→4 bytes, `uint64`→8, `ptr`→pointer-
  width, …), so the hand-picked-width footgun behind #868 (a `uint32` read with
  `get_long` pulling adjacent bytes) is gone structurally — the compiler never
  lets you pick the wrong width. Nested overlays add offsets along the chain
  (`s.last_id.seq` → 24+8). It is a pure-Aether lens: no `extern struct`, no
  C struct emitted, no `#include`, no `import std.mem` — it lowers to
  `aether_mem_*` calls over a `void*`, and the C side keeps owning the memory.
  Reuses the existing `expr as *Name` cast and `s.field` syntax. (See
  docs/c-interop.md “`@c_struct` typed overlays”.)
- **`aetherc --emit=effects` — derived per-function effect/purity JSON** (#889).
  Exposes the whole-program effect analysis (#481/#522) for external auditors
  (aeb’s supply-chain veto): `{ "<fn>": { "pure": bool, "extern": bool,
  "reaches": ["fs","net","os"] } }` on stdout (peer of `--emit=ast`/`inspect`,
  no codegen). The result is **derived** from the call graph — not author
  `@no_*` tags an attacker could omit — whole-program transitive (through
  helpers *and* imported modules), per-function, and fail-closed on a raw
  `extern` (treated as reaching every capability, never pure), matching the
  `--with=` gate’s boundary.

## [0.317.0]

### Fixed

- **Glob-imported symbols now resolve across a module boundary** (#896). A
  module that used `import M (*)` and called a glob-brought symbol compiled
  standalone but failed with `Undefined function` once it was imported by
  another module — the merger skipped glob imports when rewriting a consumed
  module's bare references to their prefixed form (only selective/qualified
  imports were rewritten). The merge-time rewrite now treats a glob import's
  selection as the imported module's full export set, so a bare `clean(...)`
  in the consumed module's body lowers to `fs_clean(...)` exactly as the
  selective and qualified forms already did.

## [0.316.0]

### Added

- **`sizeof` / `offsetof` in `const` initializers** (#879). The two layout
  builtins are now accepted in a top-level `const` initializer (and arithmetic
  over them) — `const SIZEOF_T = sizeof(T)`, `const OFF = offsetof(T, field)`,
  `const PAD = sizeof(T) + 8`. They lower to C compile-time constant
  expressions, so a port that mirrors C structs as `extern struct` overlays can
  centralise its offset/size table as named consts that are self-verifying by
  construction (the C compiler folds each value) instead of hand-maintaining
  numbers plus `_Static_assert` drift guards. The general "no function calls in
  a `const` initializer" rule is unchanged; these two builtins are the carve-out.

- **Type/keyword tokens usable as value identifiers** (#880). `ptr`, `byte`,
  `func`, `state` and `after` can now be used as ordinary value identifiers —
  parameter names, local names, struct field names, struct-literal fields, and
  field-access targets — without the `` `name` `` backtick escape. These tokens
  have meaning only in type / declaration-head / statement-head position, so a
  bare occurrence in value position is unambiguously a name. A C→Aether port no
  longer has to rename `ptr`→`ptr_`, `func`→`fn_val`, etc. (`match` and `union`
  stay reserved — `match` heads a match expression; `union` is a C keyword that
  can't be emitted as a C identifier — use the backtick escape for those.)

## [0.315.0]

### Added

- **Address-of operator `&lvalue`** (#890). Prefix `&` takes the address of an
  lvalue and lowers to C's `&` — `&(p as *T).field` → `&((T*)p)->field`,
  `&local.field` → `&local.field`, plus `&local` / `&arr[i]`. The result is a
  pointer (assignable to a `ptr` parameter), so a C extern with a
  `&struct->field` out-param (in-place mutation, sub-field write, resize
  destination) is callable without raw `mem.long_to_ptr(base + OFFSET)` offset
  math.

- **Array-to-pointer decay in pointer context** (#892). A named fixed-size
  array decays to a pointer to its first element when used as an inferred
  binding initializer, a `ptr`-typed argument, or in a pointer comparison
  (C semantics). `ids = static_ids` (with `byte[128] static_ids`) infers `ids`
  as a `ptr`, so a later `ids = heap` / `ids = null` stays legal — the
  stack-buffer-with-heap-fallback idiom. An array *literal* (`x = [1,2,3]`)
  still binds a real array; annotate explicitly to keep the array type.

- **Distinct types: `type Name = distinct Base`** (#480). A zero-cost nominal
  wrapper over a scalar / `string` / `ptr` base — `type USD = distinct float`,
  `type Fd = distinct int`. Lowers to the base C type (no boxing), but the type
  checker treats it as nominally separate: crossing the boundary needs an
  explicit `as` cast (`9.99 as USD` to wrap, `usd as float` to unwrap; `as`
  also does ordinary numeric conversions). Enforced at variable
  declarations/assignments and at call-argument boundaries (a `Fd` parameter
  rejects a raw `int`; an `EUR` is rejected where `USD` is wanted) — the
  capability-token discipline now compiler-checked.

## [0.314.0]

### Added

- **Gradual contracts: `where` clauses on function parameters** (#525). A
  parameter may carry a runtime-checked precondition: `divide(a: int, b: int
  where b != 0)`. It lowers to the same entry guard as `requires` — a violation
  is a hard panic naming the condition (`precondition violation: b != 0 in
  divide`), a programmer-error signal, not a recoverable `(value, err)`. Opt-in
  and gradual: a parameter with no `where` is unchecked; multiple `where`
  params and `and`-composed conditions are allowed; suppressed by
  `--no-contracts` like the other contract checks. (`where` on bindings is a
  tracked follow-up — it needs a binding-syntax decision, since Aether bindings
  are prefix/inferred, not the issue's postfix `let x: T` form.)

## [0.313.0]

### Added

- **Static purity inference + the `__pure(fn)` builtin** (#522). A whole-program
  analysis classifies each function pure/impure: pure means it transitively
  reaches no fs/net/os capability call and mutates no caller-visible state (a
  parameter's pointee or a module global). The compile-time `__pure(funcName)`
  builtin folds to a `true`/`false` constant, so code can branch on purity at
  compile time. Conservative — an extern / unresolved function is treated as
  impure. Reuses the #481 call-graph + capability classification.
- **Per-function effect tags: `@pure` / `@no_fs` / `@no_net` / `@no_os`** (#481).
  A function annotated with an effect tag declares it must not (transitively)
  use the named capability; `@pure` forbids all of fs/net/os. A whole-program
  pass walks the call graph from each tagged function and errors if a forbidden
  capability is reached — e.g. `@no_fs load(...)` calling `file.read_all(...)`,
  directly or through another function. Composes with the build-time
  `--with=fs,net,os` gate (whole-program) as a finer, per-function axis. A raw
  `extern` call is unclassifiable and is not flagged, matching the `--with=`
  gate's boundary.

## [0.312.0]

### Added

- **`@scoped` bindings — opt-in escape analysis** (#521). A `let`/`var`
  declaration annotated `@scoped` (`@scoped let buf = make_buffer()`) declares
  that the value must not outlive its lexical block. The typechecker rejects
  every escape: returning the binding, aliasing it into another binding or
  field, placing it in an aggregate literal, capturing it in a closure, or
  inserting it into a container (`list.add`/`map.put`/…). Only a scalar
  *derived* from it may escape (`return buf.len()`). Not a borrow checker —
  one opt-in annotation that turns a non-escape into a checked invariant.

## [0.311.0]

### Added

- **Raw identifiers: `` `name` `` escapes a reserved keyword for use as an
  ordinary identifier** (#867). A backtick-delimited identifier is always
  lexed as a plain name, so a faithful C→Aether port can keep identifiers
  like `` `reply` ``, `` `message` ``, `` `after` ``, `` `ptr` ``, `` `when` ``
  as parameter, local, struct-field, or function names instead of renaming
  every site. The parameter-position diagnostic for an *unescaped* reserved
  keyword now points at the keyword and teaches the escape (previously a
  misleading "Expected RIGHT_PAREN").
- **`heap.new(T)` supports structs with `string` fields** (#790). The POD-only
  restriction is lifted: a heap-boxed struct now owns its string fields under
  the same model value structs use — a field store adopts the heap string (and
  frees the previous one on reassignment), and `heap.free(p)` releases every
  owned field before freeing the box (a borrowed literal is never freed). The
  `calloc` in `heap.new` zero-inits the ownership trackers. This closes the
  handler-context gap (`struct AppCtx { db: ptr; data_dir: string }`) so such
  contexts no longer need a raw `malloc(...) as *T`.
- **`aetherc --audit-mem`** (#868): lists every raw `std.mem` offset access
  (`mem.get_*`/`mem.set_*`) with the byte width its accessor name implies, then
  exits without generating code. Lets a port author audit each read/write width
  against the C field's actual type — the width-exact accessors already exist,
  but nothing previously surfaced a wrong choice (reading a 4-byte field with
  `get_long` pulled in adjacent bytes).

### Fixed

- **An explicitly-typed integer local keeps its declared width across a bare
  re-bind** (#869). `uint64 v = 0` followed by an annotation-less `v = <int
  expr>` no longer silently re-infers `v` to 32-bit int (the re-bind parsed as
  a fresh declaration and adopted the initializer's type), which previously
  discarded the explicit width and tripped the #698 narrowing guard at the next
  64-bit assignment. The explicit declaration is now authoritative — an int RHS
  widens into the declared type. Fixes silent truncation in the `string2ll`
  accumulator shape (every 10+-digit integer parse).
- **A selective `import std.string (...)` no longer suppresses qualified
  `string.X` calls from merged modules** (#870). When the entry file imported a
  module selectively, the module-merge dropped the bare/non-selective surface
  for the whole compilation unit, so a qualified `string.concat(...)` arriving
  from an imported module that bare-imports `std.string` was rejected with
  E0301. The merge now injects a synthetic bare import for each merged module's
  own bare imports, re-opening the qualified surface (kept out of the
  user-explicit registry, preserving #243 sealed-scope isolation).

## [0.310.0]

_No user-facing language/stdlib changes recorded for this release; see git
history for internal/infra commits._

## [0.309.0]

_The entries previously listed here were misattributed: they shipped across
0.311–0.316 and have been moved to their correct release sections above. See
those sections for the real 0.311–0.316 notes._

## [0.308.0]

### Fixed

- **Module-level mutable global of `string` type now writes the static, not a
  local shadow** — a bare `name = expr` inside a function body assigning to a
  `#701` module-level `global_var` string lowered to a shadowing local instead
  of the file-scope static, so the write was lost. It now resolves to the
  module static. (Part of #861.)

## [0.307.0]

### Added

- **`--emit=lib` now exports module-level `const` declarations** (#854). A
  `--emit=lib` artifact's `aether_lib_meta()` catalog carried functions and
  closures but not module-level constants, so a consumer importing the `.so`
  (no source) failed every `foo.SOME_CONST` reference. Exported scalar/string
  consts (`int`, `long`, `bool`, `float`, `string`) now cross the boundary:
  they're recorded in the catalog (schema **1.2**, forward-compatible — a
  1.0/1.1 reader ignores the new slot) and rehydrated as `const NAME = value`
  in the synthesized binimport stub, so `foo.SOME_CONST` resolves against a
  `.so` exactly as against source, with no call-site changes. `ae lib-info`
  gains a `Constants:` section. Function-only artifacts stay byte-identical at
  schema 1.0. Typed const *arrays* (#745) remain out of scope (skipped, never
  half-emitted).

### Changed

- **Clearer diagnostic for a non-exported module member** (#854). Referencing
  a name an imported module doesn't export (e.g. a constant absent from a
  `.so`'s ABI) reported the misleading `Undefined variable '<module>'`, which
  pointed at the module rather than the member. It now reports
  `error[E0200]: module '<module>' has no export '<NAME>' (not part of the
  module's API / library ABI)`. Scoped to the value/member path; non-exported
  *function* calls already named `<module>.<fn>` and are unchanged.

## [0.306.0]

### Added

- **Embedded Racket and Rhombus host modules** — `contrib.host.racket` and
  `contrib.host.rhombus` embed the Racket CS runtime in-process with a live,
  persistent VM (#852). Racket and Rhombus are the **same VM** (Rhombus is a
  `#lang` on the Racket runtime), so one shared bridge backs both surfaces and
  they share one persistent VM and one string-only k-v map (a key set via
  `racket.set` is read via `rhombus.get`). Surface mirrors the other hosts:
  `evaluate` / `run` / `set` / `get` / `run_sandboxed` /
  `run_sandboxed_with_map` (live shared-map interop) / `init` / `finalize`.
  - **No fork, no patches** — unlike `contrib.host.factor` (which needs a
    forked libfactor), both upstreams are used as-shipped: Racket via a stock
    `make cs` build (it exposes a first-class embedding API), Rhombus via
    stock `raco pkg install rhombus`.
  - **Static-linked, not dlopen** — Racket CS has no shared `libracketcs`
    (upstream refuses `--enable-shared`), so a program importing the bridge
    static-links `libracketcs.a` (from `$AETHER_RACKET_LIB`) plus the runtime's
    system deps; the VM boots from the petite/scheme/racket boot images in
    `$AETHER_RACKET_BOOT_DIR`. The result-returning call is `evaluate` (not
    `eval`) because `libracketcs.a` exports its own `racket_eval`.
  - Experimental and **not in the default `CONTRIB_HOST_LANGS` set** (needs a
    built Racket CS); `make contrib` SKIPs the archive when the embedding
    headers aren't present. Same sandbox caveat as `host/factor`: the VM's own
    GC/JIT/threads aren't contained by the libc gate — rely on the
    process-level sandbox. See `contrib/host/racket/README.md`.

## [0.305.0]

### Added

- **Post-quantum ML-KEM, more NIST curves, and two block ciphers** — a large
  pure-Aether crypto tranche of the Bouncy Castle port (#739), no externs to
  OpenSSL.
  - **`std.cryptography.mlkem`** — ML-KEM / Kyber (FIPS 203), all three
    parameter sets (512/768/1024): `mlkem{512,768,1024}_{keygen,encaps,decaps}`.
    NTT over Z_3329 with Montgomery/Barrett reduction; SHAKE128/256 sampling
    reusing `std.cryptography.sha3`. Aether's first post-quantum primitive.
    Validated **byte-exact against NIST ACVP** vectors: keyGen (ek/dk) and
    encapDecap (ciphertext + shared secret) known-answers for all three
    parameter sets, plus the implicit-rejection (VAL) path. The committed
    integration test pins the ML-KEM-512 keyGen KAT (tcId 1) and the KEM
    round-trip on all sizes.
  - **`contrib.cryptography.p384` / `p521`** — NIST P-384 and P-521 ECDH +
    ECDSA, parameter-swaps of the existing P-256 short-Weierstrass module.
    Validated against the published 2·G doubling vectors + ECDSA round-trips.
  - **`contrib.cryptography.sm4`** (GB/T 32907) and **`contrib.cryptography.des3`**
    (3DES / TDEA) block ciphers, with the same ECB/CBC/CTR + PKCS#7 mode layer
    as `aes`. Validated against the SM4 GB/T 32907 KAT and a BC 3DES KAT.

### Changed

- **`std.bignum` performance** (#233) — internal Karatsuba multiplication (for
  large operands) and Montgomery `mod_pow` (for odd moduli, the RSA/DH hot
  path), with the previous schoolbook code retained as the fallback. Public
  API and all results are unchanged — purely faster. A 2048-bit `mod_pow`
  drops from ~17 s to ~0.2 s (~90×); speeds up RSA and every elliptic curve.
- **`std.cryptography` digests now use `std.bytes.get_le64`/`set_le64`** instead
  of hand-rolled little-endian 64-bit byte assembly (blake2, skein, tiger,
  sha3, argon2 — 12 sites). Behavior-preserving; follow-up to #838.

## [0.304.0]

### Documentation

- **Crypto digest context-ownership contract (#837) is now documented
  consistently across every streaming hash module.** `final_hex` /
  `final_bytes` free the context; `free_ctx` is only for abandoning a
  context *before* finalizing — calling `free_ctx` after a successful
  `final_*` is a double-free. Previously only `std.cryptography.sha2`
  stated this. The rule is now on the `final_*` / `free_ctx` doc-comments
  and in the header usage example of `sha3`, `sm3`, `blake2`,
  `ripemd128` / `ripemd160` / `ripemd256` / `ripemd320`, `whirlpool`,
  `tiger`, and `skein`, and the streaming examples that previously invited
  the broken pattern now carry an explicit `ownership:` note. Comment-only;
  no behavior change.

## [0.302.0]

### Changed

- **Caps-audit (#462): the `std.fs` file handle is now memory-cap
  accounted.** `file_open_raw` routes both the `File` struct and its
  retained path copy through `aether_caps_malloc` (a sandboxed caller can
  craft an enormous filename to inflate filesystem-driven memory), and
  `file_close` releases both with their exact sizes so the accounting
  returns to baseline. The large-file read buffer was already cap-bounded
  (#343/#463). Added two runtime tests: `caps_fs_file_open_close_balances`
  (open + close round-trips to baseline, no accounting drift) and
  `caps_fs_read_denied_past_cap` (#462's acceptance case — a read whose
  buffer exceeds the sandbox's remaining headroom is refused with the
  counter intact). Returned heap strings (path_join/clean/rel results)
  remain plain `malloc` by design — the Aether heap-string machinery owns
  and frees them, so cap-allocating them would drift the counter.
## [0.301.0]

### Added

- **`std.bytes` little-endian 64-bit accessors** — `set_le64(b, index, value)`
  / `get_le64(b, index)`, completing the LE/BE × 16/32/64 matrix (the only
  cell that was missing; `be64` and `le32` already existed). Mirrors the
  `be64` shape: grow-on-write, `-1` on out-of-range read, lossless round-trip
  for any `long`. Removes the hand-rolled byte-by-byte LE64 word assembly that
  6+ crypto modules (Keccak/SHA-3, BLAKE2b, Salsa20/scrypt, Argon2, Skein/
  Tiger, X448/Ed448) each reimplemented. Regression in
  `tests/regression/test_bytes_le64.ae`. (#838)

## [0.300.0]

### Added

- **AEAD, password hashing, more curves, and classic hashes** — a large
  pure-Aether tranche of the Bouncy Castle port (#739), each validated
  against NIST / RFC / Bouncy Castle test vectors. No externs to OpenSSL.
  - **`contrib.cryptography.aes`** gains three more AEAD modes on the
    existing block primitive: **CCM** (NIST SP800-38C), **EAX** (over the
    existing CMAC), and **OCB** (RFC 7253) — `ccm_seal`/`ccm_open`,
    `eax_seal`/`eax_open`, `ocb_seal`/`ocb_open`, each with a constant-time
    tag check and `-1` failure sentinel on tamper.
  - **`std.cryptography.scrypt`** (RFC 7914) and **`std.cryptography.argon2`**
    (RFC 9106 — Argon2d/i/id) password-hashing KDFs. scrypt reuses PBKDF2;
    Argon2 reuses BLAKE2b. `scrypt(...)`, `argon2id`/`argon2i`/`argon2d`
    (+ `_hex`), with optional secret/AD.
  - **`contrib.cryptography.secp256k1`** (Koblitz curve — ECDH + ECDSA, the
    same short-Weierstrass plumbing as P-256), **`contrib.cryptography.x448`**
    (RFC 7748 Montgomery ladder), and **`contrib.cryptography.ed448`**
    (RFC 8032 — SHAKE256-based, 57-byte keys / 114-byte signatures).
  - **`std.cryptography.whirlpool`** (ISO/IEC 10118-3), **`std.cryptography.tiger`**
    (192-bit), and **`std.cryptography.skein`** (Skein-512, Threefish + UBI),
    each with the sm3-style streaming + one-shot API.
  - Correctness-first ports over the variable-time std.bignum (curves) — not
    constant-time/side-channel-hardened; documented in each module header.
    Skein-256/1024 state sizes and Tiger2 are noted as deferred.
- Integration tests: `tests/integration/crypto_{aead_modes,pwhash,classic_hashes,curves2}`.

## [0.299.0]

### Added

- **SHA-3 / SHAKE (FIPS 202)** — `std.cryptography.sha3`, the Keccak hash
  family in pure Aether ported from Bouncy Castle (#739), no externs to
  OpenSSL. Keccak-f[1600] permutation (θ/ρ/π/χ/ι, 24 rounds) under a sponge:
  fixed-length `sha3_{224,256,384,512}_{hex,bytes}` and the extendable-output
  functions `shake{128,256}_{hex,bytes}(data, len, out_len)`, plus a streaming
  `new(variant)` / `update` / `final_*` ctx. Validated against FIPS 202 /
  NIST known-answer vectors (SHA3-224/256/384/512 and SHAKE128/256).
- **BLAKE2b + BLAKE2s (RFC 7693)** — `std.cryptography.blake2`, pure Aether
  from the Bouncy Castle port (#739). 64-bit BLAKE2b (≤64-byte digest) and
  32-bit BLAKE2s (≤32-byte digest), each with plain, variable-length, and
  keyed (MAC) modes plus streaming ctxs: `blake2{b,s}_{hex,bytes}`,
  `*_{hex,bytes}_n` (variable length), `*_keyed_{hex,bytes}`. Validated
  against RFC 7693 reference vectors (plain + keyed).

### Changed

- **`make contrib` / `install-contrib` now build and ship two more host
  bridges** — `contrib.host.factor` (dlopen libfactor; archive builds bare,
  Factor runtime only needed to *run* code) and `contrib.host.aether`
  (Aether-hosts-Aether fork+exec sandbox; libc + in-tree sandbox runtime
  only). Both were already present in the tree but were missing from the
  build catalogue / install set; they now join the other in-process bridges.
  Adds `tests/integration/host_aether/` (compile + run round-trip) alongside
  the existing factor test. `host/{java,go}` remain out of v1 (javac/jar and
  cgo c-archive don't fit the cc→ar pipeline).

### Removed

- **`contrib/climate_http_tests/`** — the Servirtium climate-API record/replay
  harness moved to the servirtium-vcr repo (`integration/climate_interop/`),
  where the VCR tapes + record-then-replay tests live alongside the
  other-language reference implementations. The copy here was a stale,
  byte-identical 2-file subset already excluded from install.

## [0.298.0]

### Added

- **AES-GCM + RSA-OAEP + RSA-PSS** — the remaining mainstream symmetric-AEAD
  and modern-RSA-padding gaps from the Bouncy Castle port (#739), pure Aether,
  no externs to OpenSSL.
  - **`contrib.cryptography.aes`** gains `gcm_seal` / `gcm_open` (AES-GCM,
    NIST SP800-38D): GHASH over GF(2^128), J0/CTR encryption, 16-byte auth tag
    with constant-time compare. Validated against NIST GCM test cases 1/2/4.
  - **`contrib.cryptography.rsa`** gains `encrypt_oaep` / `decrypt_oaep`
    (RSAES-OAEP) and `sign_pss` / `verify_pss` (RSASSA-PSS), RFC 8017 with
    SHA-256 + MGF1. Validated against reference vectors (byte-exact encrypt /
    sign, decrypt / verify, round-trips). `encrypt_oaep` and `sign_pss` take
    the seed / salt as a parameter for determinism (callers pass a CSPRNG
    value in production).
- **`tests/integration/c_import_struct_no_typedef`** — regression guard for
  `aetherc` emitting `struct Name *` (not bare `Name *`) for pointers to
  `@c_import` structs that ship no convenience typedef (the `struct tm` /
  `struct stat` shape). The fix landed earlier via #534; this adds the test
  that guards it.

### Changed

- **Heredocs strip common leading-whitespace indent** (`<<MARKER … MARKER`).
  The longest leading-whitespace prefix shared by every non-blank line is now
  removed, so a heredoc body can be indented to match its surrounding code
  without that indentation leaking into the string. Blank lines don't
  constrain the prefix; relative indentation within the block is preserved.
  The match is character-exact — a space-vs-tab disagreement at a column stops
  the strip there (no shifting past a column where lines differ); to keep a
  literal common indent, indent one line less than the rest. The closing
  marker must be at column 0. Docs in `docs/language-reference.md`; regression
  in `tests/regression/test_heredoc_dedent.ae`.

### Fixed

- **Parser: `call(...) | EXPR` is no longer misread as a trailing closure.**
  A `|` (or `||`) immediately after a function call was unconditionally parsed
  as the start of a trailing-closure parameter list (`func(args) |x| { … }`),
  so a bitwise/logical-OR on a call result — `strlen(s) | 0x80` — failed with
  "Expected IDENTIFIER, got NUMBER". A `|`/`||` is now treated as a trailing
  closure only when the parameter list is followed by `{` or `->`; otherwise it
  is left for the expression parser. Genuine typed-param trailing closures
  (`each(xs) |x: int| { … }`, `map(xs) |x: int| -> x*2`) still parse. (Bit the
  AES/ChaCha and Ed25519 crypto ports.) Regression in
  `tests/regression/test_pipe_after_call.ae`.

## [0.296.0]

### Added

- **Elliptic-curve cryptography — X25519, Ed25519, and NIST P-256** in pure
  Aether on top of std.bignum (the largest single bc-csharp / #739 gap). No
  externs to OpenSSL; each validated against its RFC / NIST test vectors.
  - **`contrib.cryptography.x25519`** — X25519 ECDH (RFC 7748): Montgomery
    ladder over GF(2^255-19). `x25519(scalar, u)`, `base_mult(scalar)`.
    Validated against RFC 7748 §5.2 and the §6.1 Diffie-Hellman round.
  - **`contrib.cryptography.ed25519`** — Ed25519 signatures (RFC 8032):
    twisted-Edwards point ops in extended coordinates, SHA-512-based key/nonce
    derivation, point compression/decompression. `publickey`, `sign`, `verify`.
    Validated against RFC 8032 §7.1 Tests 1-3 (exact signatures + verify).
  - **`contrib.cryptography.p256`** — NIST P-256 / secp256r1: short-Weierstrass
    Jacobian point arithmetic, ECDH, and ECDSA. `scalar_mult[_base]`, `ecdh`,
    `ecdsa_sign`, `ecdsa_verify`. Validated against a NIST ECDH CAVP vector,
    the published 2G doubling, and an ECDSA sign/verify round-trip.
  - These are correctness-first ports over the variable-time std.bignum — not
    constant-time/side-channel-hardened; documented in each module header.

## [0.295.0]

### Added

- **Key derivation, AES MAC/wrap, and a ChaCha20-Poly1305 AEAD** — a large
  pure-Aether tranche of the Bouncy Castle port (#739), all validated against
  RFC test vectors, no externs to OpenSSL.
  - **`std.cryptography.hkdf`** — HKDF (RFC 5869) extract/expand over the
    existing HMAC. Validated against RFC 5869 Test Case 1.
  - **`std.cryptography.pbkdf2`** — PBKDF2 (PKCS#5 v2 / RFC 8018) over HMAC.
    Validated against published PBKDF2-HMAC-SHA256 vectors.
  - **`contrib.cryptography.aes`** gains `cmac` (AES-CMAC, RFC 4493) and
    `key_wrap` / `key_unwrap` (AES Key Wrap, RFC 3394) on the existing block
    primitive. Validated against all four RFC 4493 CMAC vectors and the
    RFC 3394 wrap vector (+ unwrap integrity check).
  - **`contrib.cryptography.chacha20poly1305`** — ChaCha20, Poly1305, and the
    ChaCha20-Poly1305 AEAD (RFC 8439): `chacha20_xor`, `poly1305_mac`,
    `aead_seal`, `aead_open` (constant-time tag compare). The pure-Aether
    counterpart to AES-GCM. Validated against the RFC 8439 §2.5.2 and §2.8.2
    vectors (seal reproduces the exact spec ciphertext+tag; tampered tags are
    rejected).

### Fixed

- **Actors: a string message field retained into actor state no longer
  corrupts.** `SetN(in_n) -> { n = in_n }` stored a raw pointer into the
  message envelope's string, which is freed right after the handler returns,
  so a later message read freed bytes. The retain now copies the borrowed
  string into an owned AetherString (freeing any prior copy). Also fixes the
  defensive-copy workaround `n = string.concat(in_n, "")`, which previously
  failed to compile (`'_heap_n' undeclared`) because actor handlers skipped
  the function-scope heap-string hoist pass. (Reported by aeo.)

## [0.294.0]

### Added

- **AES CBC mode + PKCS#7 padding** in `contrib.cryptography.aes`, layered
  over the existing FIPS-197 block primitive (no externs to OpenSSL).
  - `cbc_encrypt` / `cbc_decrypt` — block-aligned CBC (C_i = E(P_i XOR C_{i-1})).
  - `pkcs7_pad` / `pkcs7_unpad` — RFC 5652 §6.3 padding (a full extra block is
    added when the input is already a multiple of 16; `pkcs7_unpad` returns
    `(plaintext, err)` and rejects malformed padding).
  - `cbc_encrypt_pkcs7` / `cbc_decrypt_pkcs7` — arbitrary-length CBC.
  - Validated against NIST SP800-38A F.2.1/F.2.2 CBC vectors plus PKCS#7
    round-trip and bad-padding-rejection cases.
- **RIPEMD-256 and RIPEMD-320 digests** in `std.cryptography`, ported in pure
  Aether from Bouncy Castle's `RipeMD256Digest` / `RipeMD320Digest`. These run
  the two RIPEMD-128 / -160 lines side by side for a wider (256-/320-bit)
  output at the same security level as -128 / -160. Same streaming + one-shot
  surface as the other digests; validated against the published RIPEMD
  reference vectors.
  - **`std.cryptography.ripemd256`** — 256-bit, 8-word dual-line.
  - **`std.cryptography.ripemd320`** — 320-bit, 10-word dual-line.

## [0.293.0]

### Added

- **Three more digests in `std.cryptography`** — RIPEMD-160, RIPEMD-128, and
  SM3, each ported in pure Aether from Bouncy Castle's
  `RipeMD160Digest` / `RipeMD128Digest` / `SM3Digest` (no externs to OpenSSL
  or any C crypto). Each module exposes one-shot `*_hex` / `*_bytes` and a
  streaming `new` / `update` / `update_bytes` / `final_hex` / `final_bytes` /
  `free_ctx` surface, mirroring `std.cryptography.sha2`.
  - **`std.cryptography.ripemd160`** — 160-bit RIPEMD (the second hash in
    Bitcoin's HASH160). Little-endian dual-line 80-round compression.
  - **`std.cryptography.ripemd128`** — 128-bit RIPEMD; 4-word, 64-round
    dual-line variant.
  - **`std.cryptography.sm3`** — Chinese SM3 (GB/T 32905); 256-bit,
    SHA-256-like big-endian construction.
  - All three validated against published test vectors (empty / `abc` /
    `message digest` / alphabet / multi-block inputs) with streaming-vs-one-shot
    consistency checks; see `tests/integration/crypto_{ripemd160,ripemd128,sm3}`.

## [0.292.0]

### Added

- **FreeBSD sandbox parity + Capsicum / Casper / audit** (`std.capsicum`,
  `std.casper`, `std.audit`) — revives `feat/freebsd-sandbox-parity` onto
  current main. Pure Aether + `#if`-guarded C; degrades cleanly off FreeBSD.
  - **`std.capsicum`** — FreeBSD Capsicum bindings: `available` / `enter` /
    `in_mode` / `rights_limit` / `fcntls_limit` with the full `R_*` / `F_*`
    constant set. `available()` returns 0 off FreeBSD (or on a kernel without
    Capsicum) and `enter()` returns `CAP_UNSUPPORTED` (-2) — portable code
    branches on `available()` before relying on enforcement, never crashes.
    Phase-2 self-sandbox at startup (`runtime/sandbox/capsicum_autosandbox.c`).
  - **`std.casper`** — Casper service delegation (DNS / passwd / sysctl) across
    the capability-mode boundary, with the mandatory two-phase ordering baked
    into the docstring (open service channels *before* `capsicum.enter()`).
    libcasper + per-service libs are resolved by globbing the actual `.so.*`
    filenames (GhostBSD lacks the `.so` linker symlinks); empty → stub path.
  - **`std.audit`** — audit trail for the in-process permission layer
    (`runtime/sandbox/aether_audit.{c,h}`).
  - Runtime sandbox split into `runtime/sandbox/spawn_sandboxed_{bsd,linux,stub}.c`
    (`#if defined(__FreeBSD__)/__linux__`-guarded; the Linux impl moved from
    the old single `runtime/aether_spawn_sandboxed.c`). The LD_PRELOAD
    containment shim now also builds on FreeBSD.
  - Examples: `capsicum-demo.ae`, `casper-demo.ae`, `audit-demo.ae`.

  Ported by an author who got Capsicum right (the two-phase Casper ordering).
  Downstream consumer: the **aeo** orchestrator's host-adaptation / fast-fail
  grammar (`require_capsicum()` / `prefer_capsicum()`) sits directly on this
  surface. **Deferred follow-ups:** automatic Capsicum wiring into
  `spawn_sandboxed` (consumers call `capsicum.enter()` explicitly for now), and
  exposing fds from `std.file` / `std.net` handles so `rights_limit()` works on
  more than raw/inherited descriptors.

## [0.291.0]

### Added

- **`contrib.cryptography.aes` — AES (FIPS-197)** (issue #739) — the
  block-cipher core that unblocks the entire symmetric surface (CBC / CTR /
  CFB / OFB / ECB / GCM / CCM / EAX / OCB / AES-key-wrap / AES-CMAC /
  AES-CTR-DRBG all drive exactly this primitive). Pure Aether, byte-oriented
  FIPS-197 reference form (256-byte S-box + inverse, GF(2⁸) `xtime`) — chosen
  over Bouncy Castle's T-box `AesEngine` for auditability and to avoid the
  cache-timing surface of big T-tables (a T-box/AES-NI fast path is a later
  perf slice). 128/192/256-bit keys. Surface: `new_encryptor` / `new_decryptor`
  / `process_block` (the 16-byte primitive the modes call), plus `ecb_encrypt`
  / `ecb_decrypt` (block-aligned, no padding) and `ctr_xor` (CTR stream).
  Verified against the FIPS-197 Appendix B/C known-answer vectors for all three
  key sizes and the NIST SP800-38A F.5 AES-128-CTR vector (also reproduced via
  `openssl enc -aes-128-ctr`); ASan-clean. Regression:
  `tests/regression/test_aes.ae`. No OpenSSL AES — the round functions, key
  schedule, and modes are all Aether. Padded modes (CBC/PKCS#7), the AEADs
  (GCM/CCM/ChaCha20-Poly1305), and key-wrap are follow-up slices on this core.

## [0.290.0]

### Added

- **`contrib.cryptography.pem` / `.asn1` / `.rsa`** (issue #739) — the format
  layer that turns `std.bignum` into usable RSA, all pure Aether (no OpenSSL
  RSA; the OS CSPRNG via `std.cryptography.random_bytes` is the only extern,
  for PKCS#1 v1.5 padding randomness).
  - **`contrib.cryptography.pem`** — RFC 7468 PEM `parse` / `encode` over a
    self-contained RFC 4648 base64 codec (no extern base64). 64-column line
    wrapping, BEGIN/END label-match validation.
  - **`contrib.cryptography.asn1`** — ASN.1 **DER** parser + emitter over
    `std.bytes`: TLV read with `last_tag`, typed readers/encoders for INTEGER
    (via `std.bignum`), SEQUENCE, OBJECT IDENTIFIER, OCTET/BIT STRING, NULL,
    BOOLEAN. Ported from Bouncy Castle's `asn1/`.
  - **`contrib.cryptography.rsa`** — the first `std.bignum` consumer: key from
    components or PKCS#1 `RSAPrivateKey` DER, raw `m^e`/`c^d mod n` via
    `bignum.mod_pow`, and PKCS#1 v1.5 encrypt/decrypt + sign/verify (over a
    caller-supplied DigestInfo, so RSA stays hash-agnostic). Ported from
    Bouncy Castle's RSA engine + `Pkcs1Encoding` + `RsaDigestSigner`.

  Cross-validated against OpenSSL end-to-end on a real RSA key: our code
  decrypts an OpenSSL PKCS#1 ciphertext, verifies an OpenSSL SHA-256
  signature, and **our v1.5 signature is byte-identical to OpenSSL's**; the
  ASN.1 codec reproduces a real key's DER byte-for-byte on re-encode.
  Regressions: `tests/regression/test_{pem_codec,asn1_der,rsa_pkcs1}.ae`.
  Constant-time decryption, OAEP, PSS, and X.509/PKCS#8 are follow-up slices.

## [0.289.0]

### Added

- **`std.bignum` — `mod_pow` / `gcd` / `mod_inverse` / `is_probable_prime`**
  (issue #739, the layer that completes the BigInteger surface for RSA/DSA).
  `mod_pow` is square-and-multiply modular exponentiation; `gcd` is Euclid;
  `mod_inverse` is iterative extended Euclid (returns `null` when no inverse
  exists, i.e. `gcd(a,m) != 1`); `is_probable_prime(n, rounds)` is Miller-Rabin
  over a fixed set of small witness bases (deterministic for all n < 3.3e24,
  a strong probable-prime test beyond). Bouncy Castle uses Barrett/Montgomery
  reduction for `ModPow` and Montgomery-form Miller-Rabin; the textbook forms
  here give identical results with no extern crypto — the Montgomery fast paths
  are a tracked follow-up optimization. Fuzzed against Python over 366
  `mod_pow`/`gcd`/`mod_inverse` cases (operands up to 128 bits) plus a primality
  sweep over 2..2000 and several 32-bit knowns (incl. the Carmichael number
  561 and the Mersenne prime M31); ASan-clean across a heavy mixed-op loop.
  Regression: `tests/regression/test_bignum_modpow.ae`. This completes the
  arbitrary-precision integer surface (#739 Tier-2 gate) that unblocks
  RSA/DSA/ECDSA/X.509. Still pure Aether — no externs to OpenSSL or any C
  bignum library.

## [0.287.0]

### Added

- **`std.bignum` — multiply / divide / remainder / mod** (issue #739, the
  layer after the foundation). `multiply` is Bouncy Castle's schoolbook
  `Multiply(uint[],uint[],uint[])`; `divide` / `remainder` / `mod` are binary
  long division over the unsigned magnitudes with BC's sign rules (quotient
  truncates toward zero with sign `a.sign*b.sign`; remainder takes the
  dividend's sign; `mod` is always in `[0,|b|)`). The whole surface was fuzzed
  against Python's arbitrary-precision `int` over 414 signed multi-limb cases
  (including the 5-limb / 2-limb division a first shift-division port looped
  on) and is ASan-clean across the intermediate-heavy divmod loop. Regression:
  `tests/regression/test_bignum_muldiv.ae`. Still pure Aether — no externs to
  OpenSSL or any C bignum library. **`mod_pow` / `gcd` / `mod_inverse` /
  `is_probable_prime` remain follow-up layers**; `mod_pow` (the RSA workhorse)
  will use Montgomery reduction rather than this O(n²) division.
- **`std.bignum` — arbitrary-precision integers (foundation layer)** (issue
  #739 slice 11, the BigInteger watershed). Pure Aether, ported from Bouncy
  Castle's `BigInteger.cs`: sign-magnitude representation (32-bit limbs over
  `std.intarr`, big-endian, separate sign in {-1,0,1}). This first layer
  covers `from_bytes` / `to_bytes` (two's-complement **signed**) +
  `from_bytes_unsigned` / `to_bytes_unsigned` (magnitude) over `std.bytes`
  (consistent with the rest of the cryptography port), `from_int`, `compare`,
  `is_zero`, `sign`, `bit_length`, `add`, `subtract`, `negate`, `abs`,
  `shift_left`, `shift_right`. Every operation was cross-checked against
  Python's arbitrary-precision `int` (add/sub/compare/shift over signed
  integers including INT_MIN; signed+unsigned byte round-trips including the
  `80` / `0080` / `00ff` / -128 two's-complement edges). Regression:
  `tests/regression/test_bignum_foundation.ae`. No externs to OpenSSL or any C
  bignum library. **Multiply, divide/mod, mod_pow, gcd, mod_inverse, and
  is_probable_prime are deferred to follow-up layers** — this foundation is the
  Tier-2 gate that, once complete, unblocks RSA/DSA/ECDSA/X.509.

## [0.286.0]

### Added

- **Native HMAC + HMAC-DRBG** (`std.cryptography.hmac`,
  `std.cryptography.drbg`, issue #739) — pure Aether on the native SHA-2.
  - **`std.cryptography.hmac`** — RFC 2104 HMAC over any native SHA-2 digest,
    working entirely in `bytes` buffers (so it's binary-safe for arbitrary
    keys/messages, including the key-longer-than-block hashed-key path).
    One-shot (`hmac_sha256` / `_hex`, `hmac_sha512`, generic `hmac_bytes` /
    `hmac_hex`) and streaming (`new(algo, key, key_len)` → `update` →
    `final_bytes` / `final_hex`). Verified against `openssl dgst -mac HMAC`
    on RFC 4231 vectors.
  - **`std.cryptography.drbg`** — SP800-90A HMAC-DRBG, ported from Bouncy
    Castle's `HMacSP800Drbg.cs`. Deterministic (caller supplies entropy):
    `new(algo, entropy, …, nonce, …, perso, …)` → `generate` /
    `generate_with_input` / `reseed`. Verified against Bouncy Castle's own
    `HMacDrbgTest.cs` SHA-256 vector (two generates match byte-for-byte).
  - Also adds `sha2.update_bytes(ctx, bytes, len)` — a binary-safe streaming
    update the HMAC construction needs.

  No externs to OpenSSL or any C crypto library. Regression:
  `tests/regression/test_hmac_drbg_native.ae`. Bouncy Castle (MIT) attribution
  on the DRBG; HMAC is the generic RFC 2104 construction. CTR-DRBG is deferred
  until native AES lands.

## [0.285.0]

### Added

- **Native SHA-2 family** (`std.cryptography.sha2`, issue #739 slice 2) —
  SHA-224, SHA-256, SHA-384, SHA-512, SHA-512/224, SHA-512/256, implemented in
  **pure Aether** on the Tier-0 foundations (`std.bits` for logical
  shifts/rotates, `std.longarr` for the 64-bit message schedule, `std.bytes`
  big-endian accessors). No externs to OpenSSL or any C crypto library — the
  compression functions, padding, and length encoding are all Aether code.
  Ported from Bouncy Castle's `GeneralDigest` / `LongDigest` /
  `Sha{224,256,384,512}Digest`. Both one-shot (`sha256_hex` / `sha256_bytes`,
  …) and streaming (`new(algo)` → `update` → `final_hex` / `final_bytes`,
  ctx self-freed on finalize). Every digest was cross-checked against
  `openssl dgst` across all block-boundary input lengths (55/56/63/64/65/
  119/120/127/128). Regression: `tests/regression/test_sha2_native.ae` (NIST/
  RFC vectors + streaming-equals-one-shot). Bouncy Castle (MIT) attribution
  per file. This unblocks native streaming-digest + DRBG (Tier 1 items 5/6),
  which the existing OpenSSL-backed digest ctx will be retired in favour of.

## [0.284.0]

### Added

- **Cryptography port Tier 0 foundations** (issue #739, slice 1) — four
  Aether-native stdlib modules every digest/cipher port depends on:
  - **`std.longarr`** — fixed-size 64-bit packed array, the `long`-cell twin
    of `std.intarr` (SHA-512 / Keccak / GCM / Poly1305 / lattice-PQC state).
  - **`std.bits`** — unsigned-bit helpers Aether's signed `int`/`long` can't
    express directly: `lsr32/64` (logical right shift — Aether's `>>` is
    arithmetic), `rotr/rotl 32/64`, `popcount32/64`, `clz32/64`, `udiv/urem
    32/64`, `ucmp64`. Ported from Bouncy Castle's `Integers.cs` / `Longs.cs`.
  - **`std.bytes` big-endian accessors** — `set_be16/32/64` + `get_be16/32/64`,
    the BE twin of the existing `_le*` family (cryptography wire format is mostly
    big-endian). Modelled on Bouncy Castle's `Pack.cs`.
  - **`std.bytes.cursor`** — forward read-position over a bytes buffer
    (`read_u8`, `read_be_u16/32/64`, `read_slice`, `remaining`, `peek`, `eof`,
    `pos`/`seek`); foundation for byte parsers (ASN.1, PEM, OpenPGP).

  Regression tests (`tests/regression/test_{bits,longarr,bytes_be,bytes_cursor}.ae`)
  include vectors ported from Bouncy Castle's `IntegersTest`/`LongsTest`.
  Bouncy Castle (MIT) attribution per ported file plus a new top-level
  `THIRD_PARTY_LICENSES.md`.

## [0.283.0]

### Fixed

- **Module token cap raised 20000 → 100000, buffer heap-allocated**
  (`compiler/aether_module.c`). `module_parse_file` capped imported modules at
  `MAX_MODULE_TOKENS` tokens; a larger module was silently truncated
  mid-token-stream, dropping its tail declarations so callers hit spurious
  `E0301: Undefined function` on the missing symbols. Raised the cap 5× and
  moved the token buffer off the stack to a `malloc`'d array (a fixed
  100k-entry stack array would risk overflow), with NULL-check cleanup and a
  matching free. Regression: `tests/integration/module_token_cap` imports a
  ~2200-function module (>20k tokens) and calls its first, middle, and last
  function — truncation under the old cap left the tail undefined.

## [0.282.0]

### Fixed

- **`fn name(...)` is now a first-class top-level function definition**
  (#791). `fn` is not a reserved word (it doubles as the function-pointer
  type head `fn(...) -> R`), so a top-level `fn name()` previously only
  survived via parse-error recovery: the parser raised "unexpected
  identifier at top level" on `fn`, recovery skipped the token, and
  `name(...)` then parsed as a function. That recovery is silent when a
  module is imported but fatal on a standalone / strict re-parse, so at
  full module-graph scale a re-parsed sibling module that used the `fn`
  spelling (e.g. std.uuid, std.url) surfaced the recovery as a spurious
  top-level parse error attributed to that module. The top-level parser
  now recognises `fn` + name + `(` as a function definition directly, so
  the spelling is first-class and parses identically on every path
  (standalone build, import, and re-parse). `fn`-typed parameters
  (`f: fn(int) -> int`) still parse as types — the definition form is
  distinguished by the identifier between `fn` and `(`.

## [0.281.0]

### Fixed

- **`std.fs`: export `join_clean` and `first_element`.** The two
  path-cleaning wrappers added in `std.fs: add join_clean + first_element`
  were defined but omitted from the module `exports (...)` list, so
  `fs.join_clean(...)` / `fs.first_element(...)` failed at the call site
  with `E0301: Undefined function` (the `tests/integration/fs_join_clean`
  regression went red). Added both to the export list.

## [0.280.0]

### Changed

- **VS Code extension: grammar, snippets, and ergonomics refresh**
  (`editor/vscode/`). Refreshed the TextMate grammar for missing keywords,
  types, and `@annotations`; added range/spread operators, the `make` keyword,
  and snippets; block-comment on-enter + indent rules; a cross-platform
  "Erlang-style" palette; and README palette/install-snippet docs. Also
  rebuilt `out/extension.js` with an activation-leak fix. Editor tooling only —
  no compiler, std, or runtime change.

## [0.279.0]

### Added

- **`heap.new(T)` / `heap.free(p)` — POD struct heap allocation** (issue
  #564; `compiler/parser`, `compiler/analysis`, `compiler/codegen`).
  `ctx = heap.new(AppCtx)` allocates a zero-initialised `AppCtx` on the
  heap and returns `*AppCtx`; fields are read/written through the pointer
  (`ctx.port = 8080`) and the box is reclaimed with `heap.free(ctx)`
  (NULL-safe). Lowers to `((T*)calloc(1, sizeof(T)))` / `free(p)` — the
  guaranteed zero-init the memory-safety review requires. **POD-only**: the
  type must be a struct with no `string` (or other heap-managed) field; a
  non-POD struct is a compile error directing the author to hold heavy data
  as an opaque handle the struct doesn't own. This is the safe first cut
  from the issue's recommendation #1 — richer boxes that own their fields
  need an ownership model (retain-on-store + typed free) specced first.
  Replaces the `malloc(64) as *AppCtx` magic-number pattern with a
  type-safe, self-documenting primitive. Pairs with the
  `defer heap.free(p)` idiom for scope-bounded lifetimes.

## [0.278.0]

### Added

- **`expr!` unwrap-or-trap operator** (`compiler/parser`, `compiler/analysis`,
  `compiler/codegen`). A postfix `!` on a `(value, err)` tuple yields the
  first slot and panics if the trailing (string) error slot is non-empty:
  `h = cryptography.random_hex(n)!` replaces the two-line
  `h, e = ...  return h` discard wrapper. Works on any tuple whose final
  slot is the `string` error (2-tuples, the `(bytes, len, err)` 3-tuple,
  …); the result type is the first slot. Composes anywhere an expression
  is allowed — assignment RHS, call arguments — via a GCC
  statement-expression that evaluates the tuple once. `!` stays the actor
  fire-and-forget operator when followed by a message type (an
  uppercase-leading identifier); the unwrap reading applies everywhere
  else. A non-tuple or string-less-final-slot operand is a compile error.


- **Streaming (incremental) digest context in `std.cryptography`**
  (`std/cryptography/`). `digest_new(algo)` returns an opaque context;
  `digest_update(ctx, data, n)` feeds bytes in pieces; `digest_final_hex(ctx)`
  / `digest_final_bytes(ctx)` finalize (and free the context). `algo` uses
  the same names as `hash_hex` ("md5", "sha256", "sha1", "md4", ...).
  This hashes data that arrives in windows without ever holding it whole —
  a blob store can now compute an upload's ETag as it streams to disk
  instead of reading the stored object back purely to MD5 it (S3 ETag =
  md5-of-object; multipart ETag = md5-of-md5s). `digest_free(ctx)` is the
  abandon-without-finalize cleanup path. Thin veneer over libcrypto's
  `EVP_DigestInit/Update/Final`; returns the "openssl unavailable" error
  shape on builds without OpenSSL.
- **`fs.join_clean(a, b)` and `fs.first_element(path)`** (`std/fs/module.ae`).
  `join_clean` is `path_join` followed by `clean` in one call — the
  cleaned path that actually reaches the filesystem after a caller-
  supplied segment is appended, so `fs.join_clean("bucket", "a/../b")`
  collapses to `bucket/b` rather than leaving the traversal in place
  (path-traversal-defense invariant for object stores). Empty-segment
  handling mirrors `path_join`'s identity behaviour. `first_element`
  returns the leading cleaned path component (`fs.first_element("/a/b")`
  → `"a"`). Together they let downstream blob-store code drop its
  hand-rolled `pathutil.join` wrapper.

### Fixed

- **`import std.fs (*)` (glob import) now carries the real tuple return
  types of `(value, err)` wrappers** (`compiler/analysis/typechecker.c`).
  A glob import registered each short alias by cloning the full symbol's
  type *before* return-type inference ran, so a wrapper whose return type
  is inferred (e.g. `fs.list_dir`'s `(ptr, string)` tuple) left the bare
  alias `list_dir` stuck on a pre-inference `int` placeholder. A
  `list, err = list_dir(...)` then stamped the call's return type as
  `int` and codegen emitted `int _tup0 = fs_list_dir(...)` — a C type
  error. Namespaced (`fs.list_dir`) and selective imports already worked;
  the glob form did not. Import-alias short symbols are now re-synced from
  their inferred full symbols after type inference, so all three import
  forms agree. (fbs-core ask #1.)

## [0.277.0]

### Fixed

- **contrib host bridges: lua + tcl compile against newer Homebrew /
  Tcl-9 libraries** (`contrib/host/lua/aether_host_lua.c`,
  `contrib/host/tcl/aether_host_tcl.c`). Both dlopen bridges name C-API
  functions as struct fields and call them as `g_lib.Fn(...)`, which the
  preprocessor mangles when the library header turned `Fn` into a
  function-like macro:
  - **Lua 5.4** (Homebrew): `luaL_openlibs(L)` is
    `#define`d to `luaL_openselectedlibs(L, ~0, 0)`, so
    `g_lua.luaL_openlibs(L)` rewrote to a non-existent
    `luaL_openselectedlibs` member (macOS build break; Debian's Lua 5.4
    ships it as a real declaration, which is why Linux CI never saw it).
    Fix: `#undef luaL_openlibs` after the headers and dlsym the real
    exported symbol (present in every shipping liblua).
  - **Tcl 9.0** (Homebrew): `Tcl_GetStringResult` and `Tcl_GetString`
    became function-like macros over `Tcl_GetStringFromObj` and are no
    longer exported, so the struct-field calls referenced non-existent
    members. Fix: `#undef` both, resolve the lowest-common-denominator
    real exports `Tcl_GetStringFromObj` + `Tcl_GetObjResult` (present in
    both 8.6 and 9.0), and recompose the two accessors as local helpers.
  Both verified by compiling each bridge against the real (8.6 / 5.3 /
  5.4) headers and against simulated Homebrew-macro headers — clean with
  the fix, reproduces the reported break without it. No behavioural
  change on platforms that were already building (the `#undef`s are
  no-ops where the macro is absent). Surfaced on a macOS/Homebrew
  `make install-contrib` (Lua 5.4.x, Tcl 9.0.3).

## [0.276.0]

### Added

- **Function-pointer struct fields** (#749 Ask A). A struct field typed
  `fn(T1, T2) -> R` now emits the C function-pointer member
  `R (*name)(T1, T2)` (instead of an untyped `void*`), and a call through
  it is a real indirect call: `o.field(args)` for a value struct,
  `p.field(args)` → `p->field(args)` for a pointer-to-struct. This is the
  `dictType` vtable shape that gates the Redis dict.c port (2340 lines)
  and the keyspace command tier. Field PARSING already worked (parse_type
  yields the fn-ptr type); the change is codegen + dispatch: a typed
  field-declarator emitter, plus — because the parser collapses a
  member-access callee to the dotted name `recv.field` and drops the
  receiver — a typecheck branch that recognises `recv.fnptrfield(args)`
  (resolving the field signature off the struct definition, tagging the
  receiver as value vs pointer) and a matching codegen branch that emits
  the indirect call. Single-level receiver (a bare local); the field
  already carries a real C fn-ptr type, so the call needs no cast. Sibling
  of fn-pointer parameters (#750) and typed fn-ptr locals. See
  [docs/language-reference.md](docs/language-reference.md) (Function-
  pointer struct fields).
## [0.275.0]

### Added

- **`@packed` extern structs** (#747 item 1, the Redis sds.c blocker). An
  `extern struct ... @packed { ... }` emits the C body with
  `__attribute__((packed))`, so the layout has no inter-field padding or
  trailing alignment — the `sdshdr8/16/32/64` shape where the length/
  alloc/flags header sits at fixed packed offsets before the string data.
  `sizeof(S)` / `offsetof(S, f)` lower to C and report the packed numbers,
  and a `*S` overlay reads/writes fields at their packed offsets (verified
  by round-trip). `@packed` is mutually exclusive with `@c_import` (a
  header-defined struct's packing is the header's job; combining them is a
  parse error). Bit-width fields and a trailing flexible array still work
  under `@packed`. Note: pure `@c_import` overlays already inherit the
  header's packed layout (no body emitted), so `@packed` is the tool when
  Aether owns the struct body (a pure-Aether port with no C header). See
  [docs/c-interop.md](docs/c-interop.md) (Packed structs).

## [0.274.0]

### Added

- **`longdouble` primitive type** (#749 Ask C, completing the aedis
  core-floor umbrella). Maps to C `long double` — the widest floating
  type — for the exact-decimal numeric paths a C interop layer needs
  (libc `strtold`, INCRBYFLOAT / sorted-set score conversion,
  object.c/util.c number formatting). Supports arithmetic (`+ - * /`),
  comparison, and conversion to/from `int` and `float`; as the widest
  numeric it wins promotion (`longdouble op int` / `op float` →
  `longdouble`). Usable in locals, params/returns, struct fields, and
  `extern` signatures; formatted with `%Lg`/`%Lf` in interpolation and
  `print`. No source literal — values arrive via an extern or by widening
  an `int`/`float`. Spelled as the type name `longdouble` (no new keyword
  token). See [docs/language-reference.md](docs/language-reference.md)
  (`longdouble`).

  With this, the #749 umbrella is fully addressed: fn-pointer parameters
  (#773) and struct fields (#777) for Ask A, the inline `...` C-varargs
  call-through already shipped for Ask B, and `longdouble` for Ask C.

## [0.273.0]

### Added

- **By-value struct returns and stack-struct locals** (#746). A function
  may now declare a by-value struct return type (`make() -> Pair`), and a
  struct can be declared as a stack-allocated local (`Pair p` — no `*`, no
  initializer) and filled field-by-field. Both were parse errors before:
  the `-> StructName` return type fell through the `->` return-type
  disambiguator (an off-by-one in its `{` lookahead — `->` is already
  consumed, so the name sits at offset 0, not 1) into the `-> expr`
  arrow-body path; and `StructName name` had no statement-level
  declaration case (only `*StructName name` and the C-ABI aliases like
  `size_t n`). Both fixes are parser-only — the `IDENT IDENT` stack-local
  case mirrors the existing `*StructName name` pointer path, and codegen
  was already correct (struct return type via get_c_type, `.field` access
  on a value struct, `return p` as a C struct copy). Completes the
  by-value struct set (by-value params already worked), so an all-scalar
  record (a geometry/bounding-box result, the geohash_helper.c shape) can
  be built on the stack and returned without heap allocation or an
  out-pointer. See [docs/language-reference.md](docs/language-reference.md)
  (By-value struct returns and stack-struct locals).
## [0.272.0]

### Added

- **Function-pointer parameters** (#750). A `fn(T1, T2) -> R` parameter now
  lowers to the exact C function-pointer type `R (*name)(T1, T2)` in both
  the prototype and the definition, and a call through it (`cb(a, b)`) is a
  real typed indirect call. Previously a fn-typed parameter collapsed to a
  bare `void*` and the body call emitted invalid C ("called object is not a
  function"); the `as fn(...)` cast only rescued a single in-body callback,
  which didn't scale to multiple callback params or callback-taking helpers.
  This is the parameter form of the existing typed-fn-pointer machinery
  (`as fn(...)` locals, fn-pointer struct fields): the parser/typechecker
  already carried `is_fnptr` onto the parameter, so the fix is codegen-only —
  a fn-ptr declarator emitter for the prototype + definition, plus
  registering the param in the fn-ptr-local registry so the call site emits
  the typed indirect call. Unblocks porting callback APIs (Redis dictScan/
  raxWalk/command-table iteration; qsort, signal handlers, libcurl/sqlite
  hooks). See [docs/language-reference.md](docs/language-reference.md)
  (Function-pointer parameters).

## [0.271.0]

### Fixed

- **Parser: terminate expression continuations at newlines**
  (`compiler/parser/parser.c`). A line-leading token was sometimes folded into
  the previous line's expression as a continuation, so statements that begin a
  fresh line (e.g. a following `[...]` or call) could be mis-grouped. The
  parser now ends an expression continuation at a newline, matching the
  line-oriented statement model; net simplification of the continuation logic.
  Covered by `tests/syntax/test_parser_line_leading_statements.ae` and a new
  `test_parser_newline_bracket` regression.

## [0.270.0]

### Fixed

- **Parser: newline now terminates infix/postfix expression continuation**
  (issue #528; `compiler/parser/parser.c`,
  `tests/regression/test_parser_line_leading_statements.ae`,
  `tests/integration/parser_newline_bracket/`). The old guarded
  recogniser handled `*StructName name`, `*ident = ...`, and a narrow
  `[x, y]` shape, but still let line-leading unary statements like `-x`
  fold into the previous expression. The binary-expression loop now
  stops whenever an infix operator starts on a later source line, and
  postfix indexing does the same for newline-led `[`. Multiline
  continuations remain supported by placing the operator before the
  newline (`total = a +` newline `b`).

### Changed

- **Codegen cleanup: removed the now-dead #759 tuple-struct heap-flag
  transfer, superseded by #762's return-escape contract**
  (`compiler/codegen/codegen_stmt.c`). Two independent fixes for #752
  (struct-with-heap-string-field returned via tuple) both landed: #759
  zeroed the source struct's `_heap_<field>` flags before the
  function-exit `<Struct>_destroy` defer, and #762 (later, more complete)
  suppresses that destroy entirely on the escaping struct and pushes the
  destroy to the *receiving* caller. With #762's suppression the destroy
  never runs in the callee, so #759's flag-zeroing became a dead store
  (`r._heap_s = 0;` on a struct whose destructor is gone). Removed the
  `emit_tuple_struct_heap_ownership_transfer` helper and its sole call
  site; #762's `mark_returned_struct_escaped` on the same tuple-return
  loop is the live, complete mechanism. No behavioural change — verified
  the generated C drops the dead store while the caller-side single free
  is unchanged; both #752 regression tests
  (`tests/integration/issue_752_struct_string_tuple/`,
  `tests/regression/test_struct_string_field_return.ae`) and unit 229/229
  stay green. Pure dead-code removal; keeps the two-mechanisms-on-one-path
  hazard from misleading a future editor.

## [0.269.0]

### Added

- **RAM-bounded streaming request bodies (#626 upload half)**
  (`std/net/aether_http_server.c`, `std/net/aether_http_server.h`,
  `std/http/module.ae`, `tests/integration/http_stream_upload/`). The
  HTTP/1.1 server no longer buffers a large request body whole before
  dispatching the handler. When a request's `Content-Length` exceeds one
  connection buffer (16 KiB), the dispatcher parses only the header
  block and hands the handler a *streaming* request; `http.request_body_read(req,
  off, max)` then pulls each window straight off the socket. Peak server
  memory for a large upload is one window per connection (O(buf + chunk))
  instead of O(Content-Length) — for N concurrent M-byte PUTs that's the
  difference between N×M and N×window bytes live. The canonical loop the
  fbs-core ask sketched works unchanged:
  ```aether
  total = http.request_body_length(req)
  off = 0
  while off < total {
      chunk, n, _ = http.request_body_read(req, off, 65536)
      if n == 0 { break }
      fs.pwrite(out, chunk, n, off)   // stream → disk, never whole in RAM
      off = off + n
  }
  ```
  Small bodies keep the legacy fully-buffered path (random-access offsets,
  no behavioural change); streaming reads must be sequential (the socket
  isn't seekable). A post-handler drain consumes any body bytes the
  handler left unread so the keep-alive connection boundary stays clean
  for the next request (verified: a follow-up GET on the same socket
  after a 3 MiB streamed PUT still answers correctly). New
  `HttpRequest` streaming fields are trailing/ABI-stable (same promise as
  the connection-metadata block). Closes the upload half of #626
  (download/sendfile half shipped earlier); sourced from
  `stdlib-streaming-upload-body-followup.md` (fbs-core, which measured the
  buffered-upload peak the streaming path removes). Integration test PUTs
  3 MiB and asserts bounded streaming + byte-identical SHA-256 round-trip
  + clean keep-alive boundary.

## [0.268.0]

### Added

- **Typed module-level constant arrays** (#745). `const NAME: T[N] = [...]`
  declares a file-scope `static const <T> NAME[]` lookup table with the C
  element width pinned — `T` ∈ {`uint8`, `uint16`, `uint32`, `uint64`,
  `int`, `long`}. Previously the only spelling was `const NAME[] = [...]`,
  which always inferred `int` elements: a uint8/uint16 table cost 4× the
  memory and mismatched a C header expecting a packed `uint16_t[]` (e.g.
  the cluster-slot CRC16 table). The table is allocated once and shared
  across calls (not re-initialised per call). Two compiler changes: the
  top-level `const` parser now accepts a `: T[N]` annotation (and a typed
  scalar `const NAME: T = value`), and the short unsigned width names
  `uint8`/`uint16`/`uint32` are recognised type spellings (siblings of the
  existing `uint64` keyword, emitting `uint8_t`/`uint16_t`/`uint32_t`); an
  integer-element array literal may initialise a narrower integer-element
  typed const array (the explicit, compile-time-constant intent). See
  [docs/language-reference.md](docs/language-reference.md) (Module-level
  constant arrays).

## [0.267.0]

### Fixed

- **Heap string fields of a struct returned from a function are no longer
  corrupted** (#752, follow-up to #634). When a function returned a struct
  with a heap-string field (directly via a single-value builder return, or
  as a tuple element `return r, ""`), the struct's `<Struct>_destroy`
  function-exit defer freed the field even though the struct escaped via
  the return — so the caller read a dangling pointer and the string came
  back as garbage. Int fields survived (no free); a literal-initialised
  string survived (static), which is why the #634 test (int-only) missed
  it. Two-sided fix matching the established return-escape contract for
  plain heap strings: (1) the callee suppresses the struct's destroy when
  it escapes via a return (`return_escaped_struct_vars` → consulted by
  `try_emit_struct_destroy`), transferring ownership to the caller; (2) the
  caller that *receives* an owned struct — a tuple-unpack target or a local
  initialised from a struct-returning call — gets a `<Struct>_destroy`
  defer so the fields are freed exactly once at its scope exit. Verified
  leak-free and double-free-free (ASan + `leaks`) across tuple, single, and
  chained receive-then-re-return forms. Regression test
  `tests/regression/test_struct_string_field_return.ae` asserts the string
  field's *value* (a behavioural gate, unlike the compile-only #634 test).
## [0.266.0]

### Fixed

- **Module-global first-assigned inside a nested block no longer shadowed
  by an uninitialized local** (#744, regression in #701). Codegen's
  branch/loop variable hoisters (`hoist_if_branch_vars`,
  `hoist_if_else_common_vars`, `hoist_loop_vars`) pre-declared a `var`
  global as a fresh function local when its first assignment appeared
  inside an `if`/`while` body — shadowing the file-scope `static`, so
  every write landed in the local and the global kept its initializer
  forever. A silent miscompile whose visibility depended on optimization
  (it corrupted the aedis MT19937-64 PRNG port: a lazily-malloc'd state
  buffer's writes never reached the global). All three hoisters now skip
  names that are module globals — the write is already routed to the
  static by the variable-declaration emitter (`is_module_global_var`), so
  the local must not be emitted. Regression test in
  `tests/integration/module_globals/nested_block_init.ae` (if-body,
  loop-body, and a lazily-initialised counter; exits non-zero if a write
  fails to reach the global).
## [0.265.0]

### Fixed

- **#752: heap-string fields of a struct returned via tuple were freed
  before the caller could read them** (`compiler/codegen/codegen_stmt.c`,
  `tests/integration/issue_752_struct_string_tuple/`). A function
  returning `(R, err)` where `R` contains a `string` field initialised
  from a heap source (e.g. `string.copy(...)`) emitted:
  ```c
  _tuple_R_string _builder_ret = (_tuple_R_string){r, ""};
  /* deferred */ R_destroy(&r);   // ← frees r.s
  return _builder_ret;
  ```
  The tuple literal memcpys `r` into the returned tuple including its
  `.s` pointer; the immediately-following `R_destroy(&r)` defer then
  frees that pointer's buffer while the caller's copy still references
  it. Use-after-free; caller saw garbage in every string field while
  scalar fields survived. New helper
  `emit_tuple_struct_heap_ownership_transfer` walks every tuple-return
  child that is a bare `AST_IDENTIFIER` of struct type and emits
  `<varname>._heap_<field> = 0;` for each heap-string field after the
  tuple literal is built and before the defer drain. The struct's
  `_destroy` defer becomes a no-op for the transferred fields; the
  caller's returned struct retains `_heap_<field> = 1` so its own
  destruct path correctly reclaims the buffer. Sourced from fbs-core's
  attempt to convert `object_get` from a positional 8-tuple to
  `(Object, err)` (issue #752 repro). The fix only touches the
  with-defer multi-value-return path — the no-defer path constructs
  the tuple inline in `return (Tuple){...};` and has no destroy defer
  to race against.

### Added

- **`std.json.from_int(n)` — integer-flavoured number constructor**
  (`std/json/aether_json.c`, `std/json/aether_json.h`, `std/json/module.ae`,
  `tests/integration/json_from_int/`). Sibling of `json.num(value: float)`:
  takes a `long` (full int64 range) and stamps a `JV_FLAG_INTEGER` flag on
  the `JsonValue` so the serializer emits `%lld` instead of `%g`. The
  motivating bug: `json.num(53248000.0)` serialised as `"5.3248e+07"`
  (`%g` switches to scientific notation past ~1e7), wrong for byte-count
  / ID / total fields and lossy past 2^53. Adds a dedicated `integer`
  slot to the JsonValue union (shares the slot, no struct growth) and
  branches the encoder + `json_get_int` + `json_get_number` + clone-tree
  paths on the flag. Parser-side automatic flagging (recognising bare
  integers in input JSON) is a separate follow-up — the value still
  round-trips correctly via the float path as long as it fits in 2^53.
  Sourced from `stdlib-json-integer-value-ask.md` (fbs-core /metrics).

## [0.264.0]

### Documentation

- Docs only change to repair CHANGELOG

## [0.263.0]

### Fixed

- **Codegen: fixed-array locals hoisted out of a loop/branch body are
  declared `T name[N]`, not the invalid `T[N] name`** (PR #753,
  `compiler/codegen/codegen_stmt.c`,
  `tests/regression/test_hoist_array_local_in_loop.ae`). A `byte[N]` /
  `T[N]` local declared inside a loop or branch — and not as the block's
  first statement — is pre-declared at function scope by the var-hoisters.
  They emitted `<get_c_type> <name>;`, but `get_c_type(TYPE_ARRAY)`
  returns `T[N]` (valid only in postfix-declarator position), so the
  hoist produced `unsigned char[8] buf;` and the variable came out
  undeclared at its use sites. New `emit_hoisted_local_decl()`
  special-cases `TYPE_ARRAY` to emit `elem name[N];`; both hoist sites
  route through it. The first-statement-in-block decl path was already
  correct. Found via the aedis Redis port's per-loop scratch buffers.

## [0.262.0]

### Changed

- **stdlib caps-audit — `std.net` HTTP client response & request buffers**
  (#461). Routed the two *unbounded* internal buffers in the HTTP client
  (`std/net/aether_http.c`) through the capability allocator: the response-
  body accumulation buffer (`full_response` — the attacker-controlled DoS
  surface: a malicious server can flood the response) and the request-header
  build buffer (`hdr`). Both are self-contained within `http_request_internal`
  with the live size tracked in a local (`cap` / `hdr_cap`), so the
  realloc-delta accounting and the error-/exit-path frees balance exactly;
  the empty-response fallback records its 1-byte size. The request body was
  already capped (prior PR). Bounded, caller-supplied request fields
  (method/url/header structs and strdups) and the redirect/dechunk/header-
  extract helpers are intentionally left on libc — they cross alloc/free
  boundaries into wrapper/test code where caps accounting can't stay
  balanced, and they are not the unbounded surface. Verified: unit 227/227
  (no accounting underflow), `-Werror` clean, http_client_dechunk +
  http_client_redirects integration tests pass (real round-trip through the
  capped response buffer).
## [0.261.0]

### Added

- **`std.cryptography.random_hex(n)` / `random_base64(n)` — printable-secret
  convenience wrappers over `random_bytes`** (`std/cryptography/module.ae`,
  `tests/integration/cryptography_random_hex/`). Two thin Aether-side wrappers
  that draw `n` cryptographically-secure bytes from the OS CSPRNG and return a
  lowercase-hex (2`n` chars) or RFC 4648 §4 unpadded-base64 string respectively.
  Motivating shape: opaque-bearer-token / API-key minting (e.g. SigV4 secret
  keys), where callers want a printable secret and the "obvious random function"
  should resolve to the secure path — not `std.math` (a clock-seeded PRNG fit
  only for sampling). Composes existing primitives; no new C, no new externs.
  Hex emission uses `std.bytes` (O(n) build vs the O(n²) repeated `string.concat`
  path). Sourced from `stdlib-csprng-secure-random-ask.md` (fbs-core), whose
  request items 1 + 2 (`random_bytes` + UUIDv4) already shipped at 0.213.0;
  this lands the convenience wrappers that were the third bullet of the same
  ask. Aether wrappers only (the existing `cryptography_random_bytes_raw` C
  side is unchanged).

- **`long long` type spelling on extern parameters / returns**
  (`compiler/parser/parser.c`, `tests/integration/long_long_extern/`). When
  the parser sees a second `long` after the first, both are consumed and the
  resulting type carries the verbatim C spelling `long long` instead of the
  default `int64_t`. The underlying TypeKind is still `TYPE_INT64`, so all
  arithmetic and typechecking behave identically — only the emitted C
  declaration text changes. Closes the "Minor, real, cheap" item from
  `aedis-core-floor-feature-asks.md`: a libc / POSIX header that spells a
  parameter as `long long` (e.g. `mstime_t` typedef chains, the MT19937 /
  SHA reference impls bundled with Redis) now matches its Aether-side
  prototype byte-for-byte, removing the gcc "conflicting types" error that
  previously forced the generated TU to compile *without* its header.
  Four-case integration test (single arg + return, large-value retention,
  mixed `long long` ↔ `int64_t` round-trip).

## [0.260.0]

### Changed

- **stdlib caps-audit — `std.os` POSIX allocation sites** (#462). Routed the
  unbounded, plugin-influenced heap allocations in `std/os/aether_os.c`
  through the capability allocator (`aether_caps_malloc/realloc/free`) so a
  sandboxed plugin can't inflate them past a memory cap: the command-output
  capture buffers (`os_exec_raw`, `os_run_capture_raw`,
  `os_run_capture_status_raw`, `os_run_pipe_drain_and_wait_raw`,
  `os_run_full_raw`'s stdout/stderr accumulator), the `os_getcwd_raw` path
  buffer, the `os_execv` argv scratch, and the `os_getenv` value. Caller-owned
  returns keep the documented libc-free / fail-safe-upward-drift contract
  (the `io_read_file_raw` / `io_getenv` model); internal/transient buffers
  free through the cap with their exact live size (realloc-failure paths free
  the *old* size). New `caps_os_getenv_denied_past_cap` unit test asserts an
  env read is refused when the cap is below the value size, with the counter
  unperturbed. Bounded sites (the 1-byte empty-heap sentinel, the
  pointer-only argv/envp arrays) and the Windows-specific helpers
  (`utf8_to_wide`/`wide_to_utf8`/`WBuf`/`win_launch`/drain-thread) are left as
  tracked follow-ups; `std/fs/aether_fs.c` remains. Verified: unit 228/228
  (ASan-clean), `leaks(1)` clean on the os example, full `.ae` regression 0
  failures.

---

Older releases (**0.259.0 and earlier**, down to 0.18.0) live in
[CHANGELOG-archive.md](CHANGELOG-archive.md).
