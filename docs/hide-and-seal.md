# `hide` and `seal except` scope-level capability denial

Aether provides two scope-level directives that let a block decline to
see selected names from its enclosing lexical scopes. They are the
language-level expression of the ["Principles of Containment"](
https://paulhammant.com/2016/12/14/principles-of-containment/), capability
flow becomes explicit not just on the way *in* (via dependency injection)
but also on the way *out* (via name denial).

## The two forms

### `hide` blacklist

```aether,fragment
{
    hide secret_token, db_handle
    // From here on, `secret_token` and `db_handle` are not in scope, even
    // though they're declared in an outer block. Reading them is a compile
    // error. Reassigning them is a compile error. Declaring a fresh
    // variable with the same name in this block is a compile error.
}
```

### `seal except` whitelist

```aether,fragment
{
    seal except req, res, inventory, response_write, response_write_status
    // Every name from every outer scope is now invisible EXCEPT the five
    // listed here. The block can still create its own local variables
    // freely; it just can't reach out for ambient state.
}
```

`seal except` is the form you reach for when writing a request handler
or any block where you want a one-line audit of "what does this code
even see?". The whitelist is the dependency surface.

## Semantics

### Scope-level, not statement-level

The position of a `hide` directive within its block does not matter.
Either of these is valid and equivalent:

```aether,fragment
{
    fooStr = "abc"
    hide fooStr        // hide appears AFTER the declaration
    use_helper()
}
```

```aether,fragment
{
    hide fooStr        // hide appears FIRST
    fooStr = "abc"     // ERROR: cannot declare 'fooStr', it is hidden
}
```

The first compiles cleanly because `fooStr` was declared in the outer
scope (not in the inner block), and the inner block hides it. The second
fails because the inner block is now trying to *create* a local binding
called `fooStr`, which collides with the hidden outer name.

### Reads AND writes both blocked

`hide x` blocks `println(x)`, `x = 5`, `x += 1`, and `do_stuff(x)`,
every form of access. Half-hiding (read-only or write-only) would be a
footgun.

### Propagates to all nested blocks

If the outer block hides `x`, every nested block, closure, and
trailing-block lambda inside it also has `x` hidden. There is no way to
"un-hide" a name in a nested block, declaring a fresh variable with
the same name in a nested block is allowed, but that's a fresh binding
in a child scope, not a re-exposure of the parent's hidden binding.

### Does NOT reach through call boundaries

A visible function defined in an outer scope can still use the hidden
name via its own lexical chain:

```aether
log_secret() { println(secret_token) }   // sees secret_token

main() {
    secret_token = "abc"
    {
        hide secret_token
        log_secret()                      // OK, log_secret is visible,
                                          //      and reads secret_token
                                          //      via its OWN lexical scope
    }
}
```

This is deliberate. `hide` is about *your scope's name resolution*, not
about *information flow* through your callees. If you wanted to deny
the capability transitively, you needed an effect type system; Aether
doesn't claim to have one.

The same caveat applies to closures captured into local variables:

```aether,fragment
{
    secret = 42
    incr = || { secret = secret + 1 }     // closure captures `secret`
    {
        hide secret
        incr()                             // legal, incr was already
                                           // captured before the hide
    }
}
```

If you don't want the closure's mutation to land, don't expose the
closure into the hiding scope.

### Applies to qualified (dotted) names

`hide` and `seal except` apply to the prefix of qualified names. If you
hide a namespace, all member access through that namespace is blocked:

```aether,fragment
import std.http
{
    hide http
    r = http.get("http://example.com")   // rejected: http is hidden here
}
```

The hidden prefix is refused before namespace resolution, so the call
never resolves. The diagnostic on a hidden dotted call reports the name
as undefined (`E0301: Undefined function 'http.get'`) rather than the
`E0304` you get for a hidden bare name, the qualified lookup returns no
symbol once the prefix is denied. Either way the access is blocked.

(Don't reach for a built-in type keyword like `string` here: `hide
string` fails earlier with `E0100`, because `string` is a reserved
keyword and can't be used as an identifier at all.)

### Works inside actor receive arms

Receive handler bodies are block scopes, so `hide` and `seal except`
work exactly as in any other block:

```aether,fragment
actor Server {
    state secret = "do not touch"
    state public_val = "ok"

    receive {
        Ping() -> {
            hide secret
            // public_val is still visible; secret is not.
            println(public_val)
        }
        Greet(name) -> {
            seal except name, greeting, println
            // Only name, greeting, and println are visible from
            // outer scopes. Other state variables are sealed out.
        }
    }
}
```

This is particularly useful for actors with multiple state variables
where individual receive arms should only touch a subset of state.

### Local bindings are always visible

`hide` only affects lookups that walk OUT of the current block into a
parent scope. A local binding declared inside the hiding block is
always visible inside that block, regardless of what's hidden:

```aether,fragment
{
    seal except println
    local_thing = "fresh"
    println(local_thing)    // OK, local_thing is local, not from outside
}
```

## What `hide` is NOT

- **Not an effect system.** It cannot prevent a function you call from
  reaching its own ambient state. It can only stop *your scope* from
  naming things directly.
- **Not a security boundary.** It's compile-time hygiene. A determined
  caller can route around it by exposing a closure or accessor function.
  It catches *accidents* and *enforces intent*, not malice.
- **Not a privacy modifier.** Java `private` means "this field is not
  part of my class's API". `hide` means "this scope declines to see this
  name". Different layer.

## What `hide` IS for

1. **Containment of ambient authority in handlers.** When a request
   handler is buried 200 lines into a big function, it's easy to
   accidentally close over a sensitive variable. A one-line `hide` at
   the top of the handler block prevents that class of bug at compile
   time.
2. **Auditable dependency surfaces.** Reading a `seal except a, b, c`
   handler, you know in one glance the entire set of names it can
   reach. No more grepping outward for ambient state.
3. **Capability-style guarantees without buying into a capability
   language.** E and Joe-E achieve "only what you were handed" by
   *constructing* every reference through capability objects. `hide`
   achieves "only what you didn't decline" by *subtracting* from
   lexical scope. Cheaper, weaker, available today.
4. **The DI flip-side of dependency injection.** DI delivers
   capabilities to a handler. Without `hide`, the handler can always
   reach around the injection and grab ambient state, the DI is
   convention. With `hide`/`seal except`, the handler is a closed
   container with a declared dependency surface, and the DI is
   enforced at the call site.

## Errors

Hide / seal violations all use the new error code:

```
E0304: 'secret' is hidden in this scope by `hide` or `seal except`
```

with a source line and column pointing at the offending use. Trying to
declare a hidden name produces the same code with the message
"cannot declare 'secret', it is hidden in this scope by `hide`".

## Interaction with shadowing

You may NOT declare a variable in the same scope where its name is
hidden:

```aether,fragment
{
    hide x
    var x = 5     // E0304: cannot declare 'x', it is hidden
}
```

You MAY declare it in a nested child block, that's a fresh binding in
the child's own scope, lexically unrelated to the hidden outer one:

```aether,fragment
{
    hide x
    {
        var x = 5     // OK, fresh binding in inner block
        println(x)    // refers to the inner binding
    }
}
```

This is consistent with normal lexical shadowing and means
`hide` cannot trap a programmer who wants to use a popular name like
`i` or `x` for unrelated purposes deeper in the call tree.

## Minimal reference card

| Form | Effect |
|---|---|
| `hide a, b, c` | Names `a`, `b`, `c` from outer scopes are not in this block (or any nested block within it). |
| `seal except a, b, c` | EVERY name from outer scopes is invisible in this block, except `a`, `b`, `c`. Local bindings inside the block are still visible. |
| `hide x` followed by `var x = …` in the same scope | Compile error, cannot redeclare a hidden name. |
| `hide x` then a nested block declaring `var x = …` | OK, fresh binding in the child scope. |
| `hide x` then calling a visible function that reads `x` from its own scope | OK, name resolution at the call site doesn't touch `x`. |
| `seal except printf, malloc` then trying to call `free` | Compile error, `free` is not in the whitelist. |
| `hide http` then `http.get("x")` | Compile error, qualified access blocked because the prefix is hidden (reported as `E0301` undefined, since the hidden prefix resolves to no symbol). |
| `hide` or `seal except` inside an actor receive arm | Works, receive arm bodies are block scopes like any other. |

## Implementation note

`hide` and `seal except` are enforced entirely at compile time, in
`compiler/analysis/typechecker.c`'s `lookup_symbol()` and
`lookup_qualified_symbol()`. When a name is not found in the current
scope, `lookup_symbol()` checks the scope's `hidden_names` list and
`seal_whitelist` before walking to the parent.
`lookup_qualified_symbol()` checks hide/seal on the dotted prefix
before namespace resolution, so `hide http` blocks `http.get()` too.
Local bindings always win, the hide/seal sets only affect the
boundary crossing into outer scopes.

There is no runtime overhead. There is no codegen change. The feature
is a pure compile-time hygiene check.

---

# Worked example: a solid IoC alternative to Java-style DI

> Up to here is the language reference. The rest of this document is a
> long-form worked example showing how `hide` / `seal except` enables
> a dependency-injection story that is smaller, simpler, and
> compiler-enforced, in contrast to the Java `DependencyManager` /
> `ComponentCache` hierarchy that the `contrib/tinyweb/` Aether port left
> behind. Skip if you only wanted the language reference.

## Q. Do we now have a solid IoC alternative to Java-style DI?

> *In the `contrib/tinyweb/` Aether conversion of the Java TinyWeb thing
> (`~/scm/tiny`) we left out `DependencyManager` and `Cache` do we now
> have a solid inversion-of-control alternative to that Java hackery?*

Yes, with `hide` / `seal except` we now have a **better** alternative
than the Java pattern, and it's smaller too.

## The key insight

The Java `DependencyManager` exists to solve a problem Java has and
Aether (now) doesn't:

> *"How do I make sure a request handler can only reach the things I
>  deliberately handed it?"*

Java's default is: every static method, every singleton, every
classpath-reachable type is one keystroke away from any handler. The DI
container is a *convention layer* on top of that, it organizes
construction order, caches instances, and handles transitive deps, but
**it cannot stop a handler from reaching around it**. `static
Foo.INSTANCE.doStuff()` always wins. The container is fundamentally
polite, not enforcing.

Aether with `seal except` flips this. The handler **declares its
dependency surface in one line**, and the compiler enforces it. You
don't need a framework to give you containment, you have a language
feature. The "container" is just ~80 lines of map plumbing.

## What it looks like

Registration at server-build time:

```aether,fragment
srv = web_server(8080) {

    // Three scopes, the factory's scope determines its cache lifetime.
    app_factory("inventory") callback { return new_inventory() }
    app_factory("db") callback { return open_db() }

    session_factory("cart") callback { return new_cart() }
    session_factory("user_prefs") callback {
        // Factories can themselves use `dep` to ask for other things.
        // The resolver walks app → session → request looking for them.
        u = dep(ctx, "user")
        return load_prefs(u)
    }

    request_factory("logger") callback {
        return new_request_logger(dep(ctx, "request_id"))
    }

    path("/cart") {
        end_point(POST, "/add") |req, res, ctx| {
            seal except req, res, ctx, dep, response_write, response_write_status

            inventory = dep(ctx, "inventory")   // app-scoped (cached for server lifetime)
            cart      = dep(ctx, "cart")        // session-scoped (cached per session)

            sku = request_get_query_param(req, "sku")
            if map_has(inventory, sku) == 0 {
                response_write_status(res, "unknown sku", 404)
                return 0
            }
            cart_add(cart, sku)
            response_write(res, "added")
        }
    }
}
```

The `seal except` line is **the entire dependency surface** of that
handler. A reviewer reads it and knows: this handler can touch `req`,
`res`, `ctx`, `dep`, `response_write`, `response_write_status`. Nothing else
from any outer scope. If someone later edits the handler to grab `db`
directly, the compiler refuses.

## What you give up vs. Java

| Java DM | Aether equivalent | Trade |
|---|---|---|
| `dep(Cart.class)` returns typed `Cart` | `dep(ctx, "cart")` returns generic `ptr` | Lose compile-time type safety on lookups. Mitigate with typed wrappers: `cart(ctx)` that internally does `(Cart*) dep(ctx, "cart")`. One line per type. |
| Constructor-injection static graph analysis at startup | Name-based lookup deferred to first request | Mitigate with a `validate(srv)` pass at `server_start` that walks every registered factory's body and ensures every name it asks for has a registration somewhere. ~30 lines of compile-time-ish reflection over the closure registry. |
| `@Inject` / `@Component` / `@Singleton` / `@RequestScoped` annotations | Three explicit functions: `app_factory`, `session_factory`, `request_factory` | Aether's wins, you can see at the registration site exactly what scope it's in. No "where is this annotation processed?" hunt. |
| `ComponentCache` / `DefaultComponentCache` / `UseOnceComponentCache` interface hierarchy | Three maps carried in `ctx`: `app_cache`, `session_cache`, `request_cache` | Aether's wins, no interface, no implementation classes, no factory pattern, just maps. |
| `DependencyException` when resolution fails | `dep(ctx, name)` returns null + a runtime check, or `validate(srv)` catches it at startup | Comparable. |
| `RequestContext.dep()` interface | A `dep(ctx, name)` function | Same. |

The biggest real loss is **type safety on lookups**. The fix is one
wrapper function per type:

```aether,fragment
inventory(ctx) { return dep(ctx, "inventory") }   // returns ptr but named-as-inventory
cart(ctx)      { return dep(ctx, "cart") }
db(ctx)        { return dep(ctx, "db") }
```

Now handlers say `cart(ctx)` instead of `dep(ctx, "cart")`, and a typo
on the dep name is caught at startup by `validate(srv)` walking the
factory closures. Type errors on USING the result are caught the moment
you try to pass it to a function expecting the wrong shape.

## What you GAIN over Java

1. The `seal except` line is the constructor injection list. There is no
   `@Inject final Cart cart;`, no constructor, no field declaration,
   just the names on one line.

2. **The handler is a closure, not a class.** No `new
   CartAddHandler(deps...)` boilerplate, no class file per endpoint, no
   inheritance from `AbstractHandler`. The handler IS the trailing
   block.

3. **Compile-time enforced containment.** Java's DM is a convention,
   `Foo.INSTANCE` always wins. Aether's `seal except` is enforced, the
   compiler refuses to resolve `Foo.INSTANCE` at all if it's not
   whitelisted.

4. **Factories ARE closures.** They capture their lexical scope at
   registration time, so a factory that needs config can just close
   over it:

   ```aether
   load_config_first()    // returns a config object
   app_factory("db") callback {
       return open_db(config.db_url)   // closes over config
   }
   ```

   No `@ConfigurationProperties`, no `Environment` injection, no
   `@Value("${db.url}")`. Just a captured variable.

5. **Scopes are just maps in a parent chain.** `request_cache →
   session_cache → app_cache → null`. Resolution walks upward. Same
   shape as `SymbolTable`'s parent chain in the compiler's
   `lookup_symbol()`. IoC here is name resolution at runtime, and
   `lookup_symbol` is name resolution at compile time; the same
   parent-chain pattern carries both.

6. **Lifetimes are explicit at the call site.** `app_factory(...)`
   reads as "this lives for the server's life". `session_factory(...)`
   reads as "this is per session". You don't have to look up an
   annotation processor or a Spring config class to understand a
   component's lifetime.

## What it'd take to ship in `contrib/tinyweb/`

Roughly **80 lines** of additions to `module.ae`:

- `app_factory(_ctx, name, factory_fn)` register at server scope
  (uses the `_ctx` builder context that `web_server { … }` creates)
- `session_factory(_ctx, name, factory_fn)` same
- `request_factory(_ctx, name, factory_fn)` same
- `dep(ctx, name)` walk request → session → app, build via factory
  if not cached, store in the right cache, return
- cycle detection, small set on the resolution stack
- `validate(srv)` optional: walk all factory closures looking for
  `dep(ctx, …)` calls and assert each name resolves to a registered
  factory

Plus session-cache lifecycle hooks on the WebSocket / cookie session
id. That's another ~20 lines, and most of it is already implied by
TinyWeb's existing session handling (which it doesn't yet expose, but
the hooks are there).

## Solid alternative, verdict

**Yes, solidly.** The combination of:

1. `seal except` enforced at compile time = real containment
2. `app_factory` / `session_factory` / `request_factory` = explicit lifetimes
3. `dep(ctx, name)` walking a parent chain of maps = trivial runtime resolution
4. Optional `validate(srv)` at startup = catches most type-safety regressions
5. Optional one-line typed wrappers per dep name = restore most Java-like ergonomics

…gets you everything Java's DM gives you, **plus enforcement**,
**minus the framework**. The `DependencyManager` / `ComponentCache` /
`UseOnceComponentCache` / `DefaultComponentCache` four-class hierarchy
in Java collapses into a map and a function that walks a parent chain.

The Java version was load-bearing because Java needed it. Aether
doesn't, it has `seal except` in the language, which is the
load-bearing primitive. Everything else is sugar.

## Test coverage that would ship with the implementation

When this lands in `contrib/tinyweb/`, the test coverage should be
comparable in shape to the `hide` / `seal except` implementation
(though smaller, it's all userland Aether, no compiler changes):

- The six-cell matrix of (scope × created-on-first-touch /
  cached-thereafter / cleared-on-session-expiry):
  - app: created on first dep call, cached for server lifetime
  - session: created on first dep call within a session, cached per
    session id, cleared when the session expires
  - request: created on first dep call within a request, cleared at
    response end
- "Compile-time enforcement of dependency surface" tests showing that
  an `end_point` with `seal except req, res, dep` literally cannot
  reach an unfactored global (these reuse the existing
  `tests/integration/hide_seal_directives/` reject-test pattern
  introduced with the `hide` / `seal except` PR).
- Cycle detection: `factory A` calls `dep(ctx, "B")`, `factory B` calls
  `dep(ctx, "A")`, resolution should fail at first attempt rather than
  stack-overflow.
- `validate(srv)` startup pass: a server with a missing factory should
  fail to start with a clear error pointing at the factory closure that
  asked for the missing name, not silently succeed and fail at first
  request.
- Closure-captured config: a factory that reads a `let config = …`
  defined before it should still resolve `config` at factory-call time
  even though many requests have happened in between.
