# `--emit=lib` caller-info channel

A thread-local channel between the host process and an
Aether `--emit=lib` `.so` for **per-call advisory context**: the
host stamps `(identity, attributes, deadline_ms)` before invoking
an `aether_<name>` export; the loaded module reads it via
`host.caller_*` accessors.

## Trust model, explicit non-goal

This is **not a security boundary**. The host always has more
authority than the loaded `.so` (it controls the process, the FD
table, the address space). Adding cryptographic signing in-process
buys nothing the host couldn't fake anyway. Treat caller-info as
**plumbing for advisory per-call context**, identity, role flags,
soft deadlines, not as authenticated facts.

If you need cross-machine trustworthy caller identity (the WASM
guest in Pollen had this need because the host and guest live in
different address spaces and could be on different machines),
build that on top: have the host pass a signed JWT in `identity`
and have the Aether code verify the signature.

## Host C API

```c
#include "aether_host.h"

/* Populate per-call context. Returns 0 on success, -1 if the
 * key/value/identity bytes would exceed AETHER_CALLER_INFO_MAX_BYTES
 * (4096 default) or the attribute count would exceed
 * AETHER_CALLER_INFO_MAX_ATTRS (32 default). On overflow the slot
 * is left in its previous state, no partial population. */
int aether_set_caller(const char* identity,
                      const char** attr_keys,
                      const char** attr_vals,
                      size_t n,
                      int64_t deadline_ms);

/* Wipe the slot. Hosts that load multiple plugins per thread
 * should clear between calls so a forgotten set_caller from a
 * previous invocation can't leak through. */
void aether_clear_caller(void);
```

`identity` may be NULL (means "no identity"; the loaded module
sees `""`). Keys + values are deep-copied into a TLS-owned buffer
at `aether_set_caller` time, so the caller's source pointers can
be freed after the call returns. `n == 0` is allowed for
identity-only / deadline-only stamps; `attr_keys` / `attr_vals`
may then be NULL.

`deadline_ms` is opaque to the runtime, whatever convention the
host wants (milliseconds since epoch, absolute monotonic-clock
milliseconds). Pair with the per-call deadline tripwire from
the resource-caps machinery to enforce.

## Aether-side accessors

```aether
import std.host

main() {
    id      = host.caller_identity()           // "" if not set
    role    = host.caller_attribute("role")    // "" if absent
    dead_ms = host.caller_deadline_ms()        // 0 if not set
}
```

All accessors are NULL/unset-safe. A guest that doesn't import
`std.host` or doesn't read these accessors works fine, no
requirement to consume.

## Lifetime

The TLS slot persists across calls until either:
- `aether_set_caller` overwrites it with new context, or
- `aether_clear_caller` wipes it.

The runtime does **not** auto-clear on export return. Hosts that
multiplex multiple guests on one thread should call
`aether_clear_caller` between invocations themselves.

## Storage limits

Default caps:
- 4096 bytes total (identity + all keys + all values, NUL bytes
  included).
- 32 attribute pairs.

Both are tuneable at compile time, `-DAETHER_CALLER_INFO_MAX_BYTES=N`
/ `-DAETHER_CALLER_INFO_MAX_ATTRS=N`. `aether_set_caller` returns
`-1` on overflow and leaves the previous state intact.

## Threading

The slot is thread-local. Hosts that call into Aether from
multiple threads each have their own slot. Aether code reading
`host.caller_*` always sees the values populated on the current
thread.

## Relationship to other isolation layers

| Layer | Granularity | Decision time | Issue |
|-------|-------------|---------------|-------|
| `--with=fs`/`--with=net` build flags | per-build | compile | existing |
| `hide` / `seal except` lexical denial | per-scope | compile | existing |
| `libaether_sandbox.so` LD_PRELOAD libc gate | per-process | runtime | existing |
| **caller-info channel** | **per-call** | **runtime** | **this document** |
| Resource caps (memory + deadline) | per-call | runtime | resource caps |

Each layer answers a different question. The build flags answer
"what is this binary allowed to do." The lexical layer answers
"what can this scope name." The libc gate answers "what syscalls
can this process make." Caller-info answers "who's calling me
right now, and what authority did the host stamp on them." Compose
them as needed; they don't conflict.
