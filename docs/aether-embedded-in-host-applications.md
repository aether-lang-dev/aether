# Aether as a Configuration Language for Other Languages

A design exploration: using Aether's trailing-block DSL, `hide`/`seal except`,
and closures as a configuration and wiring language embedded in host
applications (Java, Go, etc.).

> **Status (2026-04-18):** the **foundation is built and v2 is shipped**.
> `aetherc --emit=lib` produces a `.so`/`.dylib` with stable C-ABI entry
> points (see **[`emit-lib.md`](emit-lib.md)**), and the **v2 embedded-namespace
> layer** generates idiomatic per-language SDKs (Python ctypes,
> Java Panama, Ruby Fiddle) on top, see
> **[`embedded-namespaces-and-host-bindings.md`](embedded-namespaces-and-host-bindings.md)**
> for the typed-SDK story.
>
> What's built (✅ Done):
> - Part 2 §1, shared library output (compiler flag is named `--emit=lib`,
>   not `--embed`)
> - Part 2 §2, `aether_config_*` C ABI for walking results
> - Part 2 §3, host bindings: hand-written per-language SDK generators
>   for Python, Java (Panama, JDK 22+), Ruby (Fiddle). Java SWIG is no
>   longer the recommended path, Panama is cleaner once you have a
>   namespace manifest to template against
> - Part 3, capability-empty default (rejects `std.net|http|tcp|fs|os`)
> - Part 4, LD_PRELOAD containment composes naturally with `--emit=lib`
>
> What's still speculative (📋 Future):
> - Part 2 §4, `ae eval` → JSON on stdout (the in-process accessor
>   path may obsolete this)
> - Part 2 §5 + Part 3, host-side callbacks, `host_call()` primitive,
>   the full ARexx/rules-engine bidirectional model
> - Wall-clock timeout, allocation budget
>
> Section-by-section annotations below mark each item.

---

## Part 1: Aether's DSL vs. the configuration language landscape

### Context

Anton Medvedev's blog post
["Things I Don't Like in Configuration Languages"](https://medv.io/blog/things-i-dont-like-in-configuration-languages)
reviews ~25 config languages and finds recurring problems.  His complaints
fall into a taxonomy:

| Sin | Examples | Author's take |
|---|---|---|
| Monstrous spec | YAML, CUE, HOCON | Too many features, too hard to implement |
| Actually a programming language | Pkl, CUE, Dhall, Jsonnet, Nickel, Starlark | "I'd just use TypeScript" |
| No specification / single implementation | UCG, UCL, Ziggy, Confetti | Not portable |
| Too many string variants | HJSON (3 comment types), SDLang (`on`/`off`/`true`/`false`) | Inconsistency breeds confusion |
| Significant indentation | YAML, HUML, HJSON multi-line strings | Fragile to copy-paste |
| Unordered keys / no null / no int/float distinction | JSON, JSON5, TOML | Missing semantics for config use |
| Weird nesting | HCL (`service "http" "web_proxy"`), TOML (`[[array of tables]]`) | Not intuitive |

His solution (MAML) is basically JSON with: comments, optional commas,
ordered keys, integers distinct from floats, multi-line strings, and a
small strict spec.  He explicitly wants *data*, not *code*.

### Where Aether's DSL sits

Aether's trailing-block DSL pattern (as in TinyWeb) is firmly in the
"actually a programming language" camp, exactly what the author dislikes
for config.  But here's the thing: **he's wrong to conflate all uses of
"programmable config."**

There are two very different needs:

1. **Static data config** (what the author wants): key-value pairs, maybe
   nested, maybe with comments.  A `.env` file, a `package.json`, a
   `Cargo.toml`.  No logic.  MAML, TOML, JSON, fine for this.

2. **Builder/wiring config** (what Aether's DSL does): constructing a
   running system from composable parts, routes, filters, middleware
   chains, factory registrations, scope hierarchies.  This is inherently
   *procedural*.  HCL (Terraform), Starlark (Bazel), Gradle's Kotlin DSL,
   and Rails' `routes.rb` all exist because static data can't express
   "register this handler at this path with this filter chain and hide
   these names from its scope."

The author's critique hits languages that try to be *both*, Pkl, Dhall,
Jsonnet add programming features to config *data*.  Aether's DSL isn't
trying to be a config data format.  It's a **builder DSL**, code that
constructs a system, where the structure of the code mirrors the structure
of the system.

### What Aether's DSL does right relative to the complaints

- **No specification problem**: the DSL *is* Aether.  No second language
  to learn, parse, or implement.
- **No significant indentation**: braces, not whitespace.
- **Nesting is natural**: `path("/api") { path("/v1") { end_point(...) } }`
  reads like what it means.  Compare HCL's `service "http" "web_proxy"`
  which the author hates.
- **`hide`/`seal` adds something no config language has**: scope-level
  capability denial.  A config block can declare what it *can't see*.
  That's not expressible in any of the 25 languages the author reviewed,
  it's a feature unique to "config as code in a language with hygiene."
- **Trailing blocks give you composition for free**: `compose(server) { ... }`
  adds routes to an existing server.  In static config, you'd need merge
  semantics, includes, overrides, all the complexity HOCON added and the
  author hated.

### The honest tradeoff

The author would say: "but I can't read an Aether DSL config with a JSON
parser."  True.  If your config needs to be consumed by 15 different tools
in different languages, use JSON/TOML/YAML.  But if your config
*constructs a runtime system in the same language that runs it*, routes,
handlers, DI wiring, scope policies, a static data format is the wrong
tool, and Aether's DSL with `hide`/`seal except` is genuinely better than
anything on his list.

The TinyWeb builder DSL is closer to Gradle's `build.gradle.kts` or Ruby's
`routes.rb` than to YAML.  That's the right comparison.

---

## Part 2: Embedding Aether in a Java application

### The question

Aether is compiled (``.ae`` -> C -> native binary).  What changes would be
needed so a large Java application could use Aether as its config/wiring
language?

### What exists today

- Aether compiles `.ae` -> C -> native binary
- No library mode, it's always a standalone executable
- No foreign-language embedding API
- Output is "run the program," not "return a data structure"

### What's needed

#### 1. Aether-as-a-library: compile + eval, return a value  ✅ Done

> Built as `aetherc --emit=lib` (the `--embed` name in the original
> design was renamed to fit the broader emit-mode taxonomy alongside
> `--emit=exe` and `--emit=both`). Every top-level Aether function is
> exported as `aether_<name>(...)` with a fixed-width C signature.
> The user's `main()` body is dropped in lib-only mode (a future
> Shape B addition could expose it as `aether_main()`). See
> **[`emit-lib.md`](emit-lib.md)** for the full reference.

The original sketch:

```java
AetherConfig config = Aether.eval("server.ae");  // compile, run, get result
Map<String, Route> routes = config.getMap("routes");
```

The shape that actually shipped is per-function rather than a single
`eval()` entry point: each top-level Aether function gets its own
exported `aether_<name>` symbol, so the host calls them directly:

```java
// Pseudocode, Panama bindings against libserver.so
var buildConfig = linker.downcallHandle(
    libserver.find("aether_build_config").get(), ...);
MemorySegment cfg = (MemorySegment) buildConfig.invoke("prod", 8080);
```

**What was built:**
- `aetherc --emit=lib` shared-library mode that omits `int main(...)`
  and emits `aether_<name>` alias stubs for top-level functions.
- `ae build --emit=lib` drives gcc with `-fPIC -shared`, output names
  default to `lib<name>.so` / `.dylib`.
- The original `--embed` proposal collided with the existing
  `--lib <path>` flag (module-resolution directory). Renaming to
  `--emit=lib` cleared the collision.

#### 2. A stable C ABI for the result  ✅ Done

> Built as `runtime/aether_config.h` and `runtime/aether_config.c`. The
> only material difference from the original sketch: handles are wrapped
> in an opaque `AetherValue*` typedef (rather than bare `void*`) so C
> consumers get real type-checking on traversal, and the per-language
> SDK generators can emit a typed proxy class per target.

```c
// runtime/aether_config.h (excerpt, see file for the full surface)
typedef struct AetherValue AetherValue;

const char*  aether_config_get_string(AetherValue* root, const char* key);
int32_t      aether_config_get_int   (AetherValue* root, const char* key,
                                      int32_t default_value);
AetherValue* aether_config_get_map   (AetherValue* root, const char* key);
AetherValue* aether_config_get_list  (AetherValue* root, const char* key);
int32_t      aether_config_list_size (AetherValue* list);
AetherValue* aether_config_list_get  (AetherValue* list, int32_t index);
void         aether_config_free      (AetherValue* root);
```

Ownership is documented on every signature (`Owned` vs `Borrowed`).
The original "single `aether_config_eval(source_file)` entry point" was
dropped, under `--emit=lib` each top-level Aether function is its own
entry point, so there's no need for a generic eval. See
[`emit-lib.md`](emit-lib.md) for the full surface and accessor walkthrough.

#### 3. Java binding layer  ✅ Built (via v2 namespace generator)

The recommended path is now the **v2 embedded-namespace layer**:
`ae build --namespace <dir>` reads a manifest and emits a typed Java
SDK (Panama, JDK 22+), a class with `set*` for inputs, `on*` for
events, methods named after each Aether function, and `AutoCloseable`
so try-with-resources releases the `Arena`. The host developer writes
no JNI, no `MethodHandle` lookups, no `MemorySegment` plumbing. See
**[`embedded-namespaces-and-host-bindings.md`](embedded-namespaces-and-host-bindings.md)**
for the design and `examples/embedded-java/trading/` for a worked example.

For hosts that want raw FFI without the namespace SDK:

- **Panama (Java 22+)**: declare each `aether_<name>` as a
  `downcallHandle` against the `.so` built by `ae build --emit=lib`.
  No JNI boilerplate. The `.so` exposes everything Panama needs.
- **JNI**: still works, no different from any other C library, but
  Panama makes it unnecessary.
- **SWIG**: the original v1 design considered SWIG. In practice the
  per-target generators in `tools/ae.c` are ~200-400 lines each and
  the templating control gained is worth not having SWIG in the
  toolchain.
- **Subprocess + JSON**: see §4 below; not built.

#### 4. Config-mode output (the simplest path)  📋 Future, possibly obsolete

> Not built. With `--emit=lib` shipping, the in-process accessor path
> covers most of what subprocess+JSON would have done, and avoids the
> serialization round-trip. Worth keeping as an option for hosts that
> can't link native code (sandboxed environments, language runtimes
> without FFI), but no longer the obvious "right first step."

The original sketch:

```aether
// server_config.ae
main() {
    server = web_server(8080) {
        path("/api") {
            end_point(GET, "/health") |req, res, ctx| { ... }
        }
    }
    return server  // the map IS the config
}
```

```bash
$ ae eval server_config.ae   # would compile, run, print result as JSON
{"port": 8080, "routes": [...]}
```

If/when this is built:
- `ae eval` subcommand that compiles+runs and serializes the return
  value of `main()` to JSON on stdout
- `map_to_json()`/`list_to_json()` serializer in `std/json`

#### 5. Closures/handlers, the hard part  📋 Future (Shape B)

> Not built. The v1 ABI carries data across the boundary cleanly but
> doesn't let the host hold a live Aether closure and invoke it later.
> This is the "Shape B" milestone explicitly deferred from v1.

The above works for *data* config.  But the whole point of Aether's DSL is
that config includes **live closures**, route handlers, filters, factory
callbacks.  You can't serialize a closure to JSON.

For a Java app to use Aether-defined handlers, you'd need one of:
- **Aether stays resident**: the compiled `.so` stays loaded, Java calls
  into it to dispatch requests.  Aether handlers run as native code called
  from Java via FFI.  This is the GraalVM polyglot model.
- **Code generation**: Aether config compiles to Java source/bytecode (a
  much bigger project).
- **Accept the boundary**: Aether config produces the *wiring* (route
  table, scope policies, factory names) as data, but handlers are written
  in Java.  The config is the blueprint; Java is the runtime.  This is the
  Terraform model, HCL defines *what*, the provider plugin (Go code)
  defines *how*.

#### Summary: path of least resistance, current state

| Step | Status | What it gives you |
|---|---|---|
| Stable C ABI header (`aether_config.h`) | ✅ Shipped | Any C-FFI language walks Aether maps via opaque `AetherValue*` |
| `aetherc --emit=lib` → `.so` | ✅ Shipped | In-process eval from Java/Python/Ruby via FFI |
| Per-language SDK generators (Python ctypes, Java Panama, Ruby Fiddle) | ✅ Shipped (v2) | `ae build --namespace <dir>` emits a typed SDK per declared binding |
| `ae eval` → JSON on stdout | 📋 Deferred | Subprocess pattern for hosts without FFI |
| Aether resident runtime + callbacks | 📋 Future (Shape B) | Java dispatches to live Aether closures |
| Wall-clock timeout / allocation budget | 📋 Future | Safety primitives for embedding |

The original prediction ("`ae eval` is the right first step") didn't
play out, `--emit=lib` turned out to be the same effort and gives you
the in-process path directly.  The closure-dispatching resident runtime
(host → script callbacks) remains the next big milestone.

---

## Part 3: The ARexx model, Aether drives the host

> **Status:** 📋 Future work. The capability-empty default
> (no `std.net|http|tcp|fs|os` in lib mode) is **shipped**. The
> `host_call()` primitive, the callback registry, and the bidirectional
> dispatch are not yet built, these are "Shape B" work. The `--emit=lib`
> foundation makes them tractable: the runtime is already loadable as a
> library, so adding a host dispatch table and a callback ID registry
> is additive, not invasive.

### The insight

Instead of Aether returning data to Java, **Aether drives Java**.  The
config script runs, calls into the host application through a facade, and
registers closures that the host calls back later.

That's exactly what ARexx did for AmigaOS, every application exposed an
"ARexx port" (a named message interface), and ARexx scripts could reach
into any running app and call commands on it.  The script was the
orchestrator, the apps were the servants.

### What it would look like

**Java side**, expose a facade:

```java
// Java app registers its capabilities before launching Aether
AetherHost host = new AetherHost();
host.expose("add_route",    (method, path, handlerId) -> router.add(method, path, handlerId));
host.expose("set_factory",  (scope, name, factoryId) -> di.register(scope, name, factoryId));
host.expose("set_filter",   (path, handlerId) -> filterChain.add(path, handlerId));
host.expose("get_inventory", () -> inventory.asMap());

host.run("config/server.ae");  // Aether script drives the wiring
```

**Aether side**, calls the host, registers callbacks:

```aether
// server.ae, this IS the config, and it drives the Java app

inv = host_call("get_inventory")

web_server(8080) {
    path("/api/cart") {
        end_point(POST, "/add") |req, res, ctx| {
            hide inv   // can't touch inventory directly here
            sku = request_param(req, "sku")
            host_call("cart_add", sku)
            response_json(res, "{\"added\":\"${sku}\"}")
        }
    }
}
```

The Aether script runs *inside* the Java process.  `host_call()` crosses
the boundary into Java.  The closure handlers are registered as callback
IDs that Java can invoke later when an HTTP request arrives.

### The ARexx parallel

| ARexx (1987) | Aether embedding |
|---|---|
| ARexx port, named message interface per app | Host facade, named functions exposed by Java |
| `ADDRESS 'AppName'` to target an app | `host_call("function", args...)` |
| Script sends commands as strings | Aether calls typed facade functions |
| App returns results to ARexx | Java returns values across FFI boundary |
| Scripts glue multiple apps together | Aether config wires a single app's internals |
| Every Amiga app could be scripted | Any Java app that exposes a facade can be configured |

The key insight ARexx got right: **the scripting language doesn't need to
know the app's internals**.  It only sees the facade.  The app decides what
to expose.  This is exactly `hide`/`seal except` at the process boundary,
the facade IS the whitelist.

### What Aether needs for this

#### 1. `host_call()` the boundary crossing primitive

A new built-in (or extern) that dispatches to the host:

```aether
extern host_call(name: string, ...) -> ptr
```

On the C side, this calls a function pointer table that the embedding host
populated before `main()` runs.  The implementation is small, a map of
name -> function pointer, with a varargs marshalling convention.

#### 2. Callback registration, the reverse direction

When Aether passes a closure to the host (e.g., as a route handler), the
host gets an opaque callback ID.  Later, when an HTTP request arrives, the
host calls back into Aether with that ID:

```c
// Host side (Java via FFI)
void* callback_id = aether_eval_get_callback(config, "handler_3");

// Later, on request:
aether_invoke_callback(callback_id, req, res, ctx);
```

This needs:
- A callback registry in the Aether runtime (closures stored by ID)
- An `aether_invoke_callback()` C entry point
- Thread safety if Java is multi-threaded (mutex around Aether's
  single-threaded runtime, or one Aether runtime per thread)

#### 3. The embedding lifecycle

```
Java starts
  -> creates AetherHost, exposes facade functions
  -> calls aether_run("config.ae")
    -> Aether main() executes
      -> calls host_call() to read state, register routes
      -> closures stored in callback registry
    -> main() returns
  -> Java now has a wired-up route table with live Aether callbacks
  -> request arrives -> Java calls aether_invoke_callback(id, req, res)
    -> Aether closure runs, may call host_call() again
    -> returns
```

#### 4. What changes in the compiler

Even less now that `--emit=lib` is in:

- `host_call` is just an `extern` the compiler already handles those
- Closures already get `box_closure()` / `unbox_closure()` the callback
  registry would be a map of ID -> boxed closure
- ✅ **Done**: shared-library output (`aetherc --emit=lib`) so the
  runtime links as a library, not a standalone executable
- 📋 **Still needed**: `aether_init()` / `aether_invoke_callback()`
  entry points beyond the per-function `aether_<name>` exports we have
  today, and the callback registry to back them

#### 5. Java side

Panama FFI (Java 22+) makes this clean:

```java
Linker linker = Linker.nativeLinker();
SymbolLookup aether = SymbolLookup.libraryLookup("libaether_config.so", arena);

var init = linker.downcallHandle(aether.find("aether_init").get(), ...);
var run = linker.downcallHandle(aether.find("aether_run").get(), ...);
var invoke = linker.downcallHandle(aether.find("aether_invoke_callback").get(), ...);

// Expose facade functions as upcall stubs
var addRoute = linker.upcallStub(this::addRoute, ...);
init.invoke(facadeTable);
run.invoke("config/server.ae");
// Now invoke callbacks on HTTP requests
```

No JNI boilerplate.  Panama handles the marshalling.

### The facade is the real security boundary

It's important to be clear about what provides security here and what
doesn't.

**The facade is the hard boundary.**  The host application decides what
functions to expose.  If the host doesn't expose `db_drop()`, the Aether
script cannot call it, there's no function pointer in the dispatch table,
no way to reach it.  This is the same model ARexx used: the app's port
defines its attack surface.  The host author controls the facade; the
config author works within it.

**`hide`/`seal except` is author-side hygiene, not security.**  It helps
a config author organize their own code, "this handler shouldn't touch
the inventory object", but it's not a sandbox.  A malicious config author
can remove the `hide` line.  The facade is what stops them from
calling things the host didn't expose.  `hide`/`seal except` is for
preventing *accidents* within a team, not for defending against
*adversaries*.

### Risks of a scriptable config language

Any config language that executes as real code introduces risks that
static config (JSON, TOML) doesn't have:

**1. Network access.**  If the embedded Aether runtime has access to
socket APIs, a config file could open a listener, exfiltrate data, or
act as a backdoor.  The embedding mode should **not link `std.tcp`,
`std.http`, or `std.net`** by default.  The facade should be the *only*
way the script talks to the outside world.

**2. File system access.**  A config script that can read `/etc/passwd`
or write to `/tmp` is a liability.  The embedding mode should **not link
`std.fs` or `std.file`** unless the host explicitly opts in.

**3. Process spawning.**  `os_system()` and `os_run()` should be
unavailable in embedded mode.  A config file that can `exec` arbitrary
commands is not a config file, it's a remote code execution vector.

**4. Unbounded computation.**  A config script with an infinite loop
hangs the host.  The embedding runtime should support a **wall-clock
timeout**, if `aether_run()` doesn't return within N milliseconds,
kill it.

**5. Memory exhaustion.**  A script that allocates endlessly can OOM the
host process.  An **allocation budget** (soft limit on `malloc` calls
from the Aether runtime) would mitigate this.

The principle: **in embedded mode, the Aether runtime should be
capability-empty by default.**  No network, no filesystem, no process
spawning, no ambient authority.  Everything the script can do comes
through the facade.  This is the opposite of Aether's normal mode (where
the full stdlib is linked) and must be a distinct compiler/linker
configuration.

### Effort estimate, current state

| Piece | Status |
|---|---|
| `aetherc --emit=lib` → `.so` (per-function exports, no init/run/invoke yet) | ✅ Done |
| Capability-empty runtime (no `std.net|http|tcp|fs|os` by default) | ✅ Done |
| Per-language SDK generators (Python ctypes, Java Panama, Ruby Fiddle) via `ae build --namespace` | ✅ Done (v2) |
| Script → host event signaling (`notify(event, id)` claim-check primitive) | ✅ Done (v2) |
| `host_call` dispatch table + extern (script → host *function calls*) | 📋 Future, Small |
| Callback registry (boxed closure map) | 📋 Future, Small |
| `aether_init()` / `aether_invoke_callback()` lifecycle entry points | 📋 Future, Medium |
| Thread safety for multi-threaded host | 📋 Future, Medium-large |
| Wall-clock timeout + allocation budget | 📋 Future, Medium |
| **Remaining work** | **The "Shape B" milestone, bidirectional dispatch** |

### Conclusion

The ARexx model is the right mental model.  Aether as the wiring language,
the host as the runtime, the facade as the contract.

The facade is the security boundary, it determines what the script can
do.  `hide`/`seal except` is useful hygiene within the script itself,
helping config authors keep their own code disciplined, but it's not what
stops a script from misbehaving.  What stops misbehavior is: the host
doesn't expose the dangerous function in the first place, and the embedded
runtime doesn't link the dangerous standard library modules.

A config language that can listen on a socket is not a config language,
it's an attack surface.  The embedding mode must be capability-empty by
default: no network, no filesystem, no process spawning.  Everything comes
through the facade.

---

## Part 4: Aether hosting Aether, LD_PRELOAD containment

> **Status:** the LD_PRELOAD sandbox library (`libaether_sandbox.so`,
> built from `runtime/libaether_sandbox_preload.c`; the spawn helper
> lives in `runtime/sandbox/spawn_sandboxed_{linux,bsd,stub}.c`) was
> already shipped before this design exploration. With `--emit=lib`
> now in the toolchain, the two layers compose naturally: a host
> Aether application can `ae build --emit=lib` a child script and
> spawn it under the sandbox. The script-side mechanics described
> in this section are still concrete and work today.

### The question

The previous sections assume a Java (or Go, or Rust) host launching
Aether scripts.  But can an Aether application host Aether scripts with
the same containment guarantees?

### The mechanism already exists

Aether already has an `LD_PRELOAD`-based sandbox library
(`libaether_sandbox.so`) documented in
[containment-sandbox.md](containment-sandbox.md).  It interposes on libc
calls, `connect`, `open`, `fopen`, `execve`, `getenv`, `dlopen`,
`mmap`, `mprotect`, `fork`, `syscall` and checks each against a
grant list before allowing it through.

This is the same technique used by container runtimes and process-level
sandboxes.  It works on any dynamically linked binary, regardless of what
language compiled it.

### How Aether hosts Aether

The host Aether application:

1. Compiles the script to a shared library: `ae build rules/pricing.ae --emit=lib -o /tmp/libpricing.so` (✅ this works today)
2. Creates a POSIX shared-memory object encoding the grant list
3. Forks a child process
4. Sets `LD_PRELOAD=libaether_sandbox.so` and `AETHER_SANDBOX_SHM=<shm_name>` in the child's environment. The SHM envvar carries the *name* of the shared-memory object, not the grants themselves, the preload library reads the grants from SHM on startup. (See `runtime/sandbox/spawn_sandboxed_linux.c` for the spawn side and `runtime/libaether_sandbox_preload.c` for the SHM-reading preload side.)
5. `exec`s the script (or `dlopen`s the `.so` in the child)
6. The child runs with libc calls intercepted, any `connect()`, `open()`, `execve()` that isn't in the grant list is denied at the OS level

```aether
// host_app.ae

// Compile the rules script (--emit=lib is shipped)
os_system("ae build rules/pricing.ae --emit=lib -o /tmp/libpricing.so")

// Run it in a sandboxed subprocess. `aether_spawn_sandboxed` is the
// existing C-level helper; its signature is
// aether_spawn_sandboxed(void* grant_list, const char* program, const char* arg)
// and it is not yet exposed as an Aether extern. An Aether-level wrapper
// would expose the same thing with a grant-list builder.
aether_spawn_sandboxed(grants, "/tmp/pricing", input_json)
```

### Why LD_PRELOAD, not just a facade

The facade model (Part 3) works when the host controls *compilation*,
it decides which `extern` functions get linked.  But a compiled `.so`
is native code.  If it contains inline assembly, raw `syscall()`
instructions, or was compiled from a modified compiler, the facade means
nothing, the code can do whatever the process can do.

`LD_PRELOAD` operates at a lower level: it intercepts the libc functions
that *any* dynamically linked code must call to talk to the kernel.  Even
if the Aether script somehow smuggled in a raw `connect()` call, the
preload library catches it.

The two layers complement each other:

| Layer | What it stops | Limitation |
|---|---|---|
| **Facade** (compile-time) | Script can't *name* functions the host didn't expose | Script could declare its own `extern` or use inline C |
| **LD_PRELOAD** (runtime) | Script can't *execute* syscalls the sandbox didn't grant | Statically linked binaries bypass it (no libc) |

For Aether-hosts-Aether, both layers apply: the compiler controls what
gets linked (facade), and `LD_PRELOAD` intercepts what gets executed at
runtime.  Together they cover the compile-time and runtime escape paths.

### What this means for rules and config scripts

The deployment model becomes:

```
/opt/myapp/
    bin/myapp                    # host Aether application
    lib/libaether_sandbox.so     # LD_PRELOAD interception library
    rules/
        pricing.ae               # source, editable, redeployable
        pricing.so               # compiled, rebuilt on change
        fraud.ae
        fraud.so
    grants/
        pricing.grants           # tcp:none; fs_read:none; exec:none
        fraud.grants             # tcp:fraud-api.internal:443; fs_read:none
```

Each rules script gets its own grant file.  The host reads the grants,
sets up the `LD_PRELOAD` environment, and runs the script in a child
process.  The grants file is the security policy, auditable, diffable,
reviewable in a PR.

A pricing rule that tries to `connect()` to an unexpected host gets
`AETHER_DENIED: tcp evil.com:80` in the sandbox log.  The host sees the
child exit with an error.  No data exfiltrated.

### The Aether-specific advantage

Most languages that support `LD_PRELOAD` containment (C, C++, Rust, Go
with CGO) are general-purpose, the contained process is a full program
that happens to be sandboxed.  Aether's advantage is that the *language
itself* is designed for the builder-DSL-with-closures pattern:

- The script's *structure* (trailing blocks, nested scopes) mirrors the
  system it configures
- `hide`/`seal except` provides author-side hygiene *within* the script
  (not security, the facade and LD_PRELOAD handle security)
- The facade is a natural fit for Aether's `extern` mechanism
- The compilation to `.so` is already part of the toolchain

The result: an Aether application can host Aether rules/config scripts
with the same containment guarantees a Java application would get,
facade for the API boundary, `LD_PRELOAD` for the OS boundary, and
`hide`/`seal except` for the rule author's own code organization.
