# Plan: locale-independent float text conversion (`LC_NUMERIC` correctness)

**Status:** IMPLEMENTED on `fix/locale-independent-float-text` (2026-08-08), commit `2ee0371f`.

Four things the plan got wrong or missed, found only by building and running it — recorded here
because each is the kind of thing that reads as fine on paper:

1. **§3's `#error` rule was wrong.** `make ci-wasm` and `make ci-embedded` compile these files from
   explicit lists with toolchains that have no locale machinery, so `#error` would have turned two
   green gates RED. Replaced with a capability macro degrading to a plain passthrough — correct by
   construction there, and the codebase's own `AETHER_HAS_THREADS` idiom.
2. **§3.2's `pthread_once` does not exist on Win32.** `aether_thread.h` maps pthreads to Win32 but
   omits `pthread_once`/`PTHREAD_ONCE_INIT`. Linux compiled clean; **winbaz caught it**. Replaced
   with an atomic acquire/release lazy init.
3. **A latent restore bug in the `uselocale` bracket.** The first draft guarded the restore with
   `previous != NULL`, but `uselocale` returns `LC_GLOBAL_LOCALE` (not NULL) for a thread that never
   set one — verified empirically. The guard happened to be true so the bug was latent, but it was
   wrong in intent and would have stranded a thread in the C locale. Now restores unconditionally,
   with a test that the caller's locale survives a shim call.
4. **`std.number` is downstream of the fix, not independent of it.** §7 listed it as "out of scope";
   in fact it calls `string_from_double` as a canonical intermediate, so the fix *protects* it (that
   intermediate could previously have been `3,14`). Verified byte-identical and now asserted in the
   test.

Also worth knowing for the next locale-adjacent task: `localedef` + `LOCPATH` gives a comma-decimal
locale with no root (§4.3a), and the pre-existing `integration_http_server_h2` failure on `main` is
unrelated — see `asks/h2-50-stream-stress-framing-layer-error.md`.
**Origin:** external review of PR #1429 (IEEE-754 binary64 text-conversion prerequisite for Phase 3.5 of #863) by a Cucumber maintainer.
**Scope:** `std/string`, `std/json`, `runtime/` — *not* `contrib/i18n` and *not* `std/number`. This
is **not** an i18n feature; it is close to the opposite — a correctness fix making machine text
**locale-*independent***. See "Framing" below; getting that distinction right is the whole point.
(Originally drafted as `changes_to_il8n.md`, which misled on both counts.)

---

## 1. Framing: two kinds of number text, and we conflated them

Aether has two entirely separate needs, and PR #1429 accidentally straddled them:

| | **Machine text** | **Human text** |
|---|---|---|
| Users | `string.from_double`, `string.to_double`, JSON, config files, wire protocols, `${}` interpolation | `std.number` (Phase 4 of #863) |
| Correct decimal point | **always `.`** | whatever the locale says |
| Correct grouping | **never** | locale-defined |
| Depends on `LC_NUMERIC` | **must not** | must, via explicit locale argument |

`std/string` and `std/json` are firmly **machine text**. Their output goes into files and onto
sockets and must be byte-identical regardless of what locale the host process happens to be in.
Today they inherit `LC_NUMERIC` from the ambient process locale, which makes them
environment-dependent. That is the bug.

`std/number` (Phase 4) is human text and correctly takes an explicit locale argument — it is
**not affected by this plan** and must not be "fixed" to use the C locale.

---

## 2. The defects

### 2.1 Parse side is entirely unprotected — this is a shipped bug

`std/string/aether_string.c:1066`:

```c
double val = strtod(data, &endptr);   // obeys LC_NUMERIC, no handling whatsoever
```

`strtod` is locale-sensitive. Under `de_DE.UTF-8` (decimal point `,`):

- `strtod("3.14")` consumes only `"3"`, returns `3.0`, leaves `endptr` at `"."`.
- The trailing-garbage check at `:1072` then sees a non-space remainder and returns **0 (parse
  failure)**.

So `string.to_double("3.14")` **fails outright** in a comma-decimal locale. The round-trip
guarantee that PR #1429 exists to establish does not hold in the very locales the format-side
comment claims to have handled. `string_to_float_raw` (`:1046`, `strtof`) has the identical hole.

This is the finding the reviewer's questions lead to but did not themselves state. It is the
strongest item in this plan and the reason the whole thing is worth doing now.

**When does it actually fire?** Be precise about this, because it shapes the test design (§4.3)
and the PR narrative. A C program starts in the `C` locale; `LC_NUMERIC`/`LANG` in the environment
are **inert** until something calls `setlocale(LC_ALL, "")` — and nothing in `runtime/`, `std/`,
or `contrib/` ever calls `setlocale` (verified by grep, 2026-08-08). So a standalone Aether binary
is safe today *by accident*. The bug fires when Aether code is **embedded** in a host that has set
a locale — GUI toolkits (`gtk_init` calls `setlocale(LC_ALL, "")`), PHP, many C/C++ applications —
which is *exactly* the scenario the PR #1429 comment names as its design target ("callers embedding
Aether may have selected a non-C locale"). The format side was half-defended for that scenario;
the parse side not at all. "Safe by accident" is also fragile: the day anything in the runtime or
an app calls `setlocale` for legitimate i18n reasons (a #863 consumer is the obvious candidate),
every standalone binary inherits the bug.

### 2.2 Format side normalizes post-hoc instead of formatting correctly

`std/string/aether_string.c:842-860` formats under the caller's locale, then rewrites the decimal
point afterwards by `strstr`-ing for `localeconv()->decimal_point`. This works for the common
cases but is fragile:

- It rewrites only the **first** occurrence. Fine for `%.17g`; a trap for any future reuse.
- It assumes `localeconv()` accurately describes what `snprintf` just did. True in practice,
  but it is an inference about libc state rather than a guarantee.
- It is a workaround for not controlling the locale, in a codebase that *can* control the locale.

### 2.3 Sibling sites with no handling at all

| Site | Call | Direction | Currently handled? |
|---|---|---|---|
| `std/string/aether_string.c:814` | `snprintf("%g")` in `string_from_float` | emit | **no** |
| `std/string/aether_string.c:842` | `snprintf("%.17g")` in `string_from_double` | emit | post-hoc rewrite |
| `std/string/aether_string.c:1046` | `strtof` in `string_to_float_raw` | parse | **no** |
| `std/string/aether_string.c:1066` | `strtod` in `string_to_double_raw` | parse | **no** |
| `std/json/aether_json.c:~802` | `strtod` slow path | parse | **no** |
| `std/json/aether_json.c` | float emit path (locate during impl) | emit | **no** |
| `runtime/aether_runtime_types.c:82` | `snprintf("%f")` | emit | **no** |
| `runtime/aether_runtime_types.c:98` | `atof` | parse | **no** |

JSON is the most user-visible: RFC 8259 mandates `.` as the decimal separator. An Aether service
running under a European system locale currently emits and rejects non-conforming JSON.

Audited and **not** sites (recorded so nobody re-checks): `string_format` (`aether_string.c:1128`,
variadic `vsnprintf`) has no in-tree callers passing float conversions — C-only surface, flag it
in review if one ever appears; `string_format_list` (`:1173`, the Aether-facing `string.format`)
is pure `{}` substitution over pre-stringified args, not locale-sensitive; `string_from_int` /
`string_from_long` / `string_from_int_radix` are integer-only (`LC_NUMERIC` affects only the radix
character and `%'` grouping, neither of which they use).

### 2.4 The rationale comment is wrong on its facts

`std/string/aether_string.c:847-849` says the process-global locale must not be mutated because
"callers embedding Aether may have selected a non-C locale." That conclusion is right; the
reasoning given in review — "Aether has no threads" — is **false**, and we should not repeat it:

- `runtime/actors/aether_actor_thread.c` — actor threads
- `runtime/scheduler/multicore_scheduler.c:1054` — multicore scheduler threads
- `std/worker/aether_worker.c:276,318,358` — worker pool
- `std/net/aether_http_server.c:3617,3777` — HTTP accept threads
- `std/net/aether_http_pool.c:100` — connection pool
- `std/http/proxy/aether_proxy_health.c:187` — health checker

`setlocale` is process-global and not thread-safe against concurrent readers. With the above, a
`setlocale`-based fix would be a data race in our *own* runtime, quite apart from the embedding
argument. Both reasons stand independently; the comment should cite the durable one (embedding +
our own threads) rather than a claim that is untrue.

---

## 3. Chosen approach

**A locale-independent conversion shim, used by every machine-text float site.**

Backwards compatibility is explicitly **not** a constraint here (user decision, 2026-08-08). The
current behaviour is a bug in every locale where it differs from the new behaviour, so there is
nothing worth preserving. Concretely this means:

- **No compatibility fallback backend.** Earlier drafting kept the post-hoc `localeconv` rewrite as
  a last resort. Drop it — a silent third code path that behaves subtly differently is exactly the
  failure mode this plan exists to remove.
- **CORRECTION (found during implementation, 2026-08-08): `#error` on "neither backend" is wrong
  and would break two green CI gates.** An earlier draft said to `#error` when a platform has
  neither `_l` calls nor `uselocale`. But `make ci-wasm` (emcc) and `make ci-embedded`
  (`arm-none-eabi-gcc`, newlib, `-ffreestanding`) both compile `std/string/aether_string.c` and
  `runtime/aether_runtime_types.c` from explicit file lists (`Makefile:2509`, `:2576`), and newlib
  has neither backend. `#error` would turn both gates RED for a bug that **cannot occur** on those
  targets — freestanding newlib has no locale support to go wrong, and emcc's musl pins
  `LC_NUMERIC` to `C`.
  The correct shape is the codebase's own established idiom: a **capability macro** with graceful
  degradation, exactly like `AETHER_HAS_THREADS` / `AETHER_HAS_FILESYSTEM`
  (`runtime/config/aether_optimization_config.h:44-68`) and the stub path in
  `runtime/utils/aether_thread.h:24`. Define `AETHER_HAS_LOCALE_CONV`; when it is 0 the shim
  compiles to a direct `snprintf`/`strtod` passthrough, which is *correct by construction* on a
  platform whose `LC_NUMERIC` can never be anything but `C`. Document that reasoning at the
  `#else` so it cannot be mistaken for the fallback we rejected.
- **No opt-in flag, no legacy mode.** The old post-hoc block at `aether_string.c:847-860` is
  deleted outright, not gated.
- **Behaviour changes are allowed to be visible.** `string.to_double("3.14")` starts succeeding
  under `de_DE.UTF-8` where it used to fail; JSON output changes separator in those locales. Both
  are the fix, not a regression. Document them in the CHANGELOG as fixes, not as breaking changes.
- Free rein to change the internal C signatures (`runtime/aether_runtime_types.c:98`'s `atof` →
  `aether_c_strtod` with real error reporting) without a deprecation shim.

Two backends, selected at compile time:

1. **`_l`-suffixed libc calls** — `snprintf_l`, `strtod_l`, `strtof_l` (macOS, BSD) and
   `_snprintf_l`, `_strtod_l` (Windows/MSVC). Best option: no thread state is mutated at all, the
   locale is an explicit argument.
2. **`uselocale` bracket** — glibc and musl, which have `strtod_l` but not `snprintf_l`. Swap in a
   cached `locale_t` for the duration of the call, restore on **every** return path.

Rationale for the shim rather than fixing each site inline: eight call sites across three
subsystems, two backends, and a strict "restore on every path" obligation. Inline is how you get
seven correct sites and one leak.

### 3.1 Platform matrix

**The backend choice is per-function, not per-platform** — glibc has `strtod_l`/`strtof_l` (GNU
extension, needs `_GNU_SOURCE`) but no `snprintf_l`, so on glibc the parse functions use backend 1
and only the emit function needs the `uselocale` bracket. Structure the `#if` ladder per function.

| Platform | `strtod_l` | `snprintf_l` | `uselocale` | parse / emit backend |
|---|---|---|---|---|
| glibc (Linux, CI) | yes (`_GNU_SOURCE`) | no | yes | 1 / 2 |
| musl (Alpine) | verify (see note) | no | yes | 1-or-2 / 2 |
| macOS | yes | yes | yes | 1 / 1 |
| FreeBSD / GhostBSD | yes | yes | yes | 1 / 1 |
| MSVC | `_strtod_l` | `_snprintf_l` | no | 1 / 1 (`_`-prefixed) |
| MinGW / MSYS2 | `_strtod_l` ✓ | `_snprintf_l` ✓ | no | 1 / 1 (`_`-prefixed) — **VERIFIED on winbaz** |

musl note: musl pins `LC_NUMERIC` to `C` regardless of what `setlocale` is told, so the bug cannot
fire there and either backend is a no-op wrapper; don't burn time on the Alpine row, just make sure
it compiles.

**MinGW question SETTLED — verified empirically on winbaz, 2026-08-08.** A probe using
`_create_locale(LC_NUMERIC, "C")` + `_snprintf_l` + `_strtod_l` + `_strtof_l` + `_free_locale`
compiled clean (`-Wall -Wextra`) and ran correctly under **both** MSYS2 environments, GCC 16.1.0:

- **MINGW64** (what `.github/workflows/ci.yml:292` builds under) — PROBE OK
- **UCRT64** — PROBE OK

The probe also demonstrated the bug live: with the ambient locale set to German, plain
`snprintf("%.2f", 3.14)` printed `3,14` while `_snprintf_l` with the C locale printed
`3.1400000000000001` and `_strtod_l("3.14")` consumed all four chars. So Windows takes backend 1
(`_`-prefixed) unconditionally; no `#error` branch will be reachable on any supported platform.

Windows locale-name detail for the §4.3 test's candidate list: `setlocale(LC_ALL, "de-DE")`
succeeds on UCRT64 but fails on MINGW64 (msvcrt), which needs `"German_Germany.1252"`. Try
`de-DE`, then `German_Germany.utf8`, then `German_Germany.1252`, and SKIP if all return NULL.

### 3.2 Cache the `locale_t`

Do **not** `newlocale`/`freelocale` per call — `string.from_double` sits on hot paths (JSON
serialization, string interpolation). Create one process-lifetime C locale on first use. The
same caching applies to **both** handle types: POSIX `locale_t` (`newlocale`) and Windows
`_locale_t` (`_create_locale`) — one static handle each, never per-call.

**Do NOT use `pthread_once` (found on winbaz during implementation, 2026-08-08).**
`runtime/utils/aether_thread.h` maps pthreads onto Win32 but provides **no**
`pthread_once`/`PTHREAD_ONCE_INIT` — the first MinGW build failed with *"unknown type name
`pthread_once_t`"*. Rather than extend that shared header (wide blast radius for one call site),
the shim uses an atomic relaxed load/store lazy init: if two threads race, both build a "C" locale
and one handle is dropped — at most one small one-per-process allocation, never corrupt state,
since each handle is independently valid and immutable. Falls back to a plain `if (!ptr)` guard
when `AETHER_HAS_ATOMICS` is 0 or the build is single-threaded.
`freelocale` at exit is optional; a single never-freed process-lifetime handle is not a leak worth
an atexit hook, but confirm it does not trip the leak-detection CI matrix — if it does, register
it with the existing runtime shutdown hook rather than suppressing it.

### 3.3 Threadless builds

`AETHER_NO_THREADING` / `aether_thread.h` stubs pthreads out (see `std/worker/aether_worker.c:28`).
The once-init must degrade to a plain `if (!initialized)` there. Check whether the embedded
`!AETHER_HAS_FILESYSTEM` profile also needs a stub path.

---

## 4. Implementation

### 4.1 New shim

Create `runtime/aether_locale_num.h` + `.c` (runtime, not std — `runtime/aether_runtime_types.c` is
one of the consumers and must not depend on `std/`).

Surface:

```c
/* Locale-independent ("C" locale) float text conversion.
 *
 * Machine text only: wire formats, JSON, config, interpolation. Always uses
 * '.' as the decimal separator and never groups, regardless of the ambient
 * process locale. Human-facing formatting belongs in std.number, which takes
 * an explicit locale and must NOT be routed through here. */

int    aether_c_snprintf_double(char* buf, size_t n, const char* fmt, double v);
double aether_c_strtod(const char* s, char** endptr);
float  aether_c_strtof(const char* s, char** endptr);
```

`fmt` is restricted to a caller-supplied float conversion (`"%g"`, `"%.17g"`, `"%f"`); it is not a
general printf passthrough. Document that, and assert/reject anything with more than one conversion
specifier if cheap to do so.

Each backend must:

- preserve `errno` semantics exactly as the callers expect (`ERANGE` checks at
  `aether_string.c:1048`, `:1068` depend on it) — set `errno = 0` before, and do not let the
  locale save/restore clobber it after;
- restore the previous locale on **every** return path including error returns;
- be a straight passthrough when the backend is `_l`-based (no state to restore);
- degrade to a plain `snprintf`/`strtod` passthrough when `AETHER_HAS_LOCALE_CONV == 0` (WASM,
  newlib/embedded) — correct by construction there, **not** the rejected post-hoc fallback (§3).

Two `_l`-backend footguns the implementer must not macro-alias over:

- **Argument order differs.** BSD/macOS: `snprintf_l(buf, n, loc, fmt, ...)` — locale **before**
  format. Windows: `_snprintf_l(buf, n, fmt, loc, ...)` — locale **after** format. Write the two
  wrappers out explicitly; a `#define snprintf_l _snprintf_l` style alias compiles on neither or
  miscompiles on one.
- **Windows `_snprintf_l` is not C99-conformant on truncation**: it returns a negative value and
  does **not** NUL-terminate the buffer, where C99 `snprintf` returns the would-be length and
  always terminates. The shim's Windows emit path must force `buf[n-1] = '\0'` and normalise the
  return so callers see uniform C99-style semantics (`string_from_double` checks
  `written < 0 || written >= sizeof(buffer)` — both arms must keep working identically on every
  platform). In practice truncation is unreachable for `%.17g` in a 128-byte buffer, but
  `runtime/aether_runtime_types.c:82`'s `%f` can exceed its buffer for large magnitudes, so the
  semantics matter.

### 4.2 Call-site changes

Replace at all eight sites in §2.3. Delete the post-hoc `localeconv` block at
`aether_string.c:847-860` outright — with no compatibility fallback it has no remaining home, and
`<locale.h>` may then be droppable from `aether_string.c`'s includes (`:12`) if nothing else uses
it.

Rewrite the comment at `:847` to state the durable rationale: *these conversions are machine text
and must be locale-independent; the process-global locale is not ours to mutate because Aether
runs its own threads and may be embedded in a multithreaded host.*

`runtime/aether_runtime_types.c:98` uses `atof`, which has no error reporting. Switch to
`aether_c_strtod` while there; that is a small behavioural improvement, so call it out in the PR
rather than sliding it in.

### 4.3 Tests

**a) `tests/regression/test_string_double_locale.ae`** — run the existing round-trip corpus
(`test_string_double_roundtrip.ae`'s `check_roundtrip` shape) under a non-C locale.

**Mechanism — the test must call `setlocale` itself.** An earlier draft proposed a driver script
re-execing the test with `LC_NUMERIC=de_DE.UTF-8` in the environment. **That does not work**: a C
program starts in the `C` locale and locale env vars have no effect unless the process calls
`setlocale(LC_ALL, "")`, which nothing in the Aether runtime does (§2.1). The test binary would run
in the C locale and pass vacuously — test theatre of exactly the kind §4.3(b) warns about.

Instead the test sets the locale in-process via a helper **exported from the shim itself**
(`runtime/aether_locale_num.c` ships in every binary, so there is nothing extra to link):

```c
/* TEST-ONLY. Mutates the PROCESS-GLOBAL locale — the exact thing the rest of
 * this file exists to avoid. Call it only from single-threaded test mains,
 * before any actor/scheduler/worker threads exist. Returns 1 if the locale
 * took, 0 if it is not installed (caller prints SKIP). */
int aether_test_setlocale(const char* name);   /* wraps setlocale(LC_ALL, name) */
```

with a plain `extern aether_test_setlocale(name: string) -> int` in the test. Helper-in-shim
rather than a raw `extern setlocale` because the raw route has two traps: `LC_ALL`'s numeric value
is libc-specific (**6** on glibc but **0** on BSD/macOS/Windows — a hardcoded constant silently
sets the wrong category somewhere), and the emitted C prototype for `setlocale` can collide with
`<locale.h>` in generated code, forcing a `libc_names[]` whitelist detour (memory:
`feedback_aether_libc_whitelist`) for no benefit. The helper contains both problems in one
C-compiled line, and its int return is tidier at the Aether boundary than a NULL-checked ptr.

This is **better** than the env-var driver on every axis:

- **SKIP detection is free**: returns 0 when the locale isn't installed → `println("SKIP ...:
  de_DE.UTF-8 not installed")`, exit 0 — no `locale -a` parsing, no shell sibling, no
  `.ae`-discovery prune-list entry (memory: `feedback_integration_test_ae_discovery_prune`).
- In-process, so it works identically under the nightly and a hand run.
- It *proves the trigger*: the test reproduces the embedded-host scenario (§2.1) rather than
  simulating it.

Locale-name spelling varies: try `de_DE.UTF-8` then `de_DE.utf8` on POSIX (distro-dependent), and
on Windows `de-DE` → `German_Germany.utf8` → `German_Germany.1252` (§3.1's winbaz finding: UCRT64
accepts the BCP-47 form, MINGW64/msvcrt does not). SKIP only after the whole candidate list fails.

The local box currently has only `C`/`C.utf8`/`en_US.utf8` (verified via `locale -a`), so expect
SKIP locally until the CachyOS box is provisioned (b).

**Rootless local testing (found during implementation, 2026-08-08).** You do NOT need sudo or a
provisioned box to exercise the real bug — `localedef` compiles a locale into any writable
directory and `LOCPATH` points libc at it:

```sh
localedef -i de_DE -f UTF-8 "$PWD/loc/de_DE.UTF-8"     # no root needed
LOCPATH="$PWD/loc" LC_ALL=de_DE.UTF-8 locale -k decimal_point   # => decimal_point=","
LOCPATH="$PWD/loc" ./my_test                            # setlocale("de_DE.UTF-8") now succeeds
```

This was used to validate the shim locally and should be used in dev; it does **not** replace the
nightly gate (b), because `LOCPATH` requires the harness to set it, which a CI runner won't do by
itself. The SKIP convention matches
`tests/regression/test_fs_move.ae:129` and `test_glob_dir_prefix.ae:22`; the SKIP line must be
visible in logs — a test that silently passes because it never ran is worse than no test.

**b) Make it actually run somewhere — the CachyOS nightly, NOT `ci.yml`.**

A test that always skips is theatre, so it has to run on a box that really has the locale. That box
is the **CachyOS nightly** (`tests/cachyos/nightly.sh`), and GitHub Actions is deliberately left out
of it (user decision, 2026-08-08).

Why not `ci.yml`:

- `.github/workflows/ci.yml` has eleven jobs. The matrix job (`:19`) spans macOS and Windows where
  `locale-gen` doesn't exist, so it would need an OS guard on the most complex job; the Linux-only
  jobs (`:224`, `:261`, `:387`, ...) are valgrind / embedded / cross-compile, none a natural home
  for a stdlib regression test.
- On the stock runner image the test would only ever print SKIP unless a `locale-gen` step is
  added, so it would be destabilising the merge-blocking path in exchange for a test that does
  nothing by default.
- One hand-picked locale on one platform guards the specific `de_DE` regression, not the class of
  bug. The invariance check in (c) below is the part that generalises.

Why the nightly:

- We provision the box, so locale generation is one-time setup, not per-run cost across eleven jobs.
- Its stated purpose is covering what the portable CI can't (`tests/cachyos/README.md:4-6`).
- It has a **fail-on-missing-deps** gate, so an absent locale turns the run **RED** rather than
  silently skipping — the exact property whose absence let this bug ship.

Accepted trade-off, state it plainly in the PR: detection moves from per-PR to nightly. Proportionate
for a bug that sat unnoticed since #1429 merged.

**Wiring it in** (`tests/cachyos/nightly.sh`):

1. Add a `locale` kind to `have_dep` (`:94`), alongside the existing `cmd` and `lib`:

   ```sh
   locale) for L in "$@"; do locale -a 2>/dev/null | grep -qix "$L" && return 0; done ;;
   ```

   Match case-insensitively and accept both spellings — `locale -a` prints `de_DE.utf8` while the
   generation input is `de_DE.UTF-8`.

2. Add to the `require_deps` spec list (`:110`):

   ```sh
   "de_DE.UTF-8 locale|locale|de_DE.utf8|de_DE.UTF-8"  \
   ```

3. Provisioning note in `tests/cachyos/README.md`: uncomment `de_DE.UTF-8 UTF-8` in
   `/etc/locale.gen` and run `locale-gen` (Arch ships `glibc` with the locale *sources* present but
   ungenerated — same as this dev box, verified: `locale -a` lists only C/C.utf8/en_US.utf8 while
   `/usr/share/i18n/locales/de_DE` exists).

The Aether-side test still SKIPs cleanly when the locale is absent (§4.3a) — that keeps it harmless
for anyone running `make test` locally. The nightly's dep gate is what makes a missing locale loud
in the one place we control.

**c) Locale-invariance test — the part that generalises.** Beyond asserting correct output under
`de_DE`, assert output is **identical** across every locale available on the box: for each
candidate locale name, `aether_test_setlocale(name)` (skipping failures — same helper as (a)), format the
corpus, and diff against the C-locale baseline captured first; require zero differences. Candidate
list = `locale -a` output when obtainable (`os.run`), else a hardcoded candidate set. This degrades
to a tautology on a C-only box (which is honest), and it catches the *next* locale-sensitive libc
call someone adds — which the single-locale test would not.

**d) JSON round-trip under the same locale** — emit and re-parse a float-bearing document, assert
byte-exact `.` separators. This is the RFC 8259 conformance guard.

**e) Direct unit test of the shim** in C, if there is an existing C-level test hook; otherwise
cover it through the Aether tests.

---

## 5. Verification

1. `make ci` green (10 steps).
2. `HARDEN=1 make ci` — this touches `std/` and `runtime/`, so the **full CI matrix matters**:
   per memory `feedback_skip_actions_scope`, **`[skip actions]` must NOT be used on any commit in
   this branch.**
3. Leak-detection CI must stay clean — check the cached `locale_t` (§3.2).
4. macOS ARM64 leaks gate — this is the platform that historically catches our C-side slips
   (memory: `feedback_macos_realloc_zero_leaks`).
5. **winbaz** (`ssh winbaz`, MSYS2 login shell — a bare Git-Bash ssh session can't run the MSYS2
   gcc) — confirm the real shim compiles under MINGW64 and the float round-trip and locale tests
   behave. The §3.1 probe validated the primitives; this validates our actual code.
6. **CachyOS nightly** — after provisioning `de_DE.UTF-8`, confirm the dep gate reports
   `dep OK: de_DE.UTF-8 locale` and the locale tests actually execute (not SKIP). A green nightly
   whose new test skipped is not verification. Note `ci.yml` is intentionally untouched (§4.3b).
7. Confirm `std/number` (Phase 4) output is **unchanged** — regression-test it explicitly. If this
   change alters human-facing formatted numbers, the fix has leaked across the machine/human line
   and is wrong.

---

## 6. Reply to the reviewer

Send after the fix is in flight, not before — it concedes a real bug, so it should arrive with a
branch attached.

> Good catches, and following the second one through turned up something worse than the missing
> test.
>
> 1. Correct — we don't test under a non-C `LC_NUMERIC`. Adding that, selecting an installed
>    comma-decimal locale and skipping visibly when the image only has C/POSIX.
>
> 2. Agreed that controlling the locale beats formatting under the caller's and rewriting the
>    separator afterwards. We're going with the `_l`-suffixed calls where the platform has them
>    (`strtod_l`/`snprintf_l`) and a `uselocale` bracket as the glibc fallback, restoring on every
>    path.
>
> One correction to my own earlier framing: Aether *is* multithreaded — actors, a multicore
> scheduler, worker pools, HTTP accept threads — and may additionally be embedded in a
> multithreaded host. So `setlocale` would be a data race in our own runtime, not merely impolite
> to the embedder. `uselocale`/`_l` is right for both reasons.
>
> The bigger find: the parse side had no handling at all. `strtod` is equally `LC_NUMERIC`-
> sensitive, so under `de_DE.UTF-8` `string.to_double("3.14")` stops at the `.` and reports a parse
> failure — the round-trip guarantee that PR was meant to establish doesn't hold. Same for JSON
> number parsing, which is an RFC 8259 conformance issue. Fixing both directions across
> `std/string`, `std/json` and the runtime, with your locale test as the gate.

---

## 7. Out of scope

- **Ryu / Grisu shortest-round-trip formatting.** Would sidestep libc locale handling entirely and
  give shorter output, but it is a much larger change and `%.17g` is already round-trip-correct.
  Worth a separate issue; note it in the PR as the longer-term direction.
- **`std/number` (Phase 4).** Human-facing, explicitly locale-parameterised, correct as-is.
- **Other locale-sensitive libc** (`isspace`, `tolower`, `strcoll`, `strftime` — ~34 hits across
  `std/`+`runtime/`). Real, but a separate audit. `isspace` on a `char` with the high bit set is
  the classic UB there. **File a follow-up issue; do not expand this PR into it.**
- ~~**Renaming this file.**~~ **DONE 2026-08-08** — was `changes_to_il8n.md` at the repo root, a
  misnomer twice over (typo, and this is locale-*independence*, not i18n). Now
  `docs/locale-independent-float-text.md`, matching the kebab-case convention of its neighbours.
  The old name invited exactly the wrong conclusion: that this advanced #863's i18n work, when it
  actually removes a hazard that i18n work was walking toward.

---

## 8. Sequencing

1. ~~Settle the MinGW question on winbaz (§3.1)~~ — **DONE 2026-08-08**, `_l` family verified
   present and working on MINGW64 + UCRT64 (see §3.1). No blocker; Windows is backend 1.
2. Provision `de_DE.UTF-8` on the CachyOS box (§4.3b step 3) — do this **before** adding it to the
   dep gate, or the next nightly goes RED on a provisioning gap of our own making. Box state
   checked 2026-08-08: reachable, `/etc/locale.gen:127` has the entry commented out, and there is
   **no passwordless sudo**, so this needs an interactive run (command already given to Paul):
   `ssh -t paul@192.168.0.160 "sudo sed -i 's/^#de_DE.UTF-8 UTF-8/de_DE.UTF-8 UTF-8/' /etc/locale.gen && sudo locale-gen"`
3. Write the shim + tests; land the parse-side fix and format-side fix together in one PR (they are
   two halves of one round-trip guarantee). Nightly wiring goes in the same PR.
4. Reply to the reviewer with the branch link.
5. Watch the first nightly after merge — confirm the locale tests ran rather than skipped (§5.6).
6. File the follow-up issue for the broader locale-sensitive-libc audit (§7).
