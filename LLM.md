# Orientation for an LLM working on Aether

A dense, opinionated map of the parts of this repo that aren't obvious from
a first read: what the language is, the idioms that trip up a first port,
and the build/test wiring. Skim it before starting, and prefer the code
(`std/<m>/module.ae`, `compiler/`, the CHANGELOG) as the source of truth
whenever this file and the tree disagree.

## What Aether is, in one paragraph

Systems language. Compiles to C. Two emit modes: `--emit=exe` (default,
produces a binary with a `main()`) and `--emit=lib` (produces `.so`/`.dylib`
with ABI-stable `aether_<name>` exports for FFI from C / Python / Ruby / Java
via ctypes/SWIG/Panama). `--emit=lib` is capability-empty by default,
`std.fs` / `std.net` / `std.os` imports are rejected. The opt-in is
`--with=fs[,net,os]`, an explicit per-build flag for projects that ARE
the host and want full syscall access (e.g. implementing a systems tool
in Aether + a thin C driver).

## How to anchor Aether against languages you already know

Think of it as: **Go's ergonomics + Rust's capability discipline +
Erlang's actor syntax, compiling via C.**

- **NOT Rust**, no borrow checker, no ownership, no lifetimes. Strings
  are ref-counted or arena-owned; you release explicitly where it
  matters, drop-on-scope-exit elsewhere.
- **NOT Go**, no GC. Memory is manual at the runtime C level; Aether
  code mostly avoids the issue via RAII-via-codegen.
- **NOT C**, has closures, tuples, Go-style `(value, err)` returns,
  string interpolation, pattern matching.
- **Actor model is Erlang-ish** but message types are declared, not
  duck-typed: `message Foo { f: T }` declares each type; the mailbox
  `receive` matches on those types. Send IS `actor ! Foo { f: v }`
  (Erlang-style bang); the typed-message declaration is what differs.
- **Sandboxing is built in.** Three layers: `--emit=lib` + `--with=`
  gates stdlib imports at compile time; `hide` / `seal except` denies
  enclosing names per lexical block; `libaether_sandbox.so` (LD_PRELOAD)
  checks libc calls against a builder-DSL grant list. A mashup of Pony
  object capabilities, Java's removed SecurityManager, and a fraction of
  gVisor.
- **Hosts other languages in-process.** Aether `main()` embeds
  Lua/Python/Perl/Ruby/Tcl/JS via
  `contrib.host.<lang>.run_sandboxed(perms, code)`;
  Java/Go/aether-hosts-aether are separate-process. The same grant list
  + LD_PRELOAD gates the hosted interpreter's libc calls. Guest
  direction: `--emit=lib` → Python ctypes / Java Panama / Ruby
  Fiddle SDKs auto-generated from `aether_describe()`.
- **NOT quite Ruby/Smalltalk/Groovy's builder-style closures**
  those all run on a VM (MRI, the Smalltalk image, the JVM)
  where the block has runtime reflection access to its surrounding
  scope, regardless of whether it was reached by interpretation
  (Ruby, classic Smalltalk) or bytecode compilation (Groovy since
  ~2012, modern Ruby JIT). Aether is native: closures lower to plain
  C functions with no VM behind them, so there's no run-time hook
  for the block to reach back through. `hide` / `seal except` are
  compile-time, and the grant list is the closure's only handle to
  privileged operations. The trailing-block + `builder` shape is what
  lets a call site read like a config file (bare setters, no constructor
  noise, no parameter threading) while the body is still ordinary Aether:
  control flow, env lookups, library calls. YAML and HCL are declarative
  but can't compute; Pulumi/CDK can compute but read like glue code. The
  closure-DSL sits between them, which is what makes the next bullet
  possible.
- **NOT a YAML/HCL/Helm host** (related), and won't be. Building on
  the closure-DSL above, Aether's pitch for server-shaped libraries
  is **config IS code**: don't ship a YAML loader, expose your
  start-surface as a closure-DSL block and let the operator's
  "config" be a `.ae` file they run with `ae run`. Same file is
  config + validation + conditionals + entry point; no second
  parser, no template language, no embedded scripting hack.
  Sandboxing (`--emit=lib --with=...`, `hide`, `seal except`)
  keeps untrusted configs safe, the same machinery that makes the
  embedded-DSL case work. If a downstream user asks "should we add
  a YAML config loader?", point them at `docs/config-is-code.md`
  and the trailing-block DSL pattern.

Sandbox more info: Full comparison (who brings what, Pony's per-reference
  granularity, Java SecManager's policy-file ancestry, gVisor's
  syscall scope, plus WASI/Deno/Rust) lives in
  `docs/containment-sandbox.md` → *How Aether compares to other
  capability / sandbox systems*.

Syntax looks Go-ish at a glance (braces, `func_name(x: int) -> int`).
Don't overshoot, there's no `go` keyword, no channels (send-to-actor
plays that role), no interfaces.

## Files/dirs worth knowing

- `CHANGELOG.md` reverse-chron; the `[current]` section holds
  unreleased work (everything since the last tagged release). Read its
  top ~40 lines at session start for recent feature/fix context.
- `docs/emit-lib.md` the capability-opt-in doc. Canonical reference
  for why `std.fs` is banned under `--emit=lib` by default.
- `docs/config-is-code.md` the "don't ship a YAML loader" pitch.
  Library authors with a "start the thing" surface should expose
  it as a trailing-block DSL, not a config-file format. Pair with
  `docs/closures-and-builder-dsl.md` for the mechanism. `docs/cic-help.md`
  is the (implemented and shipped) `ae help <script>` companion that
  catches operator-side mistakes in those scripts, built into the `ae`
  driver via `tools/ae_help.c`, dispatched from `tools/ae.c`.
- `docs/next-steps.md` roadmap, in priority order. Check it before
  speccing a new stdlib addition, but treat its "done" ticks as
  authoritative over its priority labels, the `std.fs` completeness
  bundle (`fs.copy` / `fs.move` / `fs.chmod` / `fs.symlink` /
  `fs.realpath`) has shipped, for instance.
- `std/<module>/module.ae` the Aether-facing surface. Raw externs
  end in `_raw`; Go-style wrappers return `(value, err)` tuples.
- `std/<module>/aether_<module>.c` the C runtime behind the externs.
- `contrib/<module>/` opt-in modules NOT in `libaether.a`. Same
  shape as `std/*` (module.ae + aether_*.c + README), but consumers
  add `extra_sources` + `link_flags` per the contrib README rather
  than getting auto-link. **Always check `contrib/` before deciding
  a stdlib gap is real**, a porter writing a downstream port may
  not realise the module is there. Today's top-level dirs are
  `contrib/{cryptography, host, parsers, sqlite, templating, tinyweb}`:
  `parsers/xml_expat` (SAX XML via libexpat), `sqlite`,
  `templating/{liquid,native}`, and `host/<lang>` embedded interpreters
  for js, lua, perl, python, ruby, tcl, duktape, tinygo, go, java, factor,
  racket, rhombus, and aether-in-aether.
- `compiler/aetherc.c` CLI entry. `--emit=lib`, `--with=`, and the
  capability import gate (search for `--with=` argv parsing and the
  "Step 2.7: --emit=lib capability-empty check" enforcement block).
- `build/aetherc`, `build/ae` the compiled binaries. `make && make
  install` to rebuild. If `aetherc --help` lacks a feature the CHANGELOG
  `[current]` documents, the local binary is stale, rebuild.
- Downstream feature requests tend to arrive as concrete spec documents
  (exact API shape, call-site census, rationale) rather than vague asks;
  match that level when responding. Landed ones are folded into the
  CHANGELOG and the relevant `docs/` page rather than kept as loose
  planning files.
- `tests/regression/` one `.ae` file per feature; the CI gate.
  `tests/integration/` integration test directories (e.g.
  `emit_lib_with_capability/` for the `--with=` opt-in).

## Idioms that keep biting

- **String/int dispatch → `match`, not an `if` chain.** `match (mode) {
  "check" -> {…} "up" -> {…} _ -> {…} }`, string arms compare by content,
  int arms by value, `_` is the wildcard. Beats a chained `if mode == "…"`
  and a C-style `switch` (`case V:`, literals, no binding). `match` also
  captures/destructures lists (`[h|t]`); for range/relational branching use
  **guarded function clauses** (`grade(s) when s >= 90 -> "A"` the Erlang-
  style multi-clause form), and an `if`-expression for a value-producing
  two-way choice. `match`/`switch` arms are literal-only; guards are the
  escape to conditions. NB: this clause-level `when` (a runtime guard) is a
  *different* construct from the top-level/statement **compile-time `when`**
  (static-if: `when target.os == "windows" { … } else { … }`, only the
  selected arm is type-checked/emitted, see `docs/when-static-if.md`). Same
  keyword, two unrelated meanings.
- **Go-style returns, not tuples-as-values.** `fs.write_atomic(path,
  data, len) -> string`, empty string = success, non-empty = error
  message. Don't "improve" the convention, it's consistent across all
  of `std`.
- **Actor syntax.** `message Add { value: int }` declares a typed
  message; an `actor Name { state x = 0; receive { Add(value) -> { … } } }`
  holds per-actor `state` and matches messages by type in `receive`.
  `spawn(Name())` returns a handle; `handle ! Add { value: 10 }` sends
  (fire-and-forget, async). `state` fields are private to the actor, no
  shared globals, which is how Aether gets concurrency without locks.
- **`defer expr` runs at scope exit, LIFO.** Use it to pair an acquire
  with its release at the same site (`defer ref_free(x)`,
  `defer list.free(g)`) so every return path cleans up. Multiple defers
  in a scope fire last-registered-first.
- **`defer try` / `defer catch` run on ONE outcome only.** `defer catch f()`
  fires only when the function returns an error (a non-empty error slot — the
  `(value, err)` convention, and `T!`); `defer try f()` fires only on success; a
  plain `defer` still fires on both. That gives you the transaction shape
  (`p = malloc(...)`, `defer catch free(p)`, then bail freely — every early
  return releases `p`, and on success the caller owns it) instead of repeating
  `if err != "" { free(p); return }` at every exit and leaking at the one you
  forget. LIFO ordering is unchanged and the conditional ones interleave with the
  unconditional ones by registration order. Cost is one compare on the return
  path — zero where the outcome is static (an `expr!` propagation is always an
  error exit; a bare `return v` is always a success exit). Using either in a
  function that *can't* fail is a warning, not silence.
- **Closures are `|x: int| -> expr` or `|x| { … }`, invoked with
  `call(fn, args)`.** They capture enclosing variables by value and lower
  to plain C functions (no VM). Distinct from the **trailing-block /
  `builder`** call shape (`f(x) { … }`, see below), which is the DSL lever
  a closure value is data you pass to `call`, a trailing block is sugar
  attached to a call site.
- **Split-accessor pattern for multi-return.** Where a wrapper needs
  more than one return value and the language can't yet unify tuples
  cleanly across FFI, the pattern is a try/get pair backed by TLS:
  `fs_try_read_binary(path) -> int` writes into a TLS buffer,
  `fs_get_read_binary()` / `fs_get_read_binary_length()` / `fs_release_read_binary()`
  drain it. Mirrors the paths_index / checksum pattern elsewhere.
- **Strings are length-aware internally**, but `string_concat("", raw)`
  treats `raw` as C-string (strlen-bounded) and will truncate at the
  first embedded NUL. Binary-safe reads must go through the raw
  `fs_get_read_binary()` + `_length()` pair and memcpy into caller
  storage, not through the `fs.read_binary` wrapper.
- **Reserved keywords that trip users up**: `state`, `match`,
  `message` (actor-model hangover), and the literal `none` (the empty
  optional, reserved like `true`/`false`/`null`, so it can't be a variable
  name). Fails in extern param names too.
  `after` parses as something scanner-special too, symptom is a
  "Expected statement in block" at an `if x = call(), …` use site.
  **Prefer the backtick escape over renaming**: a backtick-delimited
  identifier (`` `reply` ``, `` `message` ``, `` `after` ``, `` `when` ``,
  `` `ptr` ``) is always lexed as a plain name, usable as a param / local /
  struct-field / function name. A faithful C→Aether port should backtick the
  keyword and keep the original name rather than rename to `st`/`msg`/`is_match`.
  An unescaped reserved keyword in param position is diagnosed at the keyword
  with the escape taught.
- **Runtime sequence of strings → `*StringSeq`, not `string[]`.**
  `string[]` lowers to bare `const char**` (no length, no refcount).
  `*StringSeq` (`std.string` surface, `string.seq_cons`,
  `string.seq_head`, `string.seq_tail`, `string.seq_length`,
  `string.seq_free`, etc.) is O(1) head/tail/cons/length, refcount-
  aware, structurally shared, pattern-matches with `[h|t]`. Same
  `[a, b, c]` literal builds a cons chain when target is
  `*StringSeq` (message field) or a static C array when target is
  `string[]`. `string.split_to_seq` is the runtime entry point.
- **Unqualified imports are spelt `import mod (*)`, not `import mod unqualified`.** Aether ships glob imports, `import std.math (*)` brings every public symbol into the bare namespace, no `math.` prefix. Selective form `import std.math (sqrt, pow)` is the same shape with an enumeration. There is no `unqualified` keyword and no `use mod::*` (Rust) or `from mod import *` (Python) spelling, just the parenthesised `(*)`. Applies equally to stdlib, contrib, and local modules. When a porter or doc asks for "Java-style import static" or "Rust use", point at this. (Language reference: "Glob Import" section.)
- **`<<MARKER … MARKER` heredocs.** Literal string, no
  interpolation/escaping, reach for them to embed another language's
  source verbatim (`contrib.host.*` snippets, SQL) so the guest's own
  `"…"` need no `\"`. Use a normal `"…"` string when you want `${}`.
  Closing marker is `MARKER` alone on its line; it MAY be indented, but
  only at or below the body's shallowest line (it sits at the content's
  base level, column 0 always works). A more-indented line that reads
  like the marker is content, not a terminator, so it can't silently
  truncate the body; a marker indented past the body is an unterminated-
  heredoc error. The common leading-whitespace prefix is stripped (so you
  can indent the body to match surrounding code); the strip is character-
  exact, so don't mix tabs/spaces in that prefix. Full rules: lexer.c `<<`
  case (#922).
- **Trailing-closure brace goes on the call's line.** `f(x) { … }`
  attaches the block as a trailing closure; `f(x)` then `{ … }` on the
  next line is a separate bare-brace block (the compiler warns). Keep the
  brace on the call line when you mean a closure.
- **Ownership of `ptr`-typed returns.** Strings returned by builtins
  like `string.from_long` / `string.concat` are ref-counted
  (`AetherString*` with a magic sentinel). Safe to pass to other
  string ops without explicit release; they get dropped when the
  last ref goes. Strings returned by `extern fs_foo_raw() -> string`
  are borrowed from a TLS buffer or arena, valid only until the
  next same-kind call or explicit release. If it smells like `ptr`,
  assume borrowed; if it smells like `string` from a builtin, assume
  ref-counted.
- **`@heap` on a `-> string` extern makes its return OWNED.** By default an
  `extern f() -> string` return is *borrowed* (see above), a raw pointer the
  caller must copy-before-free. Annotate the extern return `@heap` (`extern
  http_response_body(r: ptr) -> string @heap`; `@borrow` is the explicit
  default) and codegen tracks the result as a heap string (`_heap_<var>=1`) and
  releases it at scope exit via `aether_heap_str_free` which dispatches on the
  AetherString magic header, so the C side must return a *real* `AetherString*`
  (retain it if it's aliasing something the callee will free). This is what lets
  a value survive its source being freed. `std.http.client.response_body` is now
  `@heap`-owned for exactly this reason: reading the body *after*
  `response_free(resp)` is safe (the old borrowed pointer dangled, the
  serve-and-dial "garbled body" footgun). The borrowed C variant lives on as
  `http_response_body_str` for `_str`/proxy callers that copy-on-use. When you
  need a callee-freed extern string to outlive the callee, reach for `@heap` +
  a retained AetherString rather than a TLS/split-accessor dance.
- **`extern f() -> ptr` type-erases length.** Aether strings are
  length-aware internally, but once they cross an extern boundary
  as `ptr`, Aether sees only the leading bytes up to the first
  NUL. This is why `fs.read_binary` has a paired `_length()`
  accessor, not a single-return.
- **`heap.new(T)` boxes structs with `string` fields**, not just POD. A
  heap-boxed struct owns its string fields (a field store adopts the heap
  string and frees the previous one; `heap.free(p)` releases every owned field
  before freeing the box). So a handler-context like
  `struct AppCtx { db: ptr; data_dir: string }` uses `heap.new` directly,
  don't reach for a raw `malloc(...) as *T` for string-bearing structs.
- **Auditing `std.mem` accessor widths → `aetherc --audit-mem`.** Lists every
  raw `mem.get_*`/`mem.set_*` offset access with the byte width its accessor
  name implies, then exits without codegen. Run it on a C→Aether port to catch
  a wrong-width read (e.g. `get_long` on a 4-byte field pulling adjacent bytes);
  the width-exact accessors exist, this surfaces a wrong choice.
- **Spawning a child binary uses `std.os`, not a separate process
  module.** `os.run_capture(prog, argv, env) -> (stdout: string,
  exit_code: int, spawn_err: string)` is the canonical "fork + exec
  + wait + capture-stdout", argv-based, no shell, binary-safe.
  Caveat: only stdout is captured; child stderr passes
  through to the parent process. The third return slot is the
  spawn-error string ("" on successful spawn even if exit_code != 0;
  non-empty only on fork/exec/sandbox failure). Sibling externs:
  `os_run` (no capture, just exit code), `os_system` (legacy
  `system(3)` shell-out, prefer `run_capture`), `os_execv` (replace
  current process). Don't propose a new `std.process`.
- **XML parsing lives in `contrib`, not `std`.** `contrib.parsers.xml_expat`
  is a SAX-style libexpat wrapper, start/end/text callbacks,
  attribute access, entity decoding, multi-chunk streaming. Add
  `extra_sources = ["contrib/parsers/xml_expat/aether_xml_expat.c"]` and
  `link_flags = "-lexpat"` per the contrib README. Don't hand-roll
  an `index_of`-based parser, that exact mistake landed in
  fbs-core's S3 port (#627) because the porter didn't realise
  `contrib.parsers.xml_expat` existed.
- **Operator-supplied Liquid templates → `contrib.templating.liquid`.**
  Pure-Aether Shopify-Liquid port. Use it for email bodies, dashboards,
  generated reports, anywhere a trusted-human operator writes a
  template but you don't want their template to execute arbitrary
  code. `{% include %}` / `{% render %}` need an explicit
  `context_set_include_root(ctx, root)` (path traversal blocked via
  `fs.is_within_base`). Phase 1 ships scalar typed bindings
  (`context_put_int`/`_float`/`_bool`/`_nil`); array / object / dotted
  path access is Phase 2. Don't hand-roll a Mustache or own-syntax
  templater unless you have a specific reason, this is the supported
  path.
- **HTTP client lives in two tiers, server in one.** `import
  std.http` gives v1 client one-liners (`http.get(url) -> (body,
  err)`, `http.post`, `http.put`, `http.delete`) plus the entire
  server surface (`http.server_create(port)`, route handlers, etc).
  `import std.http.client` gives a v2 builder (`client.request(method,
  url)` → `set_header` → `set_timeout` → `send_request`) when the
  one-liners are insufficient, anything needing custom request
  headers, custom HTTP methods, response status discrimination,
  per-request timeouts, or response headers. Reach for v2 by default
  for non-trivial client work; v1 is fine for "GET, expect 200, want
  body". Server-side stays in `std.http` (there's no v2 server).
  Record/replay HTTP testing (Servirtium VCR) is NOT in the stdlib, it's
  a separate repo,
  [`servirtium-vcr`](https://github.com/aether-lang-org/servirtium-vcr)
  (`import core.vcr`). Don't look for `std.http.server.vcr`.

## Type-system & effect features worth reaching for

Compiler-checked capabilities an LLM should know exist before speccing a
workaround, they cover cases a porter often hand-rolls.

- **Optionals `T?` for "maybe a value", not `(value, "missing")`.** When a
  value is simply present or absent (a missing map key, an empty list's first
  element, a search that found nothing), reach for `T?` instead of a sentinel
  tuple. `let m: int? = 69` / `let z: int? = none`; test with `== none`;
  force-unwrap `m!` (panics on none); default with `m ?? d` (binds tighter than
  arithmetic); chain with `v?.field` (none-propagating, `v?.field = x` is a
  no-op when none); destructure with `match m { none -> …  some(v) -> … }`. A
  bare `T` (or `none`) is implicitly wrapped into a `T?` slot. Keep `(value,
  err)` for *fallible* operations (where you want to say why it failed); `T?` is
  the orthogonal "present or absent" axis. Works for value and reference element
  types alike (one uniform representation, so no null-vs-absent ambiguity).
  Postfix `!` is shared with the `(value, err)` unwrap-or-trap and resolves by
  operand type, so it never collides with the actor-send `!` (followed by a
  message type) or `match` arms.
- **Distinct types: `type Name = distinct Base`.** Zero-cost nominal wrapper
  over a scalar / `string` / `ptr` `type USD = distinct float`,
  `type Fd = distinct int`. Lowers to the base C type (no boxing) but is
  nominally separate: crossing the boundary needs an explicit `as`
  (`9.99 as USD` to wrap, `usd as float` to unwrap). A `Fd` param rejects a raw
  `int`; `EUR` is rejected where `USD` is wanted. The compiler-checked way to
  give capability tokens / units / handles their own type.
- **`bitstruct Name : uint8_t { f: bool 0, g: int 1..=3 }` for packed layouts.**
  A named bit layout over one unsigned integer, for wire formats / headers /
  hardware registers. Reach for it instead of the older extern-struct bitfield
  (`name: type : N`), which emits a **raw C bitfield** and is therefore always
  *signed* (a stored `0b111` in a 3-bit field reads back as `-1`) — the reason
  every unsigned read of one has to be hand-masked with `& ((1<<N)-1)`. A
  bitstruct lowers to shift/mask on an unsigned word instead, so it cannot
  sign-extend. Backing type is mandatory (`uint8_t`/`uint16_t`/`uint32_t`/
  `uint64_t`); bit ranges are explicit and spelled `1..=3` (inclusive) or `1..<4`
  (exclusive); overlaps need `@overlap`; it's strictly nominal (cross to the raw
  word with `as`). Byte order is NOT part of it — compose with `std.mem`'s
  `get_u16_be` / `set_u32_le` etc., so the swap stays visible.
- **Unsigned data → `uint64`, not `long`, so `>>` and masks behave.** Aether's
  `long` is signed, so `>>` on it is an *arithmetic* shift: `long ones =
  0xFFFFFFFFFFFFFFFF; ones >> 8` is `-1` (sign-extends), not `0x00FF…FF`. Declare
  a variable `uint64` and `>>` is logical (`uint64 ones = 0xFFFF…FF; ones >> 8`
  == `0x00FF…FF`), matching what crypto/codec code means by a bit pattern. This
  bit the v1.2 Ascon-AEAD decrypt mask (#1285): the KAT *encrypt* passed but the
  round-trip *open* failed only on partial blocks, because the tag-reconstruction
  mask never cleared its top bits — the KAT's encrypt-only assertions couldn't
  see it. The escape hatch when a value must stay `long` is `bits.lsr64(x, n)`
  (logical shift). Right-shifts that are immediately `& 0xFF`-masked (byte
  extraction like `(v >> 4) & 15`) or shift a known-non-negative counter are
  fine either way; the hazard is specifically a high-bit-set word right-shifted
  *for its bits* (an all-ones mask, a 64-bit state word). Prefer `uint64` for
  sponge/state words up front rather than sprinkling `lsr64`.
- **Function contracts `requires` / `ensures` (issue #348).** Eiffel-style
  pre/postconditions between the return arrow and the body: `add(a: int, b: int)
  -> int requires a >= 0 ensures result >= a { … }`. Each is a boolean
  expression; `requires` checks at entry (params in scope), `ensures` checks
  before each `return` with `result` bound to the value. Violation is a hard
  panic (`precondition violation: <expr> in <fn>`) — a programmer-error signal,
  **not** a `(value, err)`. Suppressed by `--no-contracts`. The param-level
  `where` below is the same machinery attached to a single parameter.
- **Gradual `where` contracts on params.** `divide(a: int, b: int where b != 0)`
  a runtime-checked precondition that lowers to an entry guard; a violation is
  a hard panic (`precondition violation: b != 0 in divide`), a programmer-error
  signal, **not** a `(value, err)`. Opt-in/gradual (a param with no `where` is
  unchecked); `and`-composable; suppressed by `--no-contracts`.
- **Contracts FOLD at compile time when operands are constants.** A predicate
  that is provably true elides its runtime check; provably false is a
  **compile error** — at the definition (`requires MIN <= MAX` after a const
  refactor staled it) or at a call site with the constant arguments
  substituted (`divide(10, 0)` fails the build; `divide(10, n)` stays a
  runtime check). Trait-bound-like checking from the contract syntax alone.
  The evaluator is int64-exact, resolves `const` names and enum members,
  never evaluates calls (capability boundary), and anything undecidable
  keeps today's runtime check silently. Fold errors fire even under
  `--no-contracts`. Escape hatch for deliberately-unreachable violating
  calls: pass the value through a runtime variable. See
  docs/contract-folding.md.
- **Per-function effect tags + purity.** Annotate a function `@pure` / `@no_fs`
  / `@no_net` / `@no_os`; a whole-program call-graph pass errors if the named
  capability is reached transitively (e.g. `@no_fs` calling `file.read_all`). A
  finer, per-function axis layered on the build-time `--with=fs,net,os` gate.
  Compile-time builtin `__pure(fn)` folds to a `true`/`false` constant so code
  can branch on purity at compile time. A raw `extern` is unclassifiable →
  treated as impure, matching the `--with=` boundary.
- **`@scoped` bindings, opt-in escape analysis.** `@scoped let buf =
  make_buffer()` declares the value must not outlive its lexical block; the
  typechecker rejects every escape (return, alias into another binding/field,
  aggregate literal, closure capture, `list.add`/`map.put`). Only a scalar
  *derived* from it may escape (`return buf.len()`). Not a borrow checker, one
  opt-in annotation turning a non-escape into a checked invariant.

## Working with downstream users

- **aether-ui** (`https://github.com/aether-lang-org/aether-ui`) 
  cross-platform widget toolkit (GTK4 / AppKit / Win32) with an
  AetherUIDriver HTTP test server. Was `contrib/aether_ui/` in this
  repo until the spin-out; now consumes Aether the same way external
  users do (install + `$(ae cflags)`). Useful reference for the
  embedded-DSL pattern: the toolkit's surface IS a closure-DSL (think QML or Flutter)
- **aeb** (`https://github.com/aether-lang-org/aeb`) 
  multi-package and multi-language build system, the second major sibling.
  Bazel-shaped in ambition but stops short of full reproducibility.
  Reads `share/aether/MANIFEST` to discover
  link-suitable runtime/stdlib `.c` files and orchestrates per-package
  compile + cache + incremental relink. The MANIFEST contract
  (`docs/install-layout.md`) was carved out specifically to support aeb
  without forcing it to guess via `find -name '*.c'`. If you're touching
  install-layout / shipped source / link contract, ping aeb side.
  See also google-monorepo-sim (../google-monorepo-sim) which showcases aeb.
- **aeo** (`https://github.com/aether-lang-org/aeo`) 
  the third major sibling:
  an infrastructure orchestrator that stands up / tears down a dependency-
  ordered tree of VMs + containers (FreeBSD jail/bhyve, Linux
  podman·docker/KVM) from one Aether composition (`aeo up|status|down|
  dry-run compose.ae`). Not a build system and not an aeb SDK, it's
  *built by* aeb and shells *to* aeb across a plain artifact+CLI seam. Its
  compose surface is the `config IS code` closure-DSL applied to live
  infra, the canonical proof the DSL pitch works beyond config files.
  Has an `aeo-agent` and attempt to adhere to the principles of containment.
- **aeocha** (`https://github.com/aether-lang-org/aeocha`)
  BDD-style test framework for Aether (`describe` / `it` / `before_each` /
  `after_each` via trailing blocks + closures, Cuppa-inspired). The
  reference consumer of the trailing-block/closure DSL for a test surface;
  look here for a worked example of the `builder`-shaped API in practice.
  There is a co-located mutation testing facility too.
- **svn-aether port (avn)** (`https://github.com/aether-lang-org/avn`) 
  is a big
  real-world consumer. Port is methodical C → Aether, one-leaf-per-
  commit. Downstream finds the gaps before anyone else, every
  cross-`import` typer issue (struct visibility, selective-import
  propagation, 128-decl cap) was filed by avn before showing up
  anywhere else. As much as anything it was used to shake out
  missing features and bugs for Aether.
- **mquickjs-port** (`https://github.com/aether-lang-org/mquickjs-port`)
  a full C→Aether port of Bellard & Gordon's
  MicroQuickJS (ES5-subset embedded JS engine, tracing GC, ~10 kB RAM). The
  end state is pure Aether: `mquickjs.c` deleted entirely, every engine leaf
  now an `ae/*.ae` file, built with `aeb`, tested via the `c.tests` runner.
  This is the extern-removal / no-C-consumers exemplar the memory notes keep
  referencing, it drove the hardest low-level features (const arrays for the
  atoms table, signed-bitfield emission, shift-width and `as int`-narrowing
  fixes, the `--audit-mem` accessor-width check). `migration_assessment.md` 
  and `ae/PORT_STATUS.md` were used in the port but could be out of date. 
  `dtoa.c` stays C by decision; the double-to-string path is not worth
  reimplementing in Aether.
- **servirtium-vcr** (`https://github.com/servirtium/servirtium-vcr`)
   record/replay for HTTP service tests in the
  [Servirtium](https://servirtium.dev) markdown-tape format, as a **one-engine-
  many-thin-bindings** monorepo. The engine is a single pure-Aether module
  (`core/vcr.ae` + `core/embed.ae` C-ABI) built once as `libservirtium_vcr.so`
  via `ae build --emit=lib` on top of the stdlib (HTTP server, regex, zlib,
  crypto); 17+ language bindings (Go/cgo, Python/ctypes, Java·Panama, Ruby·
  Fiddle, Rust, Node·koffi, Haskell, Elixir/Erlang-NIF, …, plus the JVM family
  over the Java jar) are thin FFI wrappers over that one artifact, so they
  can't drift, cross-language Servirtium compatibility is a build-time
  guarantee, not a test target. The canonical proof of the `--emit=lib` +
  auto-generated-SDK story at scale, and the reference for how a downstream
  wires many FFI consumers through one `aeb` run. Aether-side entry is
  `import core.vcr` it's a separate repo, not part of `std.http` (the
  HTTP-client idiom above says the same).
- **zsync-port** (`https://github.com/aether-lang-org/zsync-port`)
  a pure-Aether port of zsync (rsync-over-HTTP: fetch only the changed blocks
  of a file, driven by a `.zsync` control file). ~3.4k lines of Aether + one
  small Artistic-licensed C shim (`rcksum/fileio.c`, positional file I/O); the
  rolling-checksum engine, control-file format, HTTP-range downloader, and a
  native test file server are all Aether over `std.http` (client + server) and
  `std.cryptography` (MD4/SHA-1). Notable as the **speed-of-port exemplar**: a
  Go→Aether port done by a sibling Claude in ~4h, built leaf-first and
  parity-gated byte-exact against the original Go (`legacy_golang` branch is the
  oracle), which surfaced three stdlib gaps filed + landed upstream (#637 md4_hex,
  #640, #641). Also the **licensing-boundary** case: it's **Artistic-2.0, NOT
  MIT**, keep its import graph free of MIT aeb/aeocha *modules*, but building it
  *with* aeb (a tool, like make/gcc) or linking a zsync `.so` is fine and does
  not relicense (Artistic §7/§8). Builds two ways (Makefile via `build/ae`, or
  `aeb cmd/zsync/.build.ae`); the aeb path wants the *installed* Aether's nested
  include layout (dev-tree include-threading is a known aeb gap).
- **Feature request flow that works**: downstream writes a spec
  (e.g. `import_typer_at_scale.md`, `exprt_structs.md`,
  `stdlib_wish.md`), Aether implements, downstream adopts within the
  same day. The specs are extremely concrete: API names, signatures,
  rationale, call-site census. Match that level when responding.
- **Don't gate on things that aren't real threats.** `--emit=lib`
  capability-empty is right for the embedded-DSL case (host accepts
  untrusted Aether). avn / aether-ui / aeb / aeo are the opposite case,
  they are hosts. The `--with=fs` opt-in covers both cleanly.
  Similar dualities will come up; watch for them.
- **Downstream embedding link line: ALWAYS `$(ae cflags)`.** Don't
  hand-craft `-I` / `-L` / `-laether`. The install layout
  (`<prefix>/lib/aether/libaether.a`) requires an explicit
  `-L<prefix>/lib/aether` that bare `-laether` won't find, and the
  include path varies between dev/user/system installs. `ae cflags`
  emits the right flags for whichever install is in effect.

## Build / test commands

- Full build: `make` (from repo root). Rebuilds `build/aetherc` and
  `build/ae`. Incremental builds sometimes miss `aetherc` reshapes;
  `make clean && make -j$(nproc)` when in doubt.
- C unit tests: `make test` (builds and runs `build/test_runner`). Hot
  inner loop during feature work: `make test-ae` (parallel runner for the
  `.ae` regression/integration tests). `make ci` runs both as steps 4 and 5.
- JSON conformance: `make test-json-conformance` must pass 95/95
  `y_*` + 188/188 `n_*` for JSONTestSuite.
- Sanitizers: `make test-json-asan` / `make test-json-valgrind`.
- **Version drift after fetch.** Makefile derives `AETHER_VERSION`
  from the highest `v*.*.*` git tag, falling back to `VERSION` file.
  If `aetherc --version` disagrees with CHANGELOG `[current]` or
  with `cat VERSION`, the local tags are behind: `git fetch --tags`,
  then `make clean && make`.
- **Coverage path is gcc `--coverage` + gcov, no custom mapper.**
  Codegen emits `#line N "src.ae"` directives, so gcov reads them
  natively, `gcc --coverage` on a generated .c produces a `src.ae.gcov`
  (line + branch hits against .ae source) with no extra plumbing. To wire
  `make ci-coverage`: add `-fprofile-arcs -ftest-coverage` to the build
  (mirror the `build/asan-obj/` variant pattern), run the tests, shell
  `gcov` over the .gcda files.
- **Rebuild-trigger → minimum-test table** ("I touched X, so rebuild Y, run
  Z"). `build/ae` *execs* `build/aetherc` (it doesn't link the compiler in),
  so a compiler change takes effect the moment `aetherc` is rebuilt, no need
  to relink `ae`.

  | Touched | Rebuild | Then run |
  |---|---|---|
  | `docs/**`, any `.md` | none | nothing (doc-only) |
  | `compiler/**` (parser, analysis, typechecker, codegen) | `make compiler` | the targeted `tests/regression/*.ae` via `build/ae`, then `make test-ae` |
  | `std/<m>/**` | `make stdlib` | `rm -f build/test_*` (stale probe binaries), then that module's tests |
  | `runtime/**` | none for `.ae` builds (runtime `.c` is compiled fresh into every `ae build`); `make test` relinks the C unit runner | `make test` + the affected `.ae` tests |
  | `tools/ae.c` (the `ae` driver) | `make ae` | the relevant `tests/integration/*` |
  | `contrib/**` | `make contrib MODULES=<m>` | that module's `tests/integration/` test |

- **Never overlap a build with a test run.** Running `make test-ae` while
  `make compiler`/`make stdlib` is still in flight gives *false* failures,
  tests compile against a half-written `build/aetherc` / `libaether.a`. The
  tell: a wave of `(compile error)` / `(shell test)` failures that all **pass
  when re-run standalone**. Let the build finish, then test.
- **A few regression probes are RSS-threshold leak tests**
  (`heap_leak_cross_fn_recursion`, `heap_tracker_return_escape_no_leak`).
  They measure their own resident-set growth across a loop and assert it
  stays bounded. They were hardened to run two loops and bound the smaller
  growth, which drops the earlier false positives under full-suite memory
  pressure; `leaks --atExit` remains the authoritative macOS leak gate if
  one ever looks suspect.
- **Stale `~/.aether/cache`.** `ae build`/`ae run` cache compiled binaries by
  source hash; if behaviour seems impossibly stale after an edit (or after
  switching branches), `rm -rf ~/.aether/cache`. `make clean && make -j$(nproc)`
  is the full reset to known-good. Maybe this should be investigated.

## Branch / PR conventions

- Prefix with `feat/` for features, `fix/` for bug fixes, `docs/`
  for doc-only. Examples in merged history: `feat/json-object-iteration`,
  `fix/parser-ergonomics-and-cflags`, `fix/block-scope-restoration`.
- CHANGELOG entries go under `[current]` as part of the PR. The
  release workflow renames `[current]` → `[X.Y.Z]` at tag time.
- Never commit to `main` directly. Push the feature branch, open a
  PR, wait for green CI (includes Windows MSYS2 + MINGW-w64
  cross-builds that catch `#include` gaps Linux doesn't).
- **When to ask first.** Touching more than ~5 files, or crossing subsystem
  dirs (`compiler/`, `std/`, `runtime/`, `tools/`); any change to diagnostic
  text or ordering, codegen output shape, or a public CLI flag; and committing
  or pushing (the standing rule above). A doc-only sweep across many files is
  fine, just call it out.

## Invariants to not break

- `--emit=lib` stays capability-empty by default. `--with=` is the
  only escape hatch; don't add backdoors (per-function allowlists,
  `--with=all`, etc.).
- The `aether_<name>()` ABI mangling is stable across releases. If
  you rename an export, the old name stays as an alias.
- `std.string.from_int(int)` and `string.from_long(long)` are
  separate, don't merge. `long` is 64-bit on every target including
  MSVC (where C `long` is 32 bit) because Aether's `long` type is
  defined as `long long` at the C level.
- Iteration order in `std.json` is insertion order across both parse
  and builder paths. Documented contract, several downstream users
  (including svn-aether's server) rely on it.
- **Any C symbol codegen emits a call to must be linkable in EVERY
  build, including WASM.** The `ci-wasm` Makefile target uses its own
  minimal `RUNTIME_FILES` list (not `RUNTIME_SRC`); add new runtime
  files there too, and keep the symbol self-guarded so it's a no-op
  object off its target OS. Otherwise `wasm-ld` errors `undefined
  symbol` while every native platform is green, a failure mode that
  hides until the WASM job runs (e.g. an OS-specific startup hook).

## When stuck

- `git log --oneline -30` to see what just shipped.
- Search tests: `grep -rn "capability\|emit.lib\|with=" tests/`.
- Check `docs/` first, then `compiler/` for language-level questions,
  then `std/<module>/` for runtime.
- The CHANGELOG has working-memory for what-landed-when. Treat it as
  authoritative for "is feature X available yet."
- **Map generated C back to a codegen site.** Drop a unique marker at the
  suspect emit, `fprintf(gen->output, "/*AEDBG1*/ ...")` `make compiler`,
  then `build/aetherc x.ae /tmp/x.c` (or `build/ae build --emit-c x.ae`) and
  grep `/tmp/x.c` for `AEDBG1` to see which path fired and what surrounds it.
  Same trick with a `fprintf(stderr, ...)` in the parser/typechecker to trace
  resolution. Remove the marker when done.
