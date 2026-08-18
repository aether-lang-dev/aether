# Aether DSL as a Rules Engine

A design exploration: using Aether's trailing-block DSL and `hide`/`seal
except` to build a rules engine where business rules live in `.ae` files
alongside, but deployed independently of, the application binary.

> **Status (2026-04-18):** the **embedding foundation is built and v2
> namespaces have shipped**. `aetherc --emit=lib` produces a `.so`/`.dylib`
> with stable C-ABI exports (see [`emit-lib.md`](../emit-lib.md)), and
> `ae build --namespace <dir>` generates idiomatic per-language
> SDKs (Python ctypes, Java Panama, Ruby Fiddle) plus `notify(event, id)`
> for script → host event signaling, see
> [`embedded-namespaces-and-host-bindings.md`](../embedded-namespaces-and-host-bindings.md)
> for the typed-SDK story. What's still future work for the rules-engine
> vision specifically:
>
> | Piece | Status |
> |---|---|
> | Lib mode + capability-empty default | ✅ Done (`--emit=lib`) |
> | C ABI for walking returned data | ✅ Done (`aether_config.h`) |
> | Per-language host SDK generators | ✅ Done (v2: Python/Java/Ruby; Go stubbed) |
> | Script → host event signaling (`notify(event, id)`) | ✅ Done (v2) |
> | `rules` stdlib module (`field`, `is_email`, `to_int`, `>>`, etc.) | 📋 Future, ~200-300 lines |
> | Host-side callbacks (`host_call`, callback registry), script *calling into* the host | 📋 Future (Shape B) |
> | `ae eval` subprocess + JSON serializer | 📋 Future (possibly obsolete given lib mode) |
> | File-watch + hot-swap | 📋 Future, optional |
> | Wall-clock timeout / allocation budget | 📋 Future |
>
> Section-by-section annotations below mark each item.

---

## Prior art: grules (2014)

[Grules](https://github.com/pihme/grules) was a Groovy-based rules engine
built on a simple premise: **validation and preprocessing rules should
live in separate files that change on a different cadence than the
application.**

A Grails web app might redeploy its WAR once a month, but the business
rules for "what counts as a valid order" change weekly.  Grules solved
this by putting rules in standalone Groovy scripts:

```groovy
// OrderGrules.groovy, deployed independently of the app JAR
email isEmail ["Invalid email"]
age toPositiveInt ["Invalid age"] >> {it > 18} ["Must be adult"]
sku isAlphanum >> isIn(activeCatalog) ["Unknown SKU"]
```

The application loaded and evaluated these at runtime:

```groovy
def result = grules.applyRules(OrderGrules, params)
if (result.invalidParameters.isEmpty()) {
    processOrder(result.cleanParameters)
} else {
    showErrors(result.invalidParameters)
}
```

Key properties:
- Rules were **declarative**, a DSL, not general-purpose code
- Rules were **chainable**, `>>` piped a converted value to the next check
- Rules were **separately deployable**, update `OrderGrules.groovy`,
  restart the rule loader, no app redeploy
- Rules had **built-in type coercion**, `toPositiveInt` converts and
  validates in one step
- Results were **categorized**, clean, invalid, missing, not-validated

The limitation: Groovy's dynamic typing meant rule errors surfaced at
runtime, not compile time.  And being JVM-only, it couldn't serve as a
cross-language rules format.

---

## The Aether version

Aether's trailing-block DSL, closures, and `hide`/`seal except` can
express the same pattern with two advantages grules never had:

1. **Compile-time scope enforcement**, a rule block can declare exactly
   which names it's allowed to reference.  A pricing rule that shouldn't
   see the customer's payment details can `hide` them.
2. **Compiled to native code**, rules compile to C, not interpreted
   Groovy.  The performance ceiling is higher and the deployment artifact
   is a `.so`/`.dylib`, not a classfile.

### What the DSL could look like

```aether,fragment
// order_rules.ae, lives next to the app binary, deployed separately

import rules   // hypothetical rules stdlib module

rules("order_validation") {

    field("email") {
        is_email ["Invalid email address"]
    }

    field("age") {
        to_int ["Age must be a number"]
        >> is_greater_than(18) ["Must be 18 or older"]
    }

    field("sku") {
        is_alphanum ["SKU must be alphanumeric"]
        >> is_in(active_catalog) ["Unknown SKU"]
    }

    field("quantity") {
        to_positive_int ["Quantity must be a positive number"]
        >> is_less_than(max_order_qty) ["Exceeds maximum order quantity"]
    }

    // Computed field, derives from validated inputs
    computed("total") {
        seal except quantity, unit_price, discount_rate
        return quantity * unit_price * (1.0 - discount_rate)
    }
}
```

### Scope control in rules

The interesting part isn't the validation DSL (any language can do that).
It's what `hide`/`seal except` adds.

Consider a multi-tier rule set for an e-commerce checkout:

```aether,fragment
// checkout_rules.ae

checkout_rules(order: ptr, customer: ptr, payment: ptr) {

    // Tier 1: Order validation, can see order + catalog, nothing else
    rules("order") {
        seal except order, active_catalog, field, to_int,
                    to_positive_int, is_alphanum, is_in, is_greater_than
        field("sku") { is_alphanum >> is_in(active_catalog) }
        field("quantity") { to_positive_int >> is_greater_than(0) }
    }

    // Tier 2: Customer validation, can see customer, NOT payment
    rules("customer") {
        hide payment
        field("email") { is_email }
        field("shipping_country") { is_in(allowed_countries) }
    }

    // Tier 3: Payment validation, can see payment, NOT full customer record
    rules("payment") {
        hide customer
        field("card_last4") { is_length(4) >> is_numeric }
        field("amount") { to_positive_decimal >> is_less_than(charge_limit) }
    }

    // Tier 4: Fraud scoring, can see everything (no hide/seal)
    // Only this tier gets the full picture.
    rules("fraud") {
        score = fraud_score(order, customer, payment)
        if score > threshold { reject("Fraud score too high") }
    }
}
```

Each tier declares its own visibility boundary.  The order validation
rules literally *cannot* reference `payment.card_number` the compiler
rejects it.  This isn't a runtime policy; it's a compile-time guarantee
that the rule author encoded into the rule file.

A compliance auditor can read the `seal except` line and know: this rule
block touches *these* names and nothing else.

---

## The deployment model: rules adjacent to the binary

The key grules insight was: **rules change faster than application code.**
A pricing formula changes quarterly.  A fraud threshold changes weekly.
A validation regex changes when a new TLD is registered.  The application
binary, the HTTP server, the database layer, the deployment pipeline,
changes on a different, slower cadence.

Aether can support this with a simple model:

```
/opt/myapp/
    bin/myapp           # compiled Aether (or Java/Go/Rust) application
    rules/
        order.ae        # ← these change frequently
        pricing.ae      # ← recompiled and redeployed independently
        fraud.ae        # ← without touching bin/myapp
    lib/
        libaether.so    # Aether runtime (for embedded mode)
```

### How it works

**Option A: Subprocess** (simplest)  📋 Not built

The application would shell out to compile and run the rules file:

```
$ ae eval rules/order.ae --input '{"email":"bad","age":"35"}'
{"clean":{"age":35},"invalid":{"email":"Invalid email"}}
```

`ae eval` doesn't exist yet, and the in-process Option B path now
covers the same use case without a serialization round-trip.

**Option B: Embedded library** (faster)  ✅ Substantially shipped / 📋 Callbacks future

Using the v2 embedded-namespace model from
[Aether as a Config Language v2](../embedded-namespaces-and-host-bindings.md):

```aether,fragment
// rules/manifest.ae
import std.host
abi() {
    describe("order_rules") {
        input("max_order_qty", "int")
        input("charge_limit", "int")
        event("OrderRejected", "int64")
        event("FraudFlagged",  "int64")
        bindings() { java("com.example.rules", "OrderRules") }
    }
}
```

```java
// Java host, using the generated SDK
try (OrderRules rules = new OrderRules("rules/liborder_rules.so")) {
    rules.setMaxOrderQty(1000);
    rules.setChargeLimit(100_000);
    rules.onOrderRejected(id -> alertService.flag(id));
    rules.onFraudFlagged(id  -> fraudService.review(id));
    int rc = rules.validateOrder(orderId, amount, sku, customerId);
}
```

**What works today:** typed setters per `input(...)`, typed event
handlers per `event(...)`, methods named after each Aether function,
`AutoCloseable` lifecycle. The script signals events via
`notify("OrderRejected", id)` claim-check pattern, host looks up
detail by id. Worked end-to-end test in
`tests/integration/embedded_java_trading_e2e/`.

**What needs Shape B (the callback half):** rules that need to *call
into* the host (look up a SKU in the live catalog, fetch a fraud score
from a Java service) need `host_call`. Until that ships, those lookups
have to be passed in as arguments, i.e. the host pre-computes anything
the rule might need and hands it in as a map (or as typed input
declarations in the manifest). Workable for many cases, limiting for
others where the script needs to make ad-hoc lookups mid-evaluation.

**The facade is the security boundary.**  The rules file sees only what
the host exposes, if the host doesn't provide a `db_drop()` function,
the rules script cannot drop the database.  Period.  The host author
decides the attack surface.

`hide`/`seal except` is a secondary, weaker layer: it helps *rule authors*
organize their own code ("this pricing tier shouldn't touch the customer's
payment details"), but it's hygiene, not security.  A malicious rule author
could remove the `hide` line.  What they can't do is call a function the
facade never exposed.

**Option C: File-watch + recompile**  📋 Future, optional

A file watcher detects changes to `rules/*.ae`, recompiles to `.so`,
and hot-swaps the loaded library.  The application never restarts.  This
is the grules model (Groovy's classloader hot-swap) but with compiled
native code.

```
[file watcher] rules/order.ae changed
  -> ae build rules/order.ae --emit=lib -o rules/liborder.so   ✅ this works today
  -> dlclose(old_handle)                                       ✅ standard libdl
  -> dlopen("rules/liborder.so")                               ✅ standard libdl
  -> ready (next request uses new rules)                       📋 needs the host shim
```

The compile + dlopen pieces all work today. The file watcher itself
and the dlclose-then-dlopen plumbing in the host application are not
shipped, they'd be a small shim around the existing primitives.

---

## Risks: when a rules file is code, not data

A rules file that executes as real code introduces risks that a static
rules format (JSON, CSV lookup tables) doesn't have.  These risks apply
to *any* programmable rules engine, grules, Starlark, Lua in Redis,
Aether, and must be addressed head-on.

**1. Network access.**  If the rules runtime can open sockets, a rule
file could exfiltrate data, call home, or listen for commands.  The
embedded Aether runtime **must not link `std.tcp`, `std.http`, or
`std.net`** in rules mode.  The facade should be the *only* way the
rules script communicates with the outside world.

**2. File system access.**  A rules script that can read `/etc/shadow`
or write to disk is not a rules script, it's a backdoor.  The embedded
runtime **must not link `std.fs` or `std.file`** unless the host
explicitly opts in.

**3. Process spawning.**  `os_system()` and `os_run()` must be
unavailable.  A rules file that can `exec` arbitrary commands is a
remote code execution vulnerability.

**4. Unbounded computation.**  An infinite loop in a rules file hangs
the host.  The embedded runtime should support a **wall-clock timeout**
so `aether_run()` is killed after N milliseconds.

**5. Memory exhaustion.**  Runaway allocation can OOM the host process.
An **allocation budget** (soft limit on malloc from the Aether runtime)
would cap the blast radius.

The principle: **in embedded/rules mode, the Aether runtime should be
capability-empty by default.**  No network, no filesystem, no process
spawning, no ambient authority.  Everything the rules script can do comes
through the facade.  This is the opposite of standalone Aether (where the
full stdlib is linked) and must be a distinct compiler/linker mode.

---

## Comparison with grules

| Aspect | Grules (Groovy, 2014) | Aether rules DSL |
|---|---|---|
| Rule language | Groovy DSL (dynamic) | Aether DSL (compiled) |
| Error detection | Runtime | Compile time |
| Security boundary | Groovy binding (everything visible) | Facade (host controls exposure) |
| Author-side hygiene | None | `hide`/`seal except` per rule block |
| Performance | JVM interpreted/JIT | Native code via C |
| Deployment | Drop `.groovy` file, classloader reload | Drop `.ae` file, recompile to `.so` |
| Chaining | `>>` operator | `>>` operator (same idea) |
| Type coercion | `toInt`, `toPositiveInt`, etc. | Same pattern, stdlib functions |
| Host integration | Groovy binding context | Facade + extern functions |
| Ambient capabilities | Full JVM (sockets, files, reflection) | Capability-empty by default |
| Cross-language | JVM only | Any language with C FFI |

### What Aether adds that grules couldn't

1. **The facade as a hard boundary.**  Grules rules ran inside the JVM
   with full access to the classpath, any rule could instantiate any
   class, open any socket, read any file.  The Groovy binding was a
   convenience layer, not a security boundary.  Aether's embedded mode
   with a capability-empty runtime and a host-controlled facade is a
   genuine containment mechanism.

2. **Author-side hygiene with `hide`/`seal except`.**  Within the rules
   file itself, rule authors can organize tiers of visibility, "the
   pricing rule shouldn't touch the payment object."  This is *self-
   discipline tooling*, not a security feature, but it makes rule files
   more readable and auditable.  Grules had no equivalent.

3. **Native compilation.** Grules ran on the JVM and paid for Groovy's
   dynamic dispatch on every rule evaluation.  Aether compiles to C.
   For high-throughput rule evaluation (ad bidding, real-time fraud
   scoring, packet filtering), this matters.

4. **Cross-language embedding.** Grules was JVM-only.  Aether's C ABI
   means any language with FFI, Java (Panama), Go (cgo), Rust (extern C),
   Python (ctypes), Node (N-API), can load and run Aether rules.

---

## What would need to be built

### In Aether's stdlib  📋 Future

A `rules` module providing:
- `field(name)` DSL function that binds a parameter name for validation
- Built-in validators: `is_email`, `is_alphanum`, `is_length`, `is_in`,
  `is_greater_than`, `is_less_than`, `is_between`, `matches`, etc.
- Built-in converters: `to_int`, `to_float`, `to_positive_int`,
  `to_date`, `to_bool`, etc.
- `>>` chaining (already supported as a language operator or function
  composition)
- Result aggregation: clean/invalid/missing parameter categorization
- `computed(name)` derived fields from validated inputs

### In the compiler

- ✅ **Done**: shared library output (`aetherc --emit=lib`), with
  capability-empty link profile rejecting `std.net|http|tcp|fs|os`.
  Renamed from the original `--embed` proposal to fit alongside
  `--emit=exe` and `--emit=both`.
- 📋 **Future**: `ae eval` subcommand for the subprocess model
  (likely deprioritized given the lib-mode in-process path)

### In the runtime

- ✅ **Done**: capability-empty link profile (no net/fs/exec by default)
- ✅ **Done (v2)**: `notify(event, id)` claim-check primitive +
  per-language SDK trampolines for event handlers
- 📋 **Future**: wall-clock timeout and allocation budget
- 📋 **Future, optional**: file-watch + hot-swap infrastructure
  (for Option C)
- 📋 **Future**: `host_call` registry for the bidirectional model
  script *calling into* the host (Shape B)

### Effort

The embedding infrastructure overlapped with the
[config language proposal](../aether-embedded-in-host-applications.md)
and **landed there first**, then the
[v2 namespace layer](../embedded-namespaces-and-host-bindings.md)
shipped on top of it. The rules-specific remaining work is:
- The `rules` stdlib module (~200-300 lines of validation functions
  and the `field()` / `computed()` DSL wrappers), pure stdlib code,
  no compiler changes.
- Optionally, the host-side callback registry for rules that need to
  call back into the host application (Shape B).

---

## The pitch

Every rules engine faces the same tension: rules need to be **expressive
enough** to capture business logic, but **constrained enough** that a rule
can't do things its host didn't authorize.

Most rules engines solve this with a restricted DSL that can't do much
(Drools' DRL, grules' operator DSL) or a general-purpose language with
runtime sandboxing (Starlark, Lua in Redis).

Aether offers a third option: **a real programming language with a
capability-empty embedded runtime and a host-controlled facade.**  Rule
authors write real code, closures, maps, string interpolation, control
flow, but everything the rules script can *do* comes through functions
the host explicitly exposed.  No ambient network.  No ambient filesystem.
No `exec`.

Status: the **capability-empty runtime is shipped** (`aetherc --emit=lib`
rejects the I/O-heavy stdlib modules at compile time), and the **typed
host SDK** is shipped (v2: `ae build --namespace` emits a Python ctypes
module / Java Panama class / Ruby Fiddle module per declared binding).
**Host-controlled facade is half-shipped**, host → script *data* flow
works today via typed input setters; script → host *event* flow works
today via `notify(event, id)`; only script → host *function calls*
(`host_call`) are still future work. Until that ships, the host
pre-computes anything the rule needs and passes it in via the
manifest's `input(...)` declarations.

`hide`/`seal except` adds a secondary layer: rule authors can organize
their own visibility within the file, making rules more readable and
auditable.  But it's the facade that provides containment.

That's grules' vision, rules adjacent to the app, deployed independently
with the hard boundary that grules never had: the rules script literally
cannot do anything the host didn't hand it a function for.

### Aether hosting Aether rules

The host application doesn't have to be Java or Go.  An Aether
application can host Aether rules scripts with the same containment, using
the `LD_PRELOAD`-based sandbox (`libaether_sandbox.so`) already documented
in [containment-sandbox.md](../containment-sandbox.md).  The host compiles
the rules script to a `.so`, runs it in a child process with
`LD_PRELOAD` active, and libc calls (`connect`, `open`, `execve`, etc.)
are intercepted at the OS level against a grant list.

This gives two containment layers: the **facade** controls what functions
the script can *name* (compile-time), and **LD_PRELOAD** controls what
syscalls the script can *execute* (runtime).  Together they cover both
escape paths.  See
[Aether as a Config Language, Part 4](../aether-embedded-in-host-applications.md#part-4-aether-hosting-aether-ld_preload-containment)
for the full design.
