# contrib.host.factor — Embedded Factor

`import contrib.host.factor` lets an Aether program embed
[Factor](https://factorcode.org) — a dynamically-typed, image-based
**concatenative (stack-based)** language in the Forth/Joy tradition, with
first-class quotations and a CLOS-style object system — in-process, with a
live, persistent VM (which is why it boots a ~140 MB image and JITs; see
[Performance](#performance)):

```aether
import contrib.host.factor

// eval-with-result: fib(10) computed in the VM, value back INTO Aether.
// Use a <<HEREDOC — no interpolation, no escaping, so Factor's own "..."
// strings pass through verbatim and the snippet reads like real Factor.
r = factor.eval(<<FACTOR
USING: math kernel prettyprint ;
IN: scratchpad
: fib ( n -- m ) dup 2 < [ ] [ dup 1 - fib swap 2 - fib + ] if ;
10 fib pprint
FACTOR
)                                              // r = "55"

// a Factor script populates the shared k-v map; Aether reads it back
_ = factor.run(<<FACTOR
USING: math math.parser namespaces sequences kernel ;
IN: scratchpad
: fib ( n -- m ) dup 2 < [ ] [ dup 1 - fib swap 2 - fib + ] if ;
10 <iota> [ dup fib number>string swap number>string "fib" prepend set-global ] each
FACTOR
)
v = factor.get("fib9")                         // v = "34"
```

The VM is created once and reused across calls, so `set` / `run` / `eval` /
`get` share state — a scalar k-v map handed in and read back, same process.

For map-shaped interop there is also a **first-class shared map**
(`run_sandboxed_with_map`), the same mechanism the other hosts use — see
[Shared map](#shared-map-first-class-map-interop) below. Use the scalar
`set` / `get` for a few known keys; use the shared map when the guest computes
the keyset (e.g. a map-reduce whose keys you don't know upfront).

> **Status: experimental, needs a forked Factor.** This host is unlike the
> others — read the two caveats below before reaching for it.

## API

| Call | Effect |
|------|--------|
| `factor.eval(code) -> string` | evaluate `code` in the persistent VM; returns its **printed output** (`""` if the lib can't load; Factor's error text if the snippet errors — see below) |
| `factor.run(code) -> int` | fire-and-forget; 0 on success |
| `factor.set(key, value)` | store a string value under `key` in the VM's global namespace |
| `factor.get(key) -> string` | read `key` back as raw text (`10`, not `"10"`); `""` if absent |
| `factor.run_sandboxed(perms, code)` | see Caveat 2 |
| `factor.run_sandboxed_with_map(perms, code, map_token)` | run `code` with live access to an Aether-owned shared map — see below |

Like every other host, the k-v map is **string-only** — there is no int or
float channel. Numbers round-trip as their decimal text; `factor.get`
renders a stored string as its own content and a number as its digits
(Factor's `present`), matching lua/tcl/etc.

Evaluated snippets run in a **bare vocabulary** — there are no default
imports — so each snippet must name what it uses (`USING: math ; …`).

`factor.eval` captures **printed output**, not the data stack: it runs the
snippet under Factor's `eval>string` with effect `( -- )`, so the snippet must
leave a clean stack and *print* whatever it wants back (`… pprint`, `… .`,
`… present write`). A bare `2 3 +` leaves a value on the stack and is a
stack-effect error — render it: `2 3 + pprint`. On a snippet error
(stack-effect mismatch, undefined word, …) `eval>string` recovers and writes
the **error text** into the captured output, so `eval` returns Factor's
diagnostic string rather than `""` — handy when debugging, but check for it if
you treat a non-empty result as success.

Defining a word inline (`: name ( … ) … ;`) needs an **`IN:` vocabulary**
declaration first (e.g. `IN: scratchpad`) — eval runs outside any vocabulary,
and a colon definition with no `IN:` fails with *"Not in a vocabulary; IN:
form required."*

## Shared map (first-class map interop)

`factor.run_sandboxed_with_map(perms, code, map_token)` gives the Factor
script a **live view of an Aether-owned shared map** — the same
`aether_shared_map` mechanism the other hosts (lua/python/…) use. Aether
builds the map, the Factor script reads and writes it *as it runs* through two
words the bridge injects, and Aether reads the whole map back afterwards —
including keys the script **discovers at runtime**:

```factor
"key" aether-map-get          ! -> value string, or f if absent (inputs are
                              !    readable; they're frozen, so writes to them
                              !    are rejected)
"key" "value" aether-map-put  ! write/overwrite an output key
```

Because Aether owns the map, key discovery is free: after the run, enumerate
with `map.keys(m)` (`std.collections`) — no host-side "list keys" call. This
is the right tool when the **guest computes the keyset**. Example — a
word-frequency map-reduce over English text whose keys (the words) aren't
known upfront:

```factor
USING: splitting sequences math math.parser namespaces kernel ;
IN: scratchpad
"text" aether-map-get " " split
[ dup aether-map-get [ string>number ] [ 0 ] if* 1 + number>string aether-map-put ] each
```

Hand in `text = "the cat sat on the mat the cat"`, run the above, and the map
comes back with `the=3 cat=2 sat=1 on=1 mat=1` — keys Aether never named.
Still string-only (counts round-trip as decimal text). The `aether-map-get` /
`aether-map-put` words are injected automatically; you do **not** add a
`USING:`/`FUNCTION:` prelude for them.

Choose by shape: scalar `set`/`get` for a few **known** keys; the shared map
when the guest **produces** the keyset.

## Caveat 1 — needs the embed-api fork of Factor

The modern upstream VM has no re-entrant C embedding entry point: it once
shipped `start_embedded_factor`, since deleted. The surviving public path
(`start_standalone_factor`) takes the process over and `exit()`s — useless
for a host that needs the result back. So this bridge cannot bind a stock
distro `libfactor`.

It depends on a small **fork** that restores embedded eval —
[`aether-lang-dev/factor-language`](https://github.com/aether-lang-dev/factor-language),
`vm/embed_api.cpp` (one commit on top of upstream `master`):

```c
/* generic, NOT Aether-specific */
char* factor_embed_eval(const char* image_path, const char* src);
void  factor_embed_eval_free(char* result);
```

It reuses Factor's own `OBJ_EVAL_CALLBACK` / `eval>string` machinery and a
**stock `factor.image`** baked by the normal bootstrap (no custom save-image
— so the image is reproducible). The map-in/map-out interop is layered in
*this bridge* on top of the generic eval, so the fork's API stays
Aether-free and upstream-proposable.

## Caveat 2 — the in-process sandbox does not contain Factor

The other hosts run under the LD_PRELOAD libc gate, so `*_run_sandboxed`
checks each libc call against a grant list. Factor's VM does its own GC,
JIT (writable+executable code heap), signal handling and threads, which
that gate does not cleanly contain. `factor.run_sandboxed` accepts `perms`
only for signature parity and relies on the **process-level** sandbox
(`spawn_sandboxed`) for isolation, not a per-call checker. Treat embedded
Factor as trusted-ish code behind the process boundary, not as libc-gated
guest code.

## Performance

Measured on linux-x86-64 (your numbers will vary):

| | cost |
|--|--|
| **first call** (VM init + boot + callback install + 1 eval) | **~175 ms, once** |
| **warm eval** (`2 3 +`) | **~0.03 ms** (~33k/sec) |
| **k-v set+get pair** | **~0.06 ms** (~17k/sec) |

The shape that matters: a heavy **one-time** init (image boot + JIT warm-up;
~a few × a CPython cold start, far heavier than Lua), then effectively-free
steady state because the VM is persistent and JIT-native. Init is lazy — it
happens on the first `factor.*` call, not at process start.

- **Long-lived host** (server, an Aether process that evals repeatedly):
  great — amortize the ~175 ms once.
- **Fire-once-and-exit:** the init dominates; embedding is the wrong tool
  there.
- **Memory:** the image is ~140 MB on disk with a comparable resident
  footprint — far heavier than the lightweight hosts. Embedding Factor
  commits real RAM.
- **GC jitter:** Factor's generational GC can add occasional pause-time
  jitter under allocation-heavy loops (irrelevant for k-v/config use).

Each `set`/`get` parses+evals a small snippet (~0.03 ms), not a bare hash
op; fine for hundreds/thousands of keys. Dedicated C `set`/`get` that poke
the namespace without re-parsing would be where a further 10× lives, if
ever needed.

## How it loads libfactor

The bridge `dlopen`s libfactor at the deploy host's runtime — no `-lfactor`
on the link line. Discovery at first `factor.*` call (orchestrator-owned,
no hardcoded version list):

1. `${AETHER_FACTOR_SONAME}` — exact path/soname of the forked libfactor
   (e.g. `/opt/factor/libfactor.so`).
2. `libfactor.so` (unversioned, if installed).

Factor also needs its bootstrapped image; set `${AETHER_FACTOR_IMAGE}` to
the `factor.image` path. If the lib can't be loaded, `factor.*` returns the
error shape (`-1` / `""`) with an actionable message.

## Building

Not in the default `CONTRIB_HOST_LANGS` set (it needs the fork). To
experiment:

1. Build the fork's `libfactor` and bootstrap its `factor.image`:
   ```sh
   git clone https://github.com/aether-lang-dev/factor-language
   cd factor-language && make linux-x86-64 factor-lib
   curl -fsSLO https://downloads.factorcode.org/images/master/boot.unix-x86.64.image
   ./factor -i=boot.unix-x86.64.image           # bootstraps factor.image
   ```
2. Build the bridge `.a` and point the env vars at the fork:
   ```sh
   gcc -c -O2 -DAETHER_HAS_FACTOR -DAETHER_HAS_SANDBOX -I<aether-root> \
       contrib/host/factor/aether_host_factor.c -o build/contrib/host_factor.o
   ar rcs build/contrib/libaether_host_factor.a build/contrib/host_factor.o
   export AETHER_FACTOR_SONAME=/path/to/factor-language/libfactor.so
   export AETHER_FACTOR_IMAGE=/path/to/factor-language/factor.image
   ```
3. `ae build` an Aether program with `import contrib.host.factor` — the
   bridge `.a` is auto-linked by the generic host-bridge scanner.

## Testing

The dedicated end-to-end test lives at
[`tests/integration/host_factor/`](../../../tests/integration/host_factor/) —
[`uses_factor.ae`](../../../tests/integration/host_factor/uses_factor.ae) is the
driver (the fib(10)=55 set-piece: a Factor script defines a recursive `fib`,
`eval` captures its printed output, and a second script writes the first ten
terms into the shared namespace under `fib0..fib9` for Aether to read back as a
k-v map — also pinning the stack-effect error contract, a read-mutate-write
round-trip, and the absent-key `""` shape), and
[`test_host_factor.sh`](../../../tests/integration/host_factor/test_host_factor.sh)
is the runner: it builds the factor host `.a` (skipped by the default contrib
build), runs the driver, and greps for `PASS`. Because this host needs the fork,
it **SKIPs** (never fails) when `$AETHER_FACTOR_SONAME` / `$AETHER_FACTOR_IMAGE`
are unset or don't point at real files — CI machines without the runtime no-op
cleanly.

Factor is also covered by the cross-host shared-map test
[`tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh),
which runs the same `run_sandboxed_with_map` round-trip across every available
host. Its Factor case hands in the text `the cat sat on the mat the cat` and
map-reduces a word-frequency histogram into the shared map under keys discovered
at runtime, then reads them back (`the=3`, `cat=2`, six keys total). It too
**SKIPs** Factor when the two env vars are unset or missing.
