# ABI compatibility: what a shipped library promises

A library built by one Aether is used by code built by another: a C host
compiled against an older `libaether.h`, a program whose `ae` is newer, a
plugin host that loads libraries of several releases into one process.
This page says which of those pairs are supported, what each side may rely
on, and where an unsupported pair is refused before anything runs (#2957).

## The boundaries

| Boundary | What crosses it | Stability |
|---|---|---|
| The host ABI | `include/libaether.h`; a library's `aether_<name>` exports, `aether_<lib>_string_release`, `aether_<lib>_outcome` and `aether_<lib>_check` | Versioned by `AETHER_HOST_ABI` (1). Within a level it only grows |
| The catalog | `aether_lib_meta()`, the record an importer and a plugin host read | Schema `"1.minor"`: fields are only appended within 1 |
| The runtime ABI | generated C and the runtime it runs on: actor, message and frame layouts, the entry points generated code calls | Not stable across versions: a rebuild boundary |
| Shared facilities | what runtime copies in one process share through a rendezvous: sandbox levels, execution accounts | Versioned per facility, with the sizes of what the copies pass |
| Values | strings and closures | A string crosses a static boundary only as a copy; a closure, or a struct holding a string, never crosses one |

**The host ABI.** Within a level, new functions, constants and outcomes
may be added; a removal, a layout change of a struct in `libaether.h`
(`AetherExecToken`, `AetherExecLimits`) or a change of meaning bumps the
level. A host treats an outcome it does not know as a failure, as it does
`AETHER_PANICKED`. A library records the level it was built at in its
catalog (`host_abi`, schema 1.11). A library with no `host_abi` (Aether
0.856 and before) is level 0, whose surface is `aether_set_memory_cap`,
`aether_set_call_deadline` and `aether_deadline_tripped`; level 1 adds the
outcomes, cancellation, execution accounts and the preflight. A host uses a
function of a level only when the header it was compiled against has it and
the library records that level or more.

**The catalog.** An importer reads a catalog of any minor of schema 1, an
older one by its minor as before, and refuses another major. What a newer
catalog asks of its importer is listed in `requires`, its duties:

| Duty | What the importer must do |
|---|---|
| `outcome_check` | read each call's outcome and raise a failure in the program (schema 1.10) |
| `string_release` | give a returned string back through the library's release (1.9) |
| `shared_runtime` | link the program against the shared runtime the library runs on (1.5) |
| `runtime_abi` | check that runtime is the one the library was made for (1.11) |
| `actors` | run the library's actors on the program's scheduler (1.7) |

An importer that finds a duty it does not know refuses the library, naming
the duty, both Aether versions and the fix (upgrade `ae`, or rebuild the
library). A newer minor whose duties it knows is read.

**The runtime ABI.** `AETHER_RUNTIME_ABI_ID` (`runtime/aether_abi.h`) is
the Aether version, `/abi` and `AETHER_RUNTIME_ABI_REV`, and the flags
that change layouts (`+debug-mailbox`). A self-contained library carries
its own runtime, so this ABI never crosses its edge: on ELF `ae` links it
`-Bsymbolic`, so its runtime calls bind to its own copy, not to the one a
program linked `-rdynamic` exports, as macOS and Windows keep them apart by
themselves (#3481). A `--shared-runtime`
library, and C emitted with `--emit=csrc` or `--emit=obj`, must run on the
runtime it was made for. The shared runtime reports its ID
(`aether_runtime_abi()`), and the catalog of a shared-runtime library
records the one it was made for (`runtime_abi`).

**Shared facilities.** A library without `--shared-runtime` brings a copy
of the runtime, and copies in one process share the sandbox levels and the
execution accounts through the first copy loaded that has them, by its
record in its image (a PE section, an ELF note, a Mach-O section, #3445).
The record's layout never changes, so a copy of any release reads any
other's. Each
rendezvous starts with its version and its size, and the accounts' also
carry the sizes of what the copies pass each other. A copy that finds
another layout refuses that one facility, says so once on stderr naming
`--shared-runtime`, and reads nothing of the other's but the version and
sizes.

## Where an unsupported pair is refused

Every check runs before any code of the library does.

| The pair | Refused by | How |
|---|---|---|
| A catalog of another major | the importer (`ae build`) | "reads schema 1 only" |
| A catalog that requires a duty the importer does not know | the importer | names the duty, both versions and the fix |
| A `--shared-runtime` library made for another runtime ABI, or recording none (level 0) | the importer | names both runtimes; "rebuild the library with this Aether" |
| Generated C compiled against the headers of another Aether | the C compiler | `#error` naming the aetherc that generated it |
| A library linked or loaded against another runtime than its own (a shared runtime replaced in place, or `--emit=csrc` code) | its first call | every export returns its zero value with `AETHER_INCOMPATIBLE` (6), none of its code runs, the reason is printed once and in `aether_last_outcome_text()`; `aether_<lib>_check(&why)` asks first, 0 or -1 |
| A shared-runtime library on an older runtime that lacks the entry points it calls | the loader | `dlopen` or `LoadLibrary` fails |
| A rendezvous of another layout | the copy that finds it | that facility only, once on stderr |

`tests/integration/abi_policy` exercises each of these, with hand-made
catalogs and libraries compiled for a runtime of another ABI.

## Supported combinations

| Combination | Supported |
|---|---|
| A C host and a self-contained library of any version | Yes, using the functions of the library's level (read `host_abi`); a host built with older headers uses its own level's |
| An Aether program importing a self-contained library | Yes, when the importer knows every duty the catalog requires; a library of an older version is read by its minor. On ELF, a library of another runtime ABI linked before #3481 (without `-Bsymbolic`, so it would run on the program's runtime) is refused at import, naming the rebuild |
| An importer older than schema 1.11 and a newer library | Calls that succeed work; it cannot read `requires`, so it does not check outcomes, and a failed call reads as its zero value, as with 0.856's own libraries |
| Anything on `--shared-runtime` | Only on the runtime it was made for (one runtime ABI ID) |
| Two self-contained libraries of different versions in one process | Yes; each runs on its own runtime, and the shared facilities are refused across layouts |
| `--emit=csrc` and `--emit=obj` | Only with the runtime of the Aether that generated them |

Verified by the cross-version matrix below on Windows x86_64 against
0.856.0 (by hand), and on Linux x86_64 and macOS arm64 in CI against the
latest published release, on every pull request and before every release.

## Deprecated names

A removed feature's names stay, accepted and ignored, until the next host
ABI level removes them. Each is marked where it is declared.

| Name | Since | Now | Removed at |
|---|---|---|---|
| `AETHER_FLAG_LOCKFREE_POOLS` (`runtime/aether_runtime.h`) | #3374 | accepted, ignored; `aether_runtime_has_feature()` reports it off | host ABI level 2 |
| `aether_message_pool_stats` (`runtime/actors/aether_send_message.h`) | #3374 | reports zeros | host ABI level 2 |

## The cross-version matrix

`tests/integration/abi_matrix` builds `mlib.ae` with this Aether and with a
release before it (`AETHER_ABI_OLD_HOME`, an extracted release archive) and
runs every supported pair, and the refusals of the unsupported ones:

- C hosts that read everything by name: this header with this library and
  with the old one, and the old header with this library: typed calls, a
  string given back with no byte kept, errors and deadlines at the levels
  the pair has;
- the old and the new library in one process, each with its own runtime:
  calls interleaved, an account of the new one suspended while the old one
  runs and resumed, both unloaded;
- Aether consumers both ways: this importer with the old library, the old
  importer with this one;
- the shared runtime: each library on its own, the new library on the old
  runtime, and this importer with the old shared-runtime library. Matching
  runtime ABI IDs must work; different IDs must be refused (at load or by
  the preflight for the C host, at import for the Aether consumer). The
  latest published release may have the same ABI as the tree under test.

Call output depends on the host ABI levels; shared-runtime acceptance
depends on the full runtime ABI IDs, including revision and layout flags.
To run it:

```sh
AETHER_ABI_OLD_HOME=/path/to/aether-0.856.0 sh tests/integration/abi_matrix/test_abi_matrix.sh
```

Without `AETHER_ABI_OLD_HOME` it skips.

## Changing the ABI

`tests/runtime/test_abi_layout.c` records the layouts of host ABI level 1,
catalog schema 1.11 and the rendezvous versions, and fails first when one
changes:

- a struct or constant of `libaether.h`: bump `AETHER_HOST_ABI`, and
  record the new level's layouts beside the old;
- the catalog: append the field at the end of `AetherLibMeta`, bump the
  minor, and add a duty to `requires` when an importer must act on it;
- a runtime layout or entry point between releases: bump
  `AETHER_RUNTIME_ABI_REV`;
- a rendezvous: bump its version.
