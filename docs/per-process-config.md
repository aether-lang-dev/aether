# Per-process configuration in Aether

Many CLI tools and servers need a per-process key/value: parse it once at startup (from `--flag X` args, an env var, or a config file), then read it from many call sites for the rest of the process's life. Examples:

- `--superuser-token X` parsed at CLI entry, checked on every HTTP request handler.
- `--log-level debug` set once, consulted by every log call.
- `--data-dir /var/lib/myapp` baked in at startup, used by every filesystem op.
- API keys, tenant IDs, current-user identity stashed by an auth middleware.

In C this is a `static int g_thing = …;` plus `set_thing(n)` / `get_thing(void)` helpers. Aether transcribes this shape directly with a **module-level `var`** : `var` at module scope lowers to a file-scope C `static`, and same-module functions read and write it as a plain identifier, a real lvalue, so a setter genuinely updates it and every reader sees the new value. That's the simplest answer for scalar per-process config (a log-level enum, a numeric limit, a feature flag, a pointer handle).

For state that must be shared **across actors / threads**, or that owns a resource it should free, the **actor-as-singleton** is the better tool: spawn one actor at startup, set values via messages, read via messages, the mailbox serialises access for you. Both patterns are below; pick by whether the value is process-local scalar config (`var`) or concurrently-shared/owned state (actor).

This page is the worked example. It's a doc, not a stdlib feature, there's nothing to import beyond the language primitives.


## The simple case: a module-level `var`

For a scalar config value parsed once at startup and read from many call sites, declare a module-level `var` and write the parsed value through a setter:

```aether
import std.io

// Parsed once at startup, read everywhere for the rest of the process.
// The INITIALISER must be a compile-time constant (it lowers to a C
// `static`, which the C standard requires to have a constant
// initialiser), so seed it with a constant and assign the real,
// CLI-parsed value from a setter at startup, not in the initialiser.
var log_level = 0          // 0=info, 1=debug, 2=trace (inferred int)
var max_conns: long = 100  // explicitly typed numeric limit

set_log_level(n: int) {
    log_level = n          // writes the module-level static, not a local
}

should_log(level: int) -> bool {
    return level <= log_level   // every reader sees the setter's value
}

main() {
    set_log_level(2)            // e.g. value from `--log-level trace`
    if should_log(1) {
        println("debug visible")
    }
    println(max_conns)
}
```

This is the `static int g_thing; set_thing(n)` shape, one-to-one. `set_log_level` mutates the same storage `should_log` reads, no actor, no message round-trip, no accessor indirection.

Two constraints to know:

- **The initialiser must be a compile-time constant.** `var limit = compute_default()` is rejected, you can't seed a module-level `var` with a runtime or CLI-parsed value in the initialiser. The compiler says so explicitly:

  ```
  error[E0200]: module-level `var` initializer must be a compile-time constant
  expression, it lowers to a file-scope `static`, which C requires to have a
  constant initializer. Initialize it to a constant here and assign the
  computed value from a function at startup instead.
  ```

  Seed it with a constant (`var limit = 0`) and write the computed value from a setter called at startup, exactly as `set_log_level` does above.

- **It's module-private and best for scalar/pointer values.** A module-level `var` is not exported across modules, and the direct-static lowering is the right fit for scalars and pointers (`int`, `long`, `uint64`, `ptr`, …). For a **string-valued** process config (an auth token, a data-dir path, a current-user name) reach for the actor singleton below, a string is heap-managed and an owned, freed resource is exactly the "shared/owned state" case the actor pattern is built for.

## The shared/owned case: an actor singleton

When the value is a string (or any heap-owned resource), or must be visible across actor boundaries, model it as a one-actor singleton: spawn it at startup, set values via messages, read via reply-bearing messages. The mailbox serialises writes against reads, so concurrent setters and getters can't interleave incorrectly.

```aether
import std.string

// Messages that drive the config actor.
message SetUser   { name: string }
message SetToken  { value: string }
// Reads route through a reply-bearing message: include a reply
// actor_ref so the config actor can send the value back.
message GetUser   { to: actor_ref }
message GetToken  { to: actor_ref }
message UserReply { value: string }
message TokenReply { value: string }

// The singleton. State-only, no business logic, just a key/value
// holder. Spawn exactly one of these.
actor ConfigActor {
    state user  = ""
    state token = ""

    receive {
        SetUser(name)   -> { user = name }
        SetToken(value) -> { token = value }
        GetUser(to)     -> { to ! UserReply  { value: user  } }
        GetToken(to)    -> { to ! TokenReply { value: token } }
    }
}

main() {
    cfg = spawn(ConfigActor())

    // Write-once at startup.
    cfg ! SetUser  { name: "alice" }
    cfg ! SetToken { value: "secret-xyz" }

    // … later, in a request handler that holds an `actor_ref` to its
    // own reply target …
    // cfg ! GetUser { to: self_ref }
    // receive { UserReply(value) -> { … use value … } }
}
```

The reply pattern (caller sends a `Get*` message containing its own actor_ref, then `receive`s the matching `*Reply`) is the same ergonomics Erlang's `gen_server:call` provides under the hood. It's verbose for one-shot reads; if you only need writes, drop the `Get*` / `*Reply` half and design your readers to take the value from a message they were sent at startup.

## Variants

**Write-once, read-many string config** (auth token, data-dir path, current-user name):

The actor singleton above is what you want. The mailbox serialises writes with reads, so concurrent setters and getters can't interleave incorrectly. If you only set at startup and never again, the cost is one message round-trip per read.

**Scalar config (log level, numeric limit, feature flag, pointer)**:

Use a module-level `var` (see [The simple case](#the-simple-case-a-module-level-var)). A setter writes the parsed value at startup; reads are direct loads from the static, no round-trip, no message ceremony.

**Hot-path reads where actor message overhead is too much**:

Measure first. A same-core actor message bypasses the incoming queue and writes directly to the target's mailbox or SPSC queue (see [runtime-optimizations.md](runtime-optimizations.md), "SPSC Queue for Same-Core Messaging"), so the round-trip is cheap. For a per-HTTP-request auth-token check it is in the noise compared to the cost of the request itself.

If you've measured and the round-trip really is too expensive (e.g. an inner loop reading the value 10⁹ times), the alternatives in order of preference:

1. **Pass the config value through your call chain.** Plumb it as a parameter from the place that has the singleton's value to the place that needs it. This is the "explicit dependency" approach and is usually the right answer.

2. **Cache the value in a local actor's state.** A worker actor that handles requests can ask the config actor once on startup and store the value in its own `state token = "…"`. Each subsequent handler invocation reads the local state, no round-trip. The trade-off is staleness if the config can change after the worker initialized.

3. **Use a `const` if the value is genuinely immutable across the process's life.** `const TOKEN = "secret-xyz"` lowers to a `#define` and reads are zero-cost. Unlike a module-level `var`, a `const` can't be a setter's target, so it only works when the value is known at compile time (not for CLI-parsed config).

**Multi-tenant / per-request state**:

Per-process config is the wrong shape for things that vary per request (current user, tenant ID, request ID). For those, use thread-local state (TLS) or pass the value explicitly through your handler chain, the actor singleton would serialize all handlers behind one mailbox, which defeats parallelism.

## Module-level `var` vs an actor singleton

Both are first-class; choose by what the state is and who touches it.

**Reach for a module-level `var`** when the config is a process-local scalar, a log level, a numeric limit, a feature flag, a pointer handle, set once at startup and read from many call sites. It's the least machinery: a `static` plus a setter, reads compile to a direct load. Keep these caveats in mind:

- **It's not synchronised.** A module-level `var` is a plain `static`; concurrent writers need their own discipline (set it once at startup, before you spawn workers, and treat it as read-only thereafter). If multiple actors must *write* it during the run, that's the actor singleton's job, the mailbox gives you the synchronisation for free.
- **It's process-global, so mind tests.** Two test cases sharing one process see the same `var`; reset it explicitly between cases, or isolate by process. Actor state, by contrast, resets cleanly by re-spawning.

**Reach for an actor singleton** when the value is a string or other heap-owned resource (the actor owns it and frees it on its own terms), or when it must be read and written across actor boundaries with the runtime handling synchronisation. This is also the answer when you'd otherwise be tempted to reach for a `static char *g_user;` in a multi-threaded server, that bare global is a TOCTOU bug waiting to bite, and the actor model closes the hole.

The question to ask: is this process-local scalar config, or shared/owned state? The first is a `var`; the second is an actor.

## Worked example: replacing a C global-state shim

The svn-aether port has this in `subversion/ae/ra/shim.c`:

```c
static char *g_client_user = NULL;

void svnae_ra_set_user(const char *user) {
    free(g_client_user);
    g_client_user = user && *user ? strdup(user) : NULL;
}

const char *aether_ra_get_user(void) {
    return g_client_user ? g_client_user : "";
}
```

This shim stores a **string** (`char *`) and owns it (`free` / `strdup`), so it maps onto the actor singleton above rather than a module-level `var`. Set on CLI parse:

```aether
cfg ! SetUser { name: parsed_user_arg }
```

(A scalar C shim, say a `static int g_log_level;` with a `set`/`get` pair, maps the other way: it's a module-level `var log_level = 0` plus a `set_log_level` setter, exactly the [simple case](#the-simple-case-a-module-level-var) above. The string shim here needs the actor because the value is heap-owned.)

Read in the request handler, slightly more involved than a function call because the handler needs an actor_ref to receive the reply. The natural place to plumb that is through whatever spawns the handler:

```aether
// At handler-spawn time:
worker = spawn(RequestHandler())
cfg ! GetUser { to: worker }   // worker stashes the reply when it arrives

// In the worker's receive block:
receive {
    UserReply(value) -> { current_user = value }
    HttpRequest(req) -> { handle_request(req, current_user) }
}
```

This is more code than the C version, but it's also testable, thread-safe by construction, and amenable to "swap the config actor for a mock" in tests.

## See also

- [actor-concurrency.md](actor-concurrency.md), runtime details on the scheduler, mailboxes, and message delivery
- [runtime-optimizations.md](runtime-optimizations.md), the scheduler's message-passing optimizations, including same-core SPSC delivery
- `tests/integration/test_http_client_v2.ae` uses an actor singleton (`SrvActor`) for an in-process HTTP server's lifecycle
