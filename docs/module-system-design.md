# Aether Module System

This document describes Aether's module and namespace system.

## Terminology

| Term | Definition | Example |
|------|------------|---------|
| **Package** | A collection of related modules | `std` (standard library) |
| **Module** | A single importable unit with functions | `std.string`, `std.http` |
| **Namespace** | The prefix used when calling functions | `string`, `http` |

## Current Implementation

The standard library uses **namespace-style function calls**:

```aether
import std.string    // Import the string module
import std.file      // Import the file module

main() {
    // Namespace = module name after last dot
    s = string.new("hello")     // string namespace
    len = string.length(s)
    string.release(s)

    if (file.exists("data.txt") == 1) {   // file namespace
        size = file.size("data.txt")
    }
}
```

### How It Works

1. `import std.X` registers `X` as a namespace
2. `X.func()` is resolved to the C function `X_func()`
3. Dot-style calls: `string.new()`, `list.free()`, etc. (compiler translates to C-level `string_new`, `list_free`)

### Module Orchestration Phase

Before type checking, the compiler runs a module orchestration phase
(added between parsing and type checking in the compilation pipeline):

1. **Scan**: Walk the main program AST for `import` statements
2. **Resolve**: Map each import to a file path (stdlib, lib/, src/ paths)
3. **Parse**: Lex and parse each module file into an AST
4. **Cache**: Store the parsed AST in `global_module_registry`
5. **Recurse**: Process each module's own imports (transitive dependencies)
6. **Cycle Check**: Build a dependency graph and detect circular imports via DFS

Both the type checker and code generator then use `module_find()` to retrieve
cached ASTs, each module file is read and parsed exactly once.

### Available Modules

| Import | Namespace | Functions |
|--------|-----------|-----------|
| `import std.string` | `string` | `string.new()`, `string.length()`, `string.release()` |
| `import std.file` | `file` | `file.open()`, `file.read()`, `file.write()`, `file.exists()`, `file.close()`, `file.delete()`, `file.size()` |
| `import std.dir` | `dir` | `dir.exists()`, `dir.create()`, `dir.delete()`, `dir.list()` |
| `import std.path` | `path` | `path.join()`, `path.dirname()`, `path.basename()`, `path.extension()`, `path.is_absolute()` |
| `import std.fs` | `fs` | Monolithic file/dir/path module, call under the `fs` namespace, e.g. `fs.read()`, `fs.exists()`, `fs.list_dir()`, `fs.clean()` (not `file.*`/`dir.*`/`path.*`) |
| `import std.json` | `json` | `json.parse()`, `json.create_object()`, `json.free()` |
| `import std.http` | `http` | `http.get()`, `http.server_create()`, `http.server_start()` |
| `import std.tcp` | `tcp` | `tcp.connect()`, `tcp.write()`, `tcp.listen()` |
| `import std.net` | `net` | Monolithic networking module under the `net` namespace; redeclares the same TCP/HTTP externs over shared C symbols. Does not re-export the `tcp`/`http` namespaces, so `http.get()` fails after `import std.net` import `std.http`/`std.tcp` for those |
| `import std.list` | `list` | `list.new()`, `list.add()`, `list.get()`, `list.set()`, `list.remove()` |
| `import std.map` | `map` | `map.new()`, `map.put()`, `map.get()`, `map.has()`, `map.remove()` |
| `import std.collections` | `collections` | Monolithic list/map module. Functions keep their raw names; the qualified `collections.X` surface only reaches the Aether wrappers (`collections.list_add()`, `collections.map_put()`, `collections.map_get()`). Raw externs like `list_new`/`map_new` register globally and are callable unqualified, not as `collections.list_new()`. For a namespaced `list.new()`/`map.new()` surface, import `std.list`/`std.map` |
| `import std.math` | `math` | `math.abs_int()`, `math.sqrt()`, `math.sin()`, `math.random_int()` |
| `import std.log` | `log` | `log.init()`, `log.write()`, `log.shutdown()` |
| `import std.io` | `io` | `io.print()`, `io.read_file()`, `io.getenv()` |
| `import std.os` | `os` | `os.system()`, `os.exec()`, `os.getenv()` |

---

## Export Visibility

Modules declare their public API via a single Erlang-style `exports (…)`
list at the top of the file. Names in the list are accessible from
importers; names not in the list are private, still usable inside the
module's own functions, but rejected at qualified-call sites.

```aether,fragment
// lib/geometry/module.ae

exports (PI, distance)

const PI = 3

distance(x1, y1, x2, y2) {
    dx = x1 - x2
    dy = y1 - y2
    return _sqrt_approx(dx * dx + dy * dy)
}

// Private helper, not in the exports list, not accessible from outside.
_sqrt_approx(n) {
    return n  // placeholder
}
```

```aether,fails
import geometry

main() {
    d = geometry.distance(0, 0, 3, 4)   // OK, listed
    println(geometry.PI)                 // OK, listed
    // geometry._sqrt_approx(25)         // Error: not in exports list
}
```

**Rules:**
- `exports (…)` accepts any combination of function names, const names,
  and struct/actor names. The list is the contract.
- A module that declares an `exports (…)` list exposes **only** the
  listed names. Anything else is private.
- A module with no `exports (…)` list keeps the legacy default-public
  behavior, every top-level name is callable from outside. v1 is
  additive; v2 will flip this default to private once every stdlib +
  contrib module has been migrated.
- Private names can still be referenced from inside the module's own
  functions, they're merged into the program, just blocked at the
  external call boundary.

### Legacy `export <fn>` form (deprecated)

Earlier versions of Aether used a per-function `export` keyword:

```aether,fragment
// DEPRECATED, emits a warning at compile time.
export const PI = 3
export distance(...) { … }
```

This form is still accepted for one release as a deprecation alias.
Modules using it get a one-shot warning to stderr. Migrate by
collecting every `export`-tagged name into one `exports (…)` line at
the top, then removing the per-function keywords. Mixing both forms in
one module is a hard error.

## Per-symbol import aliasing

An entry in a selective-import list can be renamed with `as`, the same
token module-level aliasing already uses:

```aether,fragment
import vg (rect, fill, path as vgpath, text_paths)
```

`vgpath` now binds `vg`'s exported `path`, and the bare name `path` is
free for a local function, parameter, or another import. This is the
standard resolution when exactly one name in an otherwise-convenient
selective import collides: keep the other names bare, rename the one
that clashes, instead of qualifying every call to rescue a single
symbol.

```aether,fragment
import vg (rect, path as vgpath)

path(x: int) -> int { … }        // the local `path` is unaffected

main() {
    rect(0.0, 0.0, 460.0, 90.0)  // bare, as before
    vgpath(d)                    // vg's path, under the alias
    path(9)                      // the local one
}
```

Aliases work for constants as well as functions (`import cfg (LIMIT as
CAP)`), inside a module's own imports (the alias resolves when that
module's bodies are merged into a consumer), and alongside module-level
aliasing. The shadow rejection below applies to the **alias**, the name
the program actually binds: `import vg (path as vgpath)` plus a local
`vgpath` is the collision, while a local `path` is now fine.

## Selective-import shadow rejection

A module (or main program) that selectively imports a name AND defines a local function with the same name silently shadowed the import:

```aether,fragment
import std.string (length)

length(s: string) -> int {
    return length(s)        // intent: std.string.length
                            // reality: self → infinite recursion
}
```

Pre-fix, the merger renamed the body's bare `length(s)` to `mod_length(s)` (matching the local def's namespaced name post-merge), turning the wrapper into self-recursion, a runtime stack overflow with no compile-time signal. The orchestrator rejects this pattern with `error[E1000]` before merging:

```
error[E1000]: module 'mod' (mod/module.ae) defines local function
'length' (3:1) but also selectively imports 'length' from 'std.string'
  the local def silently shadows the import, so a bare call to
  'length(...)' inside the local body would recurse into the local
  rather than forward to the imported symbol, at runtime this is a
  stack overflow with no compile-time signal.
  fix one of:
    - rename the local function
    - drop the selective import: `import std.string` (then call via
      the qualified form)
    - keep both but call the imported version qualified inside the
      local body
```

The same check applies to entry-point `main.ae` files when the shadow lives there directly (not inside a module). Both `AST_FUNCTION_DEFINITION` and `export <fn>` shapes are caught.

## Qualified surface survives a selective import after merge

The two `import std.X` forms each light up a different call syntax: a bare
`import std.X` enables the qualified `X.fn()` surface; a selective
`import std.X (fn)` enables the bare-name `fn()` surface. These compose
per-file, but a merged compilation unit pulls definitions from several files
into one program, and a module's own `import` statements are *not* cloned in
(only its function/const/struct bodies are).

That dropped the qualified surface in one case: when the entry file imported a
module **selectively** (`import std.string (string_length)`), the merged unit
was marked selective for `string`, so a qualified `string.concat(...)` arriving
from an imported module that bare-imports `std.string` was rejected with
`error[E0301]: Undefined function 'string.concat'`.

The merge now injects a **synthetic bare import** for each merged module's own
bare imports. A synthetic import marks the namespace non-selective for the unit
(re-opening `string.X`) but is kept out of the user-explicit registry, so
sealed-scope isolation is preserved: user code still cannot call into a
namespace it never imported. This mirrors how transitive dependencies already
get synthetic imports during merge.

## Re-exports

A module can re-export a symbol it imports from another module by listing that
symbol in its own `exports`. A consumer then reaches it through the re-exporting
module even though that module never defined it:

```aether,fragment
// hub.ae, re-exports DERIVED from an inner module
import layout_consts (DERIVED)
exports (DERIVED)

// consumer.ae
import hub
x = hub.DERIVED        // resolves to the definition in layout_consts
```

Re-export is opt-in through the `exports` list, transitive (a hub may
re-export a symbol another hub re-exports), and cycle-guarded. It is also
precise: a locally-defined export is never treated as a re-export, so if a
module both defines a name and imports one, the local definition wins. The
resolver redirects an unresolved `hub.X` to the origin module that defines it,
and the merge pass pulls that origin definition into the program under the
origin's namespace.

## Cross-module method-call syntax (UFCS)

A method-style call `x.f(args)` is rewritten to the free-function call
`f(x, args)` when no field or method named `f` applies. The target `f` may be
defined in an imported module, not only in the same file. Same-file functions
take precedence, and the rewrite is a last resort, it fires only when the call
does not already resolve as a normal function or a struct field, so it never
shadows an existing binding. This is what lets a fluent chain such as
`expect(x).to_equal(y)` span module boundaries.

## Circular-import diagnostics

Imports are resolved through a dependency graph, and a cycle is a hard error.
The diagnostic names the actual cycle as a path, `a -> b -> a` rather than
the entry point, so the report points at the modules that actually form the
loop. The synthetic `__main__` entry node never appears in the path because
nothing imports it.

## Future

Work planned on top of the current module system:

- `import math.geometry as geo` import aliases. The parser accepts them and the typechecker records the alias via `add_module_alias`, but qualified calls through the alias (`geo.distance()`) do not yet resolve; completing that lookup is the remaining work.
- Exporting structs and actors from modules.

---

## Creating Packages

Developers can create their own packages using the `ae` CLI tool.

### Quick Start

```bash
# Create a new package
ae init mypackage
cd mypackage

# Build and run
ae build
ae run
```

### Package Structure

```
mypackage/
├── aether.toml       # Package manifest
├── src/
│   └── main.ae       # Main entry point
├── lib/              # Library modules (optional)
│   └── utils/
│       └── module.ae # import utils → lib/utils/module.ae
├── tests/            # Test files (optional)
│   └── test_utils.ae
└── README.md
```

### Package Manifest (aether.toml)

Every package has an `aether.toml` file. This is what `ae init mypackage`
scaffolds:

```toml
[package]
name = "mypackage"
version = "0.1.0"
description = "A new Aether project"
license = "MIT"

[[bin]]
name = "mypackage"
path = "src/main.ae"

[dependencies]

[build]
target = "native"
# link_flags = "-lsqlite3 -lcurl"  # Add extra linker flags
```

Only a subset of the manifest is honored today: the `[[bin]]` entries
(`name`/`path`/`extra_sources`) and the `[build]` `target`, `link_flags`,
and `cflags` keys. The `[package]` metadata and `[dependencies]` table are
recorded but not yet acted on by the toolchain.

### Creating Local Modules

Aether supports local modules within your project:

**Project Structure:**
```
myapp/
├── aether.toml
├── src/
│   └── main.ae              # Main entry point
├── lib/
│   └── utils/
│       └── module.ae        # Pure Aether module
└── tests/
```

**lib/utils/module.ae**, define your module:
```aether,fragment
export const MULTIPLIER = 2

export double_value(x) {
    return multiply(x, MULTIPLIER)
}

export triple_value(x) {
    return multiply(x, 3)
}

// Private helper
multiply(a, b) {
    return a * b
}
```

**src/main.ae**, use the module:
```aether,fragment
import utils

main() {
    println(utils.double_value(5))   // 10
    println(utils.triple_value(5))   // 15
    println(utils.MULTIPLIER)        // 2
}
```

### Module Resolution

The compiler searches for local modules in this order:

1. `<lib-path-entry>/<module>/module.ae` - Library directory with module file
2. `<lib-path-entry>/<module>.ae` - Single-file module in lib
3. `src/<module>/module.ae` - Source directory with module file
4. `src/<module>.ae` - Single-file module in src
5. `<module>/module.ae` - Project root
6. `<module>.ae` - Single file in project root

For nested modules like `import mypackage.utils`:
- Dots are converted to slashes: `mypackage/utils/module.ae`
- Same search paths are used with the converted path

7. `~/.aether/packages/<name>/src/module.ae` - Installed package (via `ae add`)
8. `~/.aether/packages/<name>/module.ae` - Installed package (flat layout)

Packages installed with `ae add` are searched after local paths. The package name is the first component of the import path.

#### Lib search path

`<lib-path-entry>` in steps 1 and 2 is a list of directories, PATH-style, `:` on POSIX and `;` on Windows. The list is searched left-to-right; the first hit wins. Three forms compose:

```sh
ae run main.ae --lib ./lib:./vendor/stdlib    # separator-string
ae run main.ae --lib ./lib --lib ./vendor     # repeated flag
AETHER_LIB_DIR=./lib:./vendor ae run main.ae  # env var
```

Both forms can be mixed (`--lib a:b --lib c` → `[a, b, c]`). The default is the single entry `lib` (the legacy behaviour); supplying any explicit `--lib` or `AETHER_LIB_DIR` replaces the default with the given list. Up to 8 entries. Caching invalidates on lib-path changes, two builds of the same source with different `--lib` chains get distinct cache slots, and every entry's mtime feeds into the cache key so a `touch` in a vendored module reuses the right cache bucket.

The shape matches Java `-cp`, Python `PYTHONPATH`, Ruby `-I` / `RUBYLIB`. Useful for layering project-local modules over a vendored stdlib, sharing a common lib root across two projects, or pinning a stdlib snapshot alongside HEAD-tracking deps.

Trailing slashes are normalised: `--lib ./lib/` and `--lib ./lib` register the same entry (the dedup catches them), and lookup paths stay clean, `<entry>/<module>.ae` rather than `./lib//<module>.ae`. Root paths (`/` on POSIX, `C:\` on Windows) are preserved as-is.

#### Introspecting the resolved chain

`ae lib-path` prints the resolved search order, one entry per line, in the same order the toolchain walks them. Useful when debugging "why isn't my import resolving?" without re-reading the user's shell config:

```sh
$ ae lib-path --lib ./lib:./vendor/stdlib
./lib
./vendor/stdlib

$ AETHER_LIB_DIR=/usr/share/aether:./local ae lib-path
/usr/share/aether
./local

$ ae lib-path                          # defaults, single entry
lib
```

Inputs are merged in the same priority order the build path uses: explicit `--lib` flags first, then `AETHER_LIB_DIR`, then the default `lib`.

#### Dependencies resolve onto the search path

`ae add` installs into `~/.aether/packages/<host>/<owner>/<name>` and records the dependency:

```toml
[dependencies]
"github.com/aether-lang-dev/selaenium" = "0.2.0"
```

`ae run`, `ae build` and `ae lib-path` read that section and join the package's modules to the search path. **No `--lib` is needed to import from a declared dependency.**

The publishing package decides what it exports, in its own `aether.toml`:

```toml
[package]
name = "selaenium"
modules = "aether, selenium_core, selenium_core/drivermgr"
```

Each entry names an **importable module**, and what joins the search path is that module's parent directory — because `--lib D` means "D contains modules" (`D/<name>.ae` or `D/<name>/module.ae`). So `selenium_core/drivermgr` puts `<package>/selenium_core` on the path and `import drivermgr` resolves.

This is why the declaration lives with the publisher rather than the consumer: a package can move its directories in a patch release without any consumer changing a path. A consumer names the dependency and nothing else. A package with no `aether.toml`, or none with a `modules` key, exports nothing and says so — the package root is never joined speculatively, since a real package is a whole repository whose root holds docs, scripts and tests as well as modules.

A dependency that is declared but not installed fails by name:

```
Error: dependency 'github.com/aether-lang-dev/selaenium' is not installed. Run:
    ae add github.com/aether-lang-dev/selaenium
```

rather than as an unresolved-import error naming a module the user never typed.

#### Overriding a dependency

Two forms, both of which **announce themselves** — an override that applied silently would mean a green local build against a working copy that CI does not have:

```sh
ae run x.ae --override github.com/aether-lang-dev/selaenium=../selaenium
```

per-invocation, leaving no trace in the manifest — right for "just this once" and for CI proving a change against an unpublished branch. And in the manifest, a separate stanza so an override cannot be shipped by accident inside a `[dependencies]` line:

```toml
[patch]
"github.com/aether-lang-dev/selaenium" = "../selaenium"
# or Cargo's table form, which means the same thing:
"github.com/aether-lang-dev/other" = { path = "../other" }
```

Only a local path override is supported. A table naming anything else (a git URL, say) is an error rather than a silently-ignored line.

Either way the build prints `Overriding <dep> -> <path>`. A `--override` for a dependency also overridden by `[patch]` wins, since the invocation is the more specific instruction. The override target is read exactly like an installed package: it needs its own `[package] modules` declaration.

### Namespace Convention

Function names must be prefixed with the namespace:
- Import: `import utils`
- Call: `utils.double_value(x)`
- C function: `utils_double_value()`

The compiler converts `namespace.function()` to `namespace_function()`.

### Pure Aether Modules

You can write reusable modules in pure Aether, no C backing file required:

**lib/mymath/module.ae:**
```aether,fragment
export const PI_APPROX = 3

export double_it(x) {
    return x * 2
}

export add(a, b) {
    return a + b
}

// Intra-module calls work, functions can call each other
export double_and_add(x, y) {
    return add(double_it(x), y)
}
```

**src/main.ae:**
```aether,fragment
import mymath

main() {
    println(mymath.double_it(5))    // 10
    println(mymath.add(3, 4))       // 7
    println(mymath.double_and_add(5, 3))  // 13
    println(mymath.PI_APPROX)       // 3
}
```

**How it works:**

After module orchestration, the compiler clones each module's function and constant AST nodes into the main program with namespace-prefixed names (`double_it` → `mymath_double_it`). Intra-module calls, constant references, and constant-to-constant references (e.g., `const DOUBLE_BASE = BASE * 2`) are renamed automatically. Function parameters and local variables correctly shadow module constants, `check(SCALE) { return SCALE }` returns the parameter, not the module constant `SCALE`. This makes the entire downstream pipeline (type inference, type checking, codegen) work without modification, merged functions are just regular top-level functions.

**Tree-shake of unused merges.** Immediately after merging and before typechecking, `module_prune_unreachable` runs a mark-and-sweep over the program AST. It seeds reachability from `main`, every actor handler, every `export` statement, and every non-imported user function/builder, then closes over `AST_FUNCTION_CALL`, `AST_IDENTIFIER`, and `AST_MEMBER_ACCESS` references, including suffix matches that handle glob-import (`import mymath (*)`) and selective-import (`import mymath [cube]`) shorthands. Imported function and builder definitions outside the closure are dropped from the AST so the typechecker doesn't walk them and the C compiler doesn't emit them. Constants stay (cheap, and pruning them would need a separate pass keyed on identifier references). The whole pass is invisible to user code; programs that *do* call every imported function build identically before and after.

**Native link dependencies (`@link`).** A module that wraps a native
library declares its own link flags at the top of `module.ae`:

```aether,fragment
@link("-laether_sqlite -lsqlite3")
```

When the module is in a program's resolved import closure, codegen unions
every declared flag (first-seen order, deduplicated) into the
`// aether-link:` comment on the first line of the generated C, which
downstream C builds read to link without undefined-reference archaeology.
Declaring is enough; there is no central registry to edit. Token order
within one `@link` is preserved, which matters for single-pass linkers
(veneer archive before the library it references).

`ae build` reads that comment back and puts the tokens on its own link
command (#1549), so a consumer does not restate them in `aether.toml`. They
are placed *before* `link_flags`, which therefore still wins. `-L` search
paths remain the consumer's job — those are site-specific in a way a module
cannot know — except for `<lib_dir>/contrib`, which is added automatically so
the veneer archives `make contrib` builds resolve without configuration.

**The link line is derived from the AST, which makes it transitive and
`-D`-sensitive.** Both properties matter and neither is obvious:

```
-D NAME → `when` region kept or dropped → import in or out of the closure
        → @link visible or not → // aether-link: → the link command
```

Transitive: a program that never names `contrib.sqlite` still links it when
something it imports does, at any depth. `-D`-sensitive: an `import` inside a
losing `when defined(...)` region is gone before codegen, so its `@link`
contributes nothing and the library is absent from the binary entirely — not
merely uncalled. That is the half a static `link_flags` cannot express, since
it is passed on every build regardless of which regions survived.

**What's supported:**
- Functions (with type inference from call sites)
- Constants (`const NAME = value`), including constants referencing other constants
- Intra-module calls (functions calling other functions and referencing constants in the same module)
- Export visibility (`export` keyword controls public API)
- Multiple module imports in the same program
- Mixing pure modules with stdlib imports
- Parameter/local variable shadowing of module constants
- Actors and their message types: `spawn` an `actor` and send a `message` declared in an imported module. Both enter the program under their bare name (like structs), so `spawn(Worker())` and `w ! Ping { ... }` resolve; the actor's handlers still have their intra-module function/constant references rewritten, and the per-program message registry assigns runtime type ids across the merge.

**On the module-system roadmap (language-level):**
- Module-level mutable state

### Roadmap

1. **Done**: Package initialization (`ae init`)
2. **Done**: Local package building (`ae build`)
3. **Done**: Local nested package imports
4. **Done**: Pure Aether module implementations
5. **Done**: Export visibility enforcement
6. **Planned**: Package dependencies
7. **Planned**: Package registry/publishing

## Notes

- Single-file programs continue to work without packages
- The `std` package is built-in and always available
