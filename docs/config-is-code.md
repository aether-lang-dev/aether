# Config IS Code

A design pattern, and a recommendation for Aether libraries that have a
"start the thing" surface, HTTP servers, daemons, agents, schedulers,
fixtures, test rigs. The short version: don't ship a YAML loader.
Expose your library's start surface as a closure-DSL block, and let the
operator's "config" be a `.ae` file they run with `ae run`.

This document is the *why*. For *how*, the syntax, the `builder`
keyword, the trailing-block context, see
[`closures-and-builder-dsl.md`](closures-and-builder-dsl.md).

## The progression

Most server-shaped projects walk through these stages, in order, until
they get tired of it. Aether lets you skip to the end.

### Level 1, Config files (YAML/JSON/TOML)

```yaml
# my-server.yaml
listen:
  host: 127.0.0.1
  port: 9990
auth:
  superuser_token: ${env:SUPER_TOKEN}
repos:
  - name: alpha
    path: /srv/alpha
  - name: beta
    path: /srv/beta
```

The binary parses this at startup. Pros: anyone can edit it; tools can
diff it; YAML is "just data."

The catch: every one of these grows a half-baked DSL anyway. First
people want environment-variable interpolation (`${env:...}`). Then
conditional sections (`if env == prod`). Then include directives
(`!include common.yaml`). Then computed defaults. Then templating
(Helm, Kustomize, Jinja). At each step you've reinvented a worse copy
of the host language, and operators are now learning a YAML-flavoured
mini-language that exists only inside your binary.

Apache `httpd.conf`, Nginx config, Postgres `postgresql.conf`,
Kubernetes manifests, and Helm charts are all at various stations on
this slide.

### Level 2, Config as code (HCL, Helm templates, Kustomize)

```hcl
# my-server.hcl
listen {
  host = "127.0.0.1"
  port = 9990
}

repo "alpha" { path = "/srv/alpha" }
repo "beta"  { path = "/srv/beta" }
```

Better: typed values, blocks, expressions, sometimes loops and
conditionals. Terraform, HCL, Helm-with-templating sit here.

The catch: it's a **second language**, with a separate parser, a
separate type system, a separate set of tools. You debug it
separately. Its expressivity is bounded by what the DSL author chose
to expose. The first time you need something the DSL doesn't support
a per-tenant lookup against an external API at config time, a
computed cache key, a quick `if hostname matches X` you reach for
shell or templating around the DSL, and you're back at Level 1's
problems.

### Level 3, Config IS code

```python
# pulumi-style: your config is a real program in a real language
import pulumi
import my_server

server = my_server.Server(
    listen=my_server.Listen(host="127.0.0.1", port=9990),
    repos=[
        my_server.Repo("alpha", "/srv/alpha"),
        my_server.Repo("beta",  "/srv/beta"),
    ],
)
```

Now config IS a program. Loops, conditionals, library calls, custom
helpers, all available, in the same language as the implementation.
AWS CDK, Pulumi, Bazel rule files, and most "infrastructure as code"
tooling lives here.

The catch: the **declarative readability is gone**. The user is
constructing objects, calling constructors, threading parameters. It
reads like glue code, not like config. Operators bounce off, they
wanted a config file, they got a program with import statements and
type annotations.

### Level 4, Closure-DSL: config IS code, *and reads like config*

```aether,fragment
import my_server (serve, host, port, superuser_token, repo)
import std.os (os_getenv)

main() {
    serve {
        host("127.0.0.1")
        port(9990)
        if os_getenv("STAGE") == "prod" {
            host("0.0.0.0")
        }
        superuser_token(os_getenv("SUPER_TOKEN"))
        repo("alpha", "/srv/alpha")
        repo("beta",  "/srv/beta")
    }
}
```

The Aether `builder` keyword (see
[`closures-and-builder-dsl.md`](closures-and-builder-dsl.md)) gives
you the *declarative aesthetic* of Level 1, bare setter calls, no
explicit object construction, no parameter threading, on top of
*full code* underneath: control flow, library calls, env lookups,
arbitrary Aether. The library author absorbs the wiring; the operator
writes what looks like a config file but isn't.

This is the **one step beyond config-as-code** worth naming
explicitly. Levels 1 and 2 give you readability without power. Level
3 gives you power without readability. Level 4 gives you both.

## What this buys you

When config is just an `.ae`:

- **Loops over inputs that aren't fixed at deploy time.** Read a list
  of repos from a manifest file, iterate, call `repo(...)` for each.
  No schema work, no JSON-Pointer foreach extension.
- **Environment-aware branching.** `if os_getenv("STAGE") == "prod" { ... }`
  is just an `if`. No `${env:...}` mini-language.
- **Pre-flight setup in the same artifact.** Mount checks, key
  fetches, `mkdir -p`, schema migrations all run in the same process,
  before `serve { ... }` blocks.
- **One artifact in git.** The deployment IS the config. No drift
  between code and YAML.
- **Same toolchain.** `ae run`, `aetherc`, the type checker, the LSP,
  and the test runner all operate on your config directly. Your YAML
  linter doesn't.

## When this is right

- The operator and the developer overlap. Self-hosted dev tooling,
  CI runners, internal services, fixtures, test harnesses.
- The library is "start the thing"-shaped: bind, configure, run.
- The number of config knobs is small enough that exhaustive YAML
  schemas would be overkill.

## When YAML still wins

- **Multi-tenant ops where operators don't write Aether.** A managed
  service with hundreds of customer-facing config files needs a
  schema'd, language-agnostic surface.
- **GitOps pipelines with external tooling.** Argo CD, Flux,
  Kustomize all expect declarative manifests.
- **Schemas that other tools consume.** Kubernetes resources, OpenAPI
  specs, JSON Schema validators.

For these, build a thin YAML layer that constructs and calls the same
opts struct your closure-DSL populates. The inner workhorse stays
shared; only the outer entry point differs.

## Library author recipe

To expose your library's start surface as a closure-DSL, follow the
standard three-layer split:

```aether,fragment
// 1. Opts construction, explicit API. The base everything else
//    funnels into.
export opts_new() -> ptr {
    return map.new()
}

// 2. Setters, take `_ctx: ptr` first, write into the map. Each
//    setter is one knob.
export host(_ctx: ptr, addr: string) {
    _e = map.put(_ctx, "host", addr)
}

export port(_ctx: ptr, n: int) {
    _e = map.put(_ctx, "port", string.from_int(n))
}

// (Repeatable setter, collect into a list.)
export repo(_ctx: ptr, name: string, path: string) {
    if map.has(_ctx, "repos") == 0 {
        _e = map.put(_ctx, "repos", list.new())
    }
    repos, _ = map.get(_ctx, "repos")
    list.add(repos, "${name}\t${path}")
}

// 3. Workhorse, reads opts, does the work. Single source of truth.
export serve_opts(opts: ptr) -> int {
    // ... read keys, register, bind, listen, run ...
}

// 4. Closure-DSL entry, `builder` keyword wires the trailing block
//    to populate `_builder`, then hands it to the workhorse.
builder serve(_ctx: ptr) -> int {
    return serve_opts(_builder)
}
```

The user's three options stay 1:1 equivalent:

```aether,fragment
// (a) Closure-DSL block, the recommended surface.
my_lib.serve {
    host("127.0.0.1")
    port(9990)
}

// (b) Explicit opts API, for callers who want to construct opts
//     once and reuse, or build them programmatically from another
//     source.
opts = my_lib.opts_new()
my_lib.host(opts, "127.0.0.1")
my_lib.port(opts, 9990)
my_lib.serve_opts(opts)

// (c) CLI front-end (your own main.ae), argv → setters →
//     serve_opts. Same workhorse; argv just becomes another
//     opts-population strategy alongside the closure-DSL.
```

Adding a knob is one new setter + one new key read in the
workhorse. The CLI, the closure-DSL, and the explicit API all gain
the knob in lockstep, no risk of drift between "what the YAML
supports" and "what the binary can do."

## Pitfalls

A few things to watch for when designing setters:

- **Avoid libc symbol names.** A setter named `bind` will silently
  shadow libc's `bind(2)` at link time, and your listening socket
  won't attach. Same for `listen`, `accept`, `connect`, `open`,
  `read`, `write`, `socket`, `select`. Pick `host`, `addr`,
  `subscribe`, `attach`, etc.
- **Name setters for what they configure, not for the underlying
  call.** `port(n)` reads better than `set_port(n)`; `host(addr)`
  reads better than `bind_to(addr)`. The closure-DSL aesthetic
  depends on this.
- **Repeatable knobs collect into a list, not a map.** `repo("a",
  "/x"); repo("b", "/y")` should accumulate, not overwrite. Use a
  list under a stable key.
- **Setters write, the workhorse reads.** Don't do work inside
  setters. The order of calls in the block shouldn't matter
  (operators reorder for clarity); only the final state of the opts
  map should drive behaviour.
- **Default in the workhorse, not the setter.** A setter that wasn't
  called shouldn't have planted a value. Read with `map.has` +
  fallback; that way every default lives in one place.

## Inspecting a script, `ae inspect`

Config-IS-code only lands with operators if they can ask the compiler
*"what does my script actually declare?"* without reading generated C.
`ae inspect <file.ae>` prints that summary:

```
$ ae inspect deploy.ae
inspect: deploy.ae
  artifact:  executable (entry: main)
  capabilities required (gated imports): fs net
             (pass --with=<cap,...> to grant under --emit=lib)
  imports (3):
    std.string                   [stdlib]
    std.fs                       [stdlib, capability: fs]
    std.http                     [stdlib, capability: net]
  functions (2):
    greet(name: string) -> string
    divmod(a: int, b: int) -> (int, int)
  structs (1): Config
  constants (1): DEFAULT_PORT
```

It reports only what *this* file declares (imports resolved and
classified as stdlib / contrib / local, with the `--with=` capability any
gated module needs; the artifact shape, executable entry point vs
library export surface; and the top-level functions, structs, constants,
and actors with signatures), declarations merged in from imported
modules are excluded. No code is generated. For a library
(`exports (...)`) it lists the exported ABI surface instead of an entry
point. Out of scope: full type dumps (that's the FFI `aether_describe`
metadata) and raw AST printing (that's `aetherc --emit=ast`).

## Cross-references

- [`closures-and-builder-dsl.md`](closures-and-builder-dsl.md), the
  reference for `builder`, trailing blocks, and context injection.
- [`aether-embedded-in-host-applications.md`](aether-embedded-in-host-applications.md)
  when `ae run` isn't the deployment vehicle and you're embedding
  the script inside another binary.
- [`aether-dsl-as-a-rules-engine.md`](design/aether-dsl-as-a-rules-engine.md)
  the same closure-DSL machinery applied to business rules rather
  than start-the-thing config.
