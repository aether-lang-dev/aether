# Changelog — archive (0.259.0 and older)

> Paths under `contrib/aether_ui/` in the entries below no longer resolve.
> The UI library outgrew this repo and moved to a sibling one; the entries
> are left as written, since a changelog is a record of what happened.

Older release notes, split out of the active
[CHANGELOG.md](CHANGELOG.md) to keep the live file small. Format and
conventions are identical; new entries always go in `CHANGELOG.md`, not
here. Sections are most-recent-first, continuing down from where the
active changelog ends.

## [0.259.0]

### Changed

- **stdlib caps-audit — `std.fs` directory listing, copy buffer, and
  path-clean stack** (#462). Routed the remaining *unbounded / large*
  POSIX allocations in `std/fs/aether_fs.c` through the capability
  allocator (the marquee file-read paths — `file_read_all_raw`,
  `fs_read_binary_raw`, `fs_pread_raw` — were already converted). Now
  capped: the directory listing (`dir_list_raw`/`dirlist_add`'s entries
  array + every strdup'd entry name + the `DirList` struct — a huge
  directory is an unbounded surface), the 8 MiB `fs.copy` read/write
  scratch buffer, and `path_clean`'s segment stack. A new `capacity`
  field on `DirList` lets `dir_list_free` release the entries array with
  its exact byte size; entry names free via `strlen`-at-free. Realloc
  growth threads the old size; realloc-failure paths free the old size.
  New `caps_fs_dir_list_accounting_balances` unit test asserts the cap
  counter returns exactly to baseline after list+free (no drift). The
  bounded caller-owned path strings (`path_join`/`dirname`/`basename`/
  `extension`/`rel`, `readlink`) and the Windows-only UTF-16 conversion
  buffers remain tracked #462 follow-ups. Verified: unit 228/228
  (ASan-clean), full `.ae` regression 0 failures.

## [0.258.0]

### Parser

- **Newline-led `[` array-literal recognised as a fresh statement,
  not folded into the previous line as indexing** (issue #528,
  `compiler/parser/parser.c`,
  `tests/regression/test_expr_parser_newline_bracket.ae`). The `*`
  guard PR #527 added covered the typed-pointer / deref-store
  cases; this extends `is_newline_led_statement` with a `[` branch
  for the array-literal shape. Discriminator: a newline-led `[`
  followed by a single-token primary (identifier / number / string
  / `true` / `false` / `null`) and a top-level `,` provably can't
  be a continuation index, because indexing accepts a single
  expression. Other line-led `[` shapes (single-element multi-line
  indexing, multi-token first elements) are left to fold as before
  — the conservative bias is "don't break legit code." See the
  comment block above the function for the per-operator decision
  log including the operators (`-`, `+`, `(`) deliberately NOT
  added because no inspection-cheap discriminator exists; the
  honest fix for those is lexer-level newline significance, a
  separate decision parked in #528.

## [0.257.0]

### Documentation

- **`docs/dsl-without-macros.md`** — a comparison of Aether's
  trailing-block DSL approach against Rust-style macros, with worked
  three-way examples (RSpec/Jest/Aether for test-spec grammars,
  Diesel/jOOQ/Aether for SQL builders, Unity/Three.js/Aether for an
  interactive-3D scene with no canonical source form). Committed
  `[skip actions]` as a docs-only change; this section records the
  release that carried it (the commit deliberately skipped the release
  pipeline, which is why no `## [0.257.0]` header was cut at the time).

## [0.256.0]

### Added

- **Connection-level request accessors: `remote_port`, `local_addr`,
  `local_port`, `scheme`, `is_tls`, `http_version`**
  (`std/net/aether_http_server.h`, `std/net/aether_http_server.c`,
  `std/http/module.ae`,
  `tests/integration/http_request_conn_accessors/`). Sibling batch to
  `request_remote_addr` — closes the obvious gaps vs HAProxy / CGI /
  Spring on connection-side metadata a handler should be able to
  reach without re-parsing wire bytes:
  - `request_remote_port(req) -> int` — companion to `remote_addr`
    for per-connection log de-dup and ephemeral-source firewall
    matching.
  - `request_local_addr(req) -> string` /
    `request_local_port(req) -> int` — `getsockname(2)` for the
    accepted fd. Lets a handler learn *which* NIC fielded the
    request when the listener bound 0.0.0.0 (admin-on-loopback,
    multi-tenant per-IP routing).
  - `request_scheme(req) -> string` (`"http"` / `"https"`) and
    `request_is_tls(req) -> int` — derived from `conn->ssl`. Drives
    canonical-URL building, redirect targets, cookie Secure-flag
    decisions in app code.
  - `request_http_version(req) -> string` — surfaces the field the
    parser already captured (`"HTTP/1.0"` / `"HTTP/1.1"` /
    `"HTTP/2.0"`).
  All six are populated alongside `remote_addr` in the dispatcher
  hot path; one additional `getsockname` syscall per request, IPv4
  + IPv6 via `sockaddr_storage`. Sketched in the aeb-agent ask's
  follow-up triage.

- **`http.request_remote_addr(req) -> string` — trusted TCP peer
  address for in-app source-IP allow/deny lists**
  (`std/net/aether_http_server.h`, `std/net/aether_http_server.c`,
  `std/http/module.ae`, `tests/integration/http_request_remote_addr/`).
  Returns the IP `getpeername(2)` reports for the connection that
  carried the request — dotted-quad for IPv4 ("203.0.113.7"), the
  `inet_ntop` form for IPv6 ("2001:db8::1"). Distinct from the
  `X-Forwarded-For` header (client-supplied, spoofable) used by
  `std.http.middleware.use_real_ip`: use the new accessor when the
  server is directly exposed and a header-based allow-list would be
  a control that *looks* enforced but isn't. Returns `""` when
  unavailable (Unix-domain socket, syscall failed, null req).
  Populated once per parsed request next to the conn fd; one cheap
  syscall + `inet_ntop` per request. Filed by aeb-agent
  (`aether-http-server-expose-peer-addr.md`): they wanted
  `--allow-from <cidr>` as defense-in-depth on top of lease/token
  auth and found `X-Forwarded-For` was the only client-IP signal a
  handler could read — not a basis for an access-control decision.

## [0.255.0]

### Fixed

- **Closure-env UAF Part 3 — `_ctx`-injection drain gate must look up
  the callee under its normalised (dot→underscore) name** (`closure-env-
  freed-when-passed-to-extern-callee.md`,
  `compiler/codegen/codegen_stmt.c`,
  `tests/integration/closure_qualified_ctx_inject_no_uaf/`). Part 2
  added an AST-arg-index → function-def-param-index shift at the drain
  gate to account for `_ctx: ptr` builder auto-injection. The shift
  looks the callee up in the merged program AST, but used the *raw*
  call-site value (e.g. `aether_ui.btn`). Cross-module functions live
  in the merged AST under their *normalised* name (`aether_ui_btn`),
  so for qualified-call sites (`import aether_ui` +
  `aether_ui.btn(...)`, aether-ui's actual usage) the lookup missed,
  the shift was skipped, and the escape walk asked about the wrong
  parameter (`label`, provably non-escaping) instead of `on_press`
  (which `box_closure(on_press)` launders into the retaining extern).
  False-non-escape → drain fires → C-side invocation later reads the
  freed env → UAF (`heap-use-after-free` at `example_testable.ae:24`,
  reported by aether-ui after Parts 1+2 shipped in 0.251.0). Fix
  normalises with `codegen_normalise_callee` before the lookup, same
  as the visible-body and escape-walker callers already do. Part 2's
  selective-import test passed without this because selective imports
  lower the call site to a bare unqualified name; the new test locks
  in the qualified-call path that aether-ui uses.

## [0.254.0]

### Testing & docs

- **Lock in the typed/qualified C-pointer guarantee** (#703). The feature
  (`extern type`, `const ptr`, `cstring`/`cstring_const`, `*T`, `const *T`)
  already shipped, but its decisive promise — an Aether-owned or extern
  prototype matching a C header EXACTLY so the generated TU compiles with
  that header force-included (no conflicting-declaration error), restoring
  C's caller/definition cross-check — had no regression test
  (`tests/regression/test_c_typed_pointers.ae` only proves the spellings
  parse/typecheck/run; it never compiles a prototype against a force-
  included header). Adds `tests/integration/c_qualified_ptr`: compiles a
  generated `.gen.c` with its exact-match header force-included (positive),
  asserts the emitted prototypes use `const void*` / `const char*` / `char*`,
  and asserts a drifted signature is rejected at C compile time (negative).
  Documents the spellings in the user-facing
  [language reference](docs/language-reference.md) (Typed and qualified C
  pointers) — previously only in the internal porting-gaps doc.

## [0.253.0]

### Added

- **Import object-like C macros** via `extern const NAME: type @c_import`
  (#702). Object-like C macros (`EAGAIN`, `O_NONBLOCK`, `LLONG_MAX`, a
  build-generated `REDIS_GIT_SHA1`, the errno/`C_OK` families) have no
  linkable symbol, so they were invisible to Aether — ports inlined the
  platform's numeric value (wrong on the next platform) or kept a one-line
  C accessor per macro. This declaration teaches the typechecker a name and
  an Aether type; generated C emits the macro name **verbatim** at every use
  site and emits nothing for the declaration itself (no value, no `#define`,
  no forward decl), so per-platform values come out right by construction —
  the including TU's headers are the source of truth. The type is trusted as
  declared, the same model as `extern` functions; `@c_import` is required.
  Object-like macros only. See
  [docs/language-reference.md](docs/language-reference.md) (Extern Functions).

## [0.252.0]

### Fixed

- **Builder/ctx lowering: emit explicit casts at the int↔void* boundaries
  that the universal-void*-handle design introduces.** Two missing casts
  in `compiler/codegen/codegen_stmt.c` (the `_bcfg = factory()` store) and
  `compiler/codegen/codegen_expr.c` (passing `_builder` to int-typed
  callee params) emitted bare int↔pointer conversions that GCC ≤13 warned
  about (`-Wint-conversion`) and GCC 14+/MinGW64 reject by default.
  Blocked the entire aether-ui Win32 bring-up.
  - Factory return → `void* _bcfg`: now emits `(void*)(intptr_t)factory()`.
  - `void* _builder` → int callee param: now emits `(int)(intptr_t)_builder`
    (mirror of the existing TYPE_INT → TYPE_PTR coercion).

  Regression: `tests/integration/builder_int_factory_void_ptr_cast`
  compiles the generated C with explicit `-Werror=int-conversion`; pre-fix
  it errors, with the fix it's clean. See
  `builder-ctx-handle-void-ptr-int-conversion.md`.

## [0.251.0]

### Fixed

- **Closure-env UAF: extern callees and `_ctx`-injection builders no
  longer free a closure's heap env at the call site when the C side
  retains it.** Two related bugs in the `emit_closure_env_drained_call`
  gate (`compiler/codegen/codegen_stmt.c`):
  - **Part 1** — the gate required only "no escape proved" (a verdict
    the body-walk silently defaults to for callees with no visible
    body). Extern callbacks-registry sites (`register_cb(|| {...})`)
    were therefore drained even though the C side keeps the boxed
    closure and invokes it later. Now also require
    `callee_has_visible_body`; unknown-body callees stay conservatively
    escaping (leak ≫ UAF).
  - **Part 2** — when the callee is a `_ctx: ptr` builder and the user
    omits `_ctx`, codegen auto-injects `_aether_ctx_get()` at AST arg
    0, shifting the closure arg's AST position by one relative to the
    function-def param position. The gate consulted the escape walk
    with the unmapped AST index, asking about `label` (provably
    non-escaping) instead of `on_press` (escapes via
    `box_closure(on_press) → extern store`), got a false-non-escape
    verdict, and fired the drain. This was aether-ui's calculator UAF:
    `btn("7") callback { call(digit, 7) }` on a `btn(_ctx, label,
    on_press)` wrapper. The fix maps AST arg index → function-def
    param index whenever the callee's first param is `_ctx` and
    `user_args == declared_params - 1`.

  Two regression tests:
  `tests/integration/closure_extern_retains_no_uaf` (Part 1, direct
  extern-callee shape) and
  `tests/integration/closure_builder_ctx_inject_no_uaf` (Part 2,
  cross-module visible-body wrapper with `_ctx`-injection — the
  calculator pattern). Both run under ASan; SEGV on stock 0.250, clean
  with the fix. See `closure-env-freed-when-passed-to-extern-callee.md`
  for the full investigation trail.

## [0.250.0]

### Added

- **`contrib.templating.native`: `pretty_print_html` / `pretty_print_xml`
  (debug-only).** Two post-processors that take the tight output of
  `render_html()` / `render_xml()` (or any externally well-formed
  HTML / XML string with this module's escape conventions) and return
  an indented version with `\n` line endings between elements.
  Surface:
  ```aether
  pretty_print_html(s: string, indent_size: int) -> string
  pretty_print_xml(s: string, indent_size: int)  -> string
  ```
  `indent_size` honoured in the range `1..8`; out-of-range falls
  back to 2. Pretty-print is for human reading / diffing during
  development; production paths should use `render_html()` /
  `render_xml()` directly. **HTML carve-outs:** `<pre>`,
  `<textarea>`, `<script>`, and `<style>` pause depth tracking for
  their subtree — their bodies pass through byte-for-byte because
  whitespace inside them is significant to rendering / behaviour.
  **XML has no whitelist:** every element gets indented; if your
  XML uses mixed content or CDATA-sensitive data, render the tight
  version instead. The post-processor understands `<name>` /
  `</name>` / `<name/>` plus pass-through for `<?...?>`,
  `<!--...-->`, and `<![CDATA[...]]>`. Once the v2 XML attribute
  story lands, the parser here will need updating to skip
  `key="value"` runs inside `<`...`>`. New test
  `tests/integration/native_templating_pretty/` (10 cases — flat
  body, nested elements, `<pre>` and `<script>` carve-outs, XML
  prolog handling, self-close elements, indent size choice with
  out-of-range fallback, end-to-end DSL → pretty round-trip).

## [0.249.0]

### Added

- **`contrib.templating.native`: XML emitter (skinny v1).** Sibling of
  the HTML DSL — same trailing-block shape, XML escape rules:
  ```aether
  feed = native.render_xml() {
      native.xml_tag("rss") {
          native.xml_tag("channel") {
              for i in 0..items_n {
                  native.xml_tag("item") {
                      native.xml_tag("title") { native.xml_text(titles[i]) }
                  }
              }
          }
      }
  }
  ```
  Surface: `render_xml()` (prefixes `<?xml version="1.0" encoding="UTF-8"?>`),
  `xml_tag(name) { ... }`, `xml_self_close(name)`, `xml_text(s)`,
  `xml_raw(s)`. `xml_text` escapes the five canonical XML entities
  (note: `'` → `&apos;`, NOT `&#39;`; `>` also escaped, matching
  `std.xml`'s writer). **Skinny v1 deliberately omits attributes,
  namespaces, processing instructions, and DOCTYPE** — attribute
  shape (attrs-as-map vs. deferred-`>` state machine) needs a real
  downstream user to settle, and the others fall out of `xml_raw`
  until that user appears. New test
  `tests/integration/native_templating_xml/` (7 cases — prolog,
  basic wrap, self-close, escape of all 5 metas, raw bypass, for-loop
  composition, `${...}` interpolation of a captured user value).
- **`contrib.templating.native`: builder DSL.** The walking-skeleton
  `html_text(sb, s)` / `html_tag_open(sb, s)` API gains a sibling
  surface that lets you write templates as Aether closures with
  trailing-block tag emission:
  ```aether
  out = native.render_html() {
      native.tag("ul") {
          for i in 0..items_size {
              native.tag("li") { native.text(items[i]) }
          }
      }
  }
  ```
  `render_html()` returns the assembled string. `tag(name) { ... }`
  emits `<name>BODY</name>` where BODY is the rendered output of the
  trailing block. `text(s)` HTML-escapes the five canonical entities;
  `raw(s)` bypasses escaping (trusted markup only). The `for` is
  Aether's own — no template-language reimplementation, no separate
  expression evaluator — so `if`, `switch`, ref cells, captured
  closures, and `${...}` interpolation all compose. Implementation:
  each `tag()` is a `builder` function with a fresh `std.strbuilder`
  as its per-tag emitter; after the trailing block runs, `tag()` reads
  the body bytes and writes `<name>body</name>` to the parent emitter
  via the builder-context stack. Trade-off: one strbuilder allocated
  per `tag()` call. The plain-function surface
  (`html_text(sb, ...)` etc.) stays as the escape hatch for
  performance-sensitive paths. New integration test
  `native_templating_dsl/` (6 cases — basic wrap, nested tags, escape
  of all 5 metas, raw bypass, `for` loop inside a tag block, `${...}`
  interpolation of a captured user value). Note the parens
  (`render_html()`, `tag("ul")`) — Aether trailing blocks attach only
  to call sites with parens.

### Changed

- **`contrib.xml.expat` → `contrib.parsers.xml_expat`** (breaking-ish import
  path move). The directory is now `contrib/parsers/xml_expat/` and the
  Aether import is `import contrib.parsers.xml_expat`, namespacing calls
  as `xml_expat.parser_new(...)` etc. Rationale: `contrib/xml/` mixed an
  *input* (parser) with the natural future home for an *output* (an XML
  emitter under `contrib/templating/native`'s sibling slot). Splitting by
  **direction-of-bytes** (`contrib/parsers/` vs `contrib/templating/`)
  keeps the threat models separate and prevents the "where does this
  format live?" cycle every time we add a new module. Downstream consumers
  must update three things: the dotted import path, the namespace prefix
  on call sites (`expat.foo` → `xml_expat.foo`), and the
  `extra_sources` path in `aether.toml`
  (`contrib/parsers/xml_expat/aether_xml_expat.c`). The
  `aether_xml_expat.c` filename and the `xml_expat_*` C-side externs are
  unchanged.

## [0.248.0]

### Added

- **`contrib.templating.native`: walking-skeleton drop.** Escape-correct
  HTML emission as plain Aether function calls over a `std.strbuilder`,
  no template DSL: `html_text(sb, s)` (HTML-escapes the five canonical
  entities), `html_raw(sb, s)` (verbatim, trusted markup), and tag
  emitters (`html_tag_open` / `html_tag_close` / `html_tag`). The
  escape rule lives in the *helper function*, not in template syntax —
  the template author physically cannot produce un-escaped user input
  in an attribute. This is the inverse pitch to
  `contrib.templating.liquid` (which is for operator-supplied
  templates); native is for "library author exposes template-shaped
  surface where users write Aether". The trailing-block builder DSL
  (`html { h1 { text(...) } }` with implicit `_ctx`, sketched in
  `docs/closures-and-builder-dsl.md`) is the eventual sugar; v0 stays
  with plain function calls so the escape contract is testable first.
  New module `contrib/templating/native/`, README, integration test
  `native_templating_skeleton/` (6 cases).

## [0.247.0]

### Added

- **Mutable module-level globals** (#701). A `var name[: type] = const-expr`
  at module scope declares one persistent, mutable word of state shared by
  every function in the module — a PRNG seed, a counter, a one-slot cache.
  It lowers to a file-scope C `static`; reads and writes from same-module
  functions are plain identifier access (no accessors, no indirection). The
  type annotation is optional (`var hits = 0` infers `int`); the initializer
  must be a compile-time constant (C requires it of a `static`). Globals are
  module-private and non-atomic, matching the C statics they replace. This
  deletes the vestigial C files that ports otherwise keep solely to hold a
  `static` plus get/set accessors (e.g. aedis's `rand.c`, `mt19937-64.c`).
  See [docs/language-reference.md](docs/language-reference.md) (Mutable
  module-level globals).

## [0.246.0]

### Fixed

- **Silent narrowing of inferred-int locals** (#698). A local declared
  with a bare 32-bit int initializer (`parsed = 0`) infers `int`; a later
  assignment of a 64-bit value (`parsed = some_long`) truncated to 32
  bits with no diagnostic — the generated C narrowing is an ordinary
  implicit conversion. This silently corrupted data (the aedis Redis
  port: `lpStringEqualsInt64` parsed a 64-bit field into an inferred
  `int`, so the equality never matched and `HGET` returned nil; 925 fuzz
  failures with #697, 0 after both). The compiler now diagnoses a
  narrowing assignment to an *inferred-type* int local, naming the
  variable and the fix (annotate `long`/`uint64` to keep precision, or
  `int` to make the narrowing deliberate). Explicitly-annotated locals
  are unaffected — the annotation is the user's choice. The inferred-vs-
  annotated distinction is tracked with a parser marker on the
  declaration node, so it survives the pre-typecheck inference pass.

## [0.245.0]

### Fixed

- **Int-shift sign-extension into 64-bit values** (#697). Shifting a
  32-bit-`int` operand (e.g. a `std.mem` byte accessor's result) and
  combining it into a `uint64`/`int64` expression — the canonical
  `u = u | (byte << 24)` binary-decode pattern — evaluated the shift in C
  `int`: for a byte >= 0x80 the sign bit overflowed and was sign-extended
  across the high 32 bits (`0x86000000` became `0xFFFFFFFF86000000`), and
  counts >= 32 were undefined behaviour. This silently corrupted binary
  codecs (the aedis Redis listpack/siphash port). The typechecker now
  propagates a 64-bit result context down into int arithmetic operands
  (left-shift and the value-combining `+ - * | & ^`; `>>` is excluded so
  arithmetic-shift signedness is preserved), and codegen casts narrow
  operands to the 64-bit type at the use site, so the value is built in
  64 bits. Pure-int arithmetic is unchanged. Covers the OR-combining and
  `uint64 x = byte << n` declaration forms.

## [0.244.0]

### Fixed

- **Windows `os.run` / `os.run_capture` / `os.run_supervised` couldn't
  spawn an argv-specified child** (#706). Two independent bugs in the
  Windows process-spawn path, both in `std/os/aether_os.c`:
  1. All three `CreateProcessW` call sites (`win_launch`, the supervised
     variant, and `os_run_full_raw`) passed the program path as
     `lpApplicationName`. Per MSDN, a non-NULL `lpApplicationName` must
     be a full path — `CreateProcessW` does no `execvp`-style PATH
     search, no `.exe` append, no built-in resolution. A bare `"cmd"`
     therefore became a literal CWD-relative filename, failed with
     `ERROR_FILE_NOT_FOUND`, and bubbled up as `rc=-1` /
     `err="spawn failed"`. Switched all three sites to
     `lpApplicationName=NULL` so `CreateProcessW` parses and PATH-
     resolves the first token of `lpCommandLine`.
  2. `build_command_line` then disagreed with the POSIX `build_argv_array`
     on the argv convention: POSIX seeds `argv[0]` with `prog` and treats
     `argv_list` as the *whole* argument vector; the Windows builder
     treated `argv_list[0]` as the program and dropped `prog`. So
     `os.run("cmd", ["/c","echo","x"])` — correct on Linux — produced
     the command line `/c echo x` on Windows and tried to spawn a program
     named `/c`. Re-aligned `build_command_line` to always seed with
     `prog` then append the whole `argv_list`. Together with the
     `lpApplicationName=NULL` flip the whole `os.run*` family now works
     on Windows. (`os.system()`, the shell-string path, was unaffected
     all along — it routes through libc `system()` not `CreateProcessW`.)
     Discriminator: literal-argv calls failed too, so this is distinct
     from the heap-string issue (#688) already fixed.

## [0.243.0]

### Fixed

- **Stale reference to deleted `runtime/memory/memory.c` in `tools/ae.c`**
  (#693). The file was removed in 39446f9 ("delete dead memory.c") on
  2026-05-12, but three references survived in `tools/ae.c` — two
  `snprintf` calls in the dev-tree no-lib branch (POSIX + Windows) and
  one entry in the source-manifest array. Invisible on most setups
  because `has_lib` is true (a built `libaether.a` is present), so the
  archive is linked and the raw sources are never compiled — but where
  the libaether-discovery probe misses (some MinGW layouts, fresh
  clones), gcc was handed the deleted path and the build failed with a
  confusing "no such file" pointing at the user's program. Cleaned up;
  the snprintf `%s`/`tc.root|src` arg balance was adjusted alongside.

## [0.242.0]

### Fixed

- **Spurious W1001 "unused variable" for fn-cast locals only used as
  callees.** `g = f as fn(ptr) -> int` followed by `g(p)` warned that
  `g` was unused: the call parses as `AST_FUNCTION_CALL` with the callee
  name in `node->value` (no `AST_IDENTIFIER` child), so the reference
  walk in the unused-variable analysis never saw it. The walk now also
  matches function-call names against tracked locals. Generated C was
  always correct (`((int(*)(void*))(g))(p)`); this was warning noise
  only. Regression: `tests/regression/test_fn_cast_call_marks_used.ae`.

## [0.241.0]

### Changed

- **Resource-cap accounting extended to `std.bytes`, `std.config`, and
  `std.io`** (#462, T2 caps audit), following the cap-gating pattern
  established in #343/#463. Plugin-reachable allocations now route through
  `aether_caps_malloc` so a sandboxed plugin can't allocate past the
  configured memory cap.
  - **Internal-lifetime** allocations (`std.bytes`' growable buffer +
    struct — most important, since its size is driven by a caller-chosen
    index; `std.config` entries/strings; `std.io`'s `FileInfo`) are
    drift-free: the matching `aether_caps_free` always knows the exact
    size (the bytes buffer's `capacity` field, a `dup_str`'d string's
    `strlen+1`, or `sizeof`), so the counter returns exactly to baseline
    after create + grow + free — locked in by a new
    `caps_bytes_accounting_balances` unit test.
  - **Caller-owned-return** allocations (`io.getenv`, errno messages) are
    cap-*gated* at allocation but freed via libc in Aether-land, so the
    counter drifts up on that cold path — the same documented,
    intentional trade-off as `file_read_all_raw` (#343). The drift is
    fail-safe: a plugin is denied once it would cross the cap, never
    allowed past it.
  - The fs/os `wchar_t`/path sweep is **intentionally not** part of this
    change: those are bounded-size, defense-in-depth, and largely
    Windows-only (untestable off-Windows), so the effort/risk outweighs
    the value now that the unbounded (#463) and attacker-inflatable
    (`std.bytes`) sites are covered.

## [0.240.0]

### Fixed

- **`os.run` / `os.run_capture` / `os.run_supervised` argv corruption for
  heap-allocated strings** (#688). The argv (and envp) builders read each
  list entry with `list_get_raw()` and blind-cast the result to `char*`.
  String literals and `aether_args_get()` returns happen to be raw
  `const char*`, so the cast lands on the payload; but heap-built
  strings (`string.concat`, `substring`, interpolation results, etc.)
  are magic-wrapped `AetherString*` whose first 16 bytes are the struct
  header, not the payload — execve(2) then received the header bytes,
  and the child process saw garbage like `\336\300W\256\1` in place of
  the intended argv. Routed every `list_get_raw` result destined for a
  `const char*` consumer through `aether_string_data()`, which unwraps
  the magic header and passes raw pointers through unchanged. Same fix
  applied to the sandbox grant-list reader (`aether_spawn_sandboxed.c`
  serialize_grants / is_fork_granted) and `fs.glob_multi`'s pattern
  list (`std/fs/aether_fs.c`), which had the same shape.

## [0.239.0]

### Added

- **`ae inspect <file.ae>`: operator-facing view of what a script
  declares** (#473). Prints the artifact shape (executable entry point
  vs library export surface), the capability posture (which `--with=`
  capabilities the gated imports require), the resolved import list
  (classified stdlib / contrib / local), and the top-level declarations
  (functions with signatures, structs, constants, actors) — scoped to
  what *this* file declares, excluding decls merged in from imported
  modules. Backed by a new `aetherc --emit=inspect` mode that walks the
  post-typecheck AST (no code generation), peer to `--emit=ast`. Makes
  the config-IS-code pitch concrete: operators can ask the compiler what
  their script declares without reading generated C.

## [0.238.0]

### Fixed

- **Native Windows (MinGW64): `${...}` string interpolation no longer
  prints empty** (#681). On MINGW64 the C runtime's printf family bound
  to the legacy MSVCRT implementations, which (a) make
  `vsnprintf(NULL, 0, …)` return `-1` instead of the would-be length —
  collapsing the two-pass sizing in the generated `_aether_interp`
  helper to a zero-byte buffer — and (b) mishandle the C99 conversions
  Aether emits (`%lld`/`%llu`/`%zu`/`%g`). Both the generated translation
  unit (as its first line, before any header) and the Makefile now define
  `__USE_MINGW_ANSI_STDIO=1`, binding the printf family to the
  C99-conformant `__mingw_*` implementations. No-op off MinGW.

### Added

- **`std.json`: terse value builder + encoder** (#628). The value
  constructors, object/array mutators, and serializer already existed but
  composing them was verbose; `json.obj()`, `json.arr()`, `json.str()`,
  `json.num()`, `json.boolean()`, `json.null_value()`, `json.set()`,
  `json.push()`, and `json.encode()` give the Go-style
  build-a-tree-then-encode shape so JSON responses no longer have to be
  hand-concatenated (with the attendant missing-comma / unescaped-quote
  hazards — escaping is handled by the encoder). Thin aliases over the
  existing API; the `create_*` / `object_set` / `array_add` / `stringify`
  names remain.

## [0.237.0]

### Added

- **`std.xml`: a pull/SAX reader and an escaping builder** (#627). S3,
  SOAP-ish, and config XML no longer need hand-rolled `index_of`-scanning
  parsers or string-concatenated response builders. The reader
  (`xml.parser` / `xml.next` → `EVENT_*`, with `xml.name` / `xml.text` /
  `xml.attr`) decodes the five predefined entities + numeric character
  references, handles attributes, self-closing tags, and CDATA, and skips
  the prolog / comments / PIs. The builder (`xml.writer`, `xml.start` /
  `xml.attribute` / `xml.text_node` / `xml.element` / `xml.end` /
  `xml.finish`) escapes text and attribute content so callers can't
  forget to. Deliberately omits XSD, XPath, namespaces, and DTD
  validation. New module `std/xml/` (C core + wrappers), regression test,
  and stdlib-reference docs.
## [0.236.0]

### Fixed

- **VS Code / Cursor extension: highlighting and the `.ae` file icon
  vanished after `install.sh`.** The installer copied the extension
  folder but then *removed* the extension from the editor's
  `extensions.json`, expecting the editor to re-scan the directory and
  re-register it on startup. Current VS Code / Cursor treat
  `extensions.json` as the authoritative registry and don't reliably
  re-scan for manually-dropped folders, so the extension sat
  present-but-unloaded — no syntax highlighting, no module icon. The
  installer now writes a proper `aether.aether-language` registration
  entry pointing at the installed folder (and still clears stale
  `aether*` rows and `.obsolete` keys). Fully quit and reopen the editor
  after installing.

## [0.235.0]

### Fixed

- **Two-string `string_*` externs with mixed argument representations.**
  `string.glob_match(pattern, s)` returned the wrong answer (a true
  match read as no-match) when one argument was a string literal and
  the other was a heap-allocated `AetherString` (e.g. the result of
  `string.substring`). `string.starts_with` / `string.ends_with`
  carried the same hazard for their `prefix` / `suffix` argument; so
  did `string.split` for its `delimiter`. Root cause: the call-site
  #297 auto-unwrap intentionally skips `string_*`-prefixed externs
  on the assumption that they handle both representations
  internally, but these particular implementations passed the raw
  pointer straight to `fnmatch(3)` / `strlen` / `memcmp`. When that
  pointer was actually a magic `AetherString*`, the C code read 16
  bytes of struct header as the first chars of the payload and
  produced wrong matches.
  - Fix: route the second string argument through `str_data` /
    `str_len`, the magic-aware accessors the first argument already
    used, so the impl behaves identically regardless of which side
    carries the wrapped representation
    (`std/string/aether_string.c`, `std/string/aether_string.h`).
  - Aether-side wrappers in `std/string/module.ae` are unchanged —
    the C-ABI shift to `const void*` for the affected parameters is
    a pointer-width-compatible widening and the generated forward
    declarations in user programs still compile and link.
  - Regression coverage in
    `tests/regression/test_string_extern_mixed_repr.ae`: every
    combination of (lit, heap) × (lit, heap) for `glob_match`,
    `glob_match_pathname`, `starts_with`, `ends_with`, `equals`,
    `contains`, `index_of`.

## [0.234.0]

### Added

- **`std.os` process supervision, `run_full`, `chdir`/`getcwd`; `std.signal`,
  and `ae run -- <args>` forwarding.** The notes for this release were
  reconstructed during the CHANGELOG drift reconciliation (see the
  Workflow note at the top of this file). The shipped content — verified
  against the `v0.234.0` tag — is the process-supervision primitive
  family (`os.run_supervised`, `os.kill`, `os.wait_pid_timeout`,
  `os.run_full`), the working-directory accessors (`os.chdir` /
  `os.getcwd`), the `std.signal` constant module, `ae run <file> --
  <args>` argument forwarding, the `examples/applications/build-supervisor.ae`
  demo, the full-suite macOS `leaks(1)` gate, and the CI-image
  pkg-config/libssl/zlib/pcre2 fix. The detailed per-entry text for
  these had been absorbed under `## [0.231.0]` by the release-pipeline
  drift; it remains there verbatim rather than being relocated, because
  the 0.231↔0.234 boundary for the `make ci-windows` Fixed entry could
  not be disambiguated with certainty and re-cutting it risked the exact
  mislabeling this reconciliation is correcting. This header records that
  0.234.0 shipped that body of work; the prose lives under 0.231.0.

## [0.233.0]

### Fixed

- **Diagnostics: parse errors in an imported module are now attributed
  to that module** (#646). A syntax error inside an imported module was
  labeled with the *importing* file's name and rendered a source snippet
  from the importing file (indexing the imported file's line number into
  the wrong buffer), pointing the caret at unrelated, valid code. The
  module parser now re-points the diagnostic source context (filename +
  buffer) at the module for the duration of its parse and restores the
  caller's context afterward, so the location header and snippet name the
  real file. Nested imports save/restore correctly, and the error/warning
  running totals are preserved across the swap.
- **Codegen: returning a struct from a tuple-returning function now
  compiles** (#634). A function whose tuple return embeds a user struct
  (e.g. `(Point, string)` — the idiomatic `(record, err)` shape) emitted
  the synthesised `_tuple_…` typedef *before* the struct body, so the
  generated C failed with `unknown type name 'Point'`. Tuple typedef
  emission is now deferred until after the user struct bodies are
  written (the return-type merge pre-scan still runs first); a tuple
  embeds each element by value, so the struct must be fully defined
  first.
- **Lexer: source columns are counted per character, not per byte**
  (#645). A multibyte UTF-8 codepoint (e.g. an em-dash `—`, 3 bytes)
  advanced the column counter by its byte count, so every token after a
  non-ASCII character on the same line reported a column offset by
  `bytes − chars` and diagnostic carets landed right of the real token.
  `advance()` now skips UTF-8 continuation bytes when advancing the
  column; tokenization is unchanged.
- **Closure capturing both a `ptr` and a `string`** is verified working
  and guarded by a regression test (#624). The mixed-capture case was
  resolved by the closure-environment rework in the 0.231.0 leak
  hardening; the test pins it so it cannot regress.

## [0.232.0]

### Added

- **Top-level `ae install`, `ae upgrade`, and `ae use` commands.** The
  version-management functionality was only reachable through the longer
  `ae version install` / `ae version use` forms, so the intuitive
  `ae install` / `ae upgrade` (the muscle-memory from rustup/nvm/pyenv)
  returned "Unknown command" — making the toolchain feel broken. Now:
  - `ae install [<version>]` installs a release into
    `~/.aether/versions/` (the latest release if no version is given).
  - `ae upgrade` (alias `ae update`) installs the latest release and
    switches to it — a no-op with a notice when already current.
  - `ae use <version>` switches the active version.

  These delegate to the existing install/switch logic; `ae version
  install <v>` / `ae version use <v>` are unchanged and equivalent.
  `ae help` lists the new top-level forms.

## [0.231.0]

### Fixed

- **`make ci-windows` example sweep now matches the canonical filter.**
  The Windows cross-compile target swept *every* `examples/**/*.ae`,
  including the intentionally-broken `ae-help-demo/broken_dsl.ae` (and the
  `host-*-demo.ae` files that need host runtimes), so step [1/3] aborted
  before ever MinGW-compiling anything. Aligned both `find` invocations
  with the main example-build sweep (`grep -v '/ae-help-demo/'` +
  `grep -v '/host-.*-demo\.ae$'`).
- **Memory-leak hardening across the compiler and stdlib.** Every
  detectable memory leak in the regression suite is eliminated (and the
  integration suite is clean under ASan + LSan), verified on macOS
  `leaks(1)` and Linux glibc ASan+LSan. The only remaining allowed
  entry is an OpenSSL process-global reclaimed at process exit — a proven
  `leaks(1)` false positive, not a real leak.
  - **String-ABI unification**: the plain-`char*` string ops
    (`concat`/`substring`/`to_upper`/`to_lower`/`trim`/`copy`, plus the
    length-bearing `concat_wrapped`) now return a magic refcounted
    `AetherString` adopted zero-copy, and the raw-cast sites (`char_at`,
    `==`/`!=`, `map.put` key, match-on-string) route through magic-aware
    accessors. A missed migration becomes a detectable leak, never a
    crash/UAF.
  - **Escape-analysis precision** (codegen): see-through read-only string
    accessors, an authoritative body-walk over the conservative heuristic
    for visible-body callees, self-reassignment is not an escape,
    discarded/void-call argument draining, a call-site pointer-identity
    drain for recursive accumulators, and recognising that invoking a
    closure parameter (`cb()`) does not escape it.
  - **Container ownership**: owned-puts adopt the caller's single
    reference (no double-retain); `map_put_raw` interns its key, so a
    heap key is the caller's to reclaim (the `@retain` was incorrect); a
    map *borrows* string values passed through a `string` parameter (the
    program owns and frees them — tagging a borrowed value owned would
    double-free it); a list owns a stored closure box **and** reclaims its
    captured environment.
  - **Closure-environment lifetime**: a transient capturing closure passed
    to a parameter that neither stores nor returns it (`run(cb){ cb() }`)
    has its heap environment freed after the call, gated on a proven
    non-escape so a stored/returned closure is untouched.
  - **`exit()` epilogue**: scope-exit defer-frees are emitted before a
    noreturn `exit(...)`, so programs ending in `exit(0)` reclaim their
    live heap locals instead of leaking them.
  - **stdlib**: `std.json` `object_set`/`array_add` reclaim the value on a
    type-mismatch error; `std.io` `fd_read_n`/`fd_read_line` buffers are
    `@heap` so callers reclaim them; `std.regex` `replace`/`replace_all`
    own their result; ~a dozen owned-heap stdlib returns (`path_*`,
    `os_*`, `io.getenv`, base64 encoders, …) are classified so callers
    reclaim them.

### Added

- **`std.os` process-supervision primitives** — the floor any build/test
  orchestrator hits, so a supervisor can be written in Aether instead of a
  bash job-control trampoline. Filed by aeb. **Cross-platform**: POSIX
  uses fork + setpgid + killpg + waitpid; Windows maps the same shape onto
  Job Objects, `WaitForSingleObject`, `TerminateProcess`, and a console
  control handler.
  - `os.run_supervised(prog, argv, env, new_process_group, forward_signals,
    timeout_secs, reap_group) -> (exit_code, outcome)` — one call that
    runs a child in its own process group, forwards interactive
    SIGINT/SIGTERM (Windows: Ctrl-C/Ctrl-Break) to that group, enforces a
    wall-clock timeout (POSIX TERM → 5s grace → KILL; Windows
    `TerminateJobObject`; reporting exit_code 124 the way GNU `timeout`
    does), and group-reaps anything the child leaked — a stray test
    server, a backgrounded helper (Windows:
    `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`). `outcome` ∈ {`"exited"`,
    `"signalled"`, `"timeout"`, `"error"`} (Windows never reports
    `"signalled"`). Options are explicit `int` flags (1/0) rather than a
    map — Aether maps hold `ptr` values, and explicit params match the
    rest of `std.os`.
  - `os.kill(pid, sig) -> int` — POSIX `kill(2)`. A negative pid signals
    the whole process group; `sig == 0` is the existence/permission probe
    (the `kill -0` reap-loop check). Previously there was no `kill` in the
    public surface at all. Windows: `sig == 0` probes via `OpenProcess`,
    any other signal forces `TerminateProcess` (no graceful SIGTERM),
    negative-pid group signalling returns -1.
  - `os.wait_pid_timeout(pid, secs) -> (status, timed_out, err)` — bounded
    wait on a child, so a timeout needs no separate watchdog process
    (`secs <= 0` blocks indefinitely). Windows: `OpenProcess` +
    `WaitForSingleObject`.
  - `os.run_full(prog, argv, env, stdin_data) -> (stdout, stderr,
    exit_code, err)` — the three-way child-stdio primitive `os.run_capture`
    leaves open: feeds `stdin_data` to the child (binary-safe — exactly its
    stored length, embedded NULs included) and captures stdout and stderr
    on **separate** channels with no pipe-buffer deadlock regardless of
    stream sizes (regression-tested with a 512 KB round-trip). POSIX pumps
    all three fds in one `poll(2)` loop; Windows drains stdout/stderr on
    reader threads while writing stdin. The `2>&1`-merged shape and
    binary-safe (length-bearing) capture of the *output* streams are out
    of scope for v1.
- **`os.chdir` / `os.getcwd`** — process working-directory accessors,
  previously absent entirely. `chdir(path) -> string` ("" on success,
  error message on failure); `getcwd() -> string` ("" on failure).
  Cross-platform (POSIX `chdir(2)`/`getcwd(3)`, Windows
  `SetCurrentDirectory`/`GetCurrentDirectory` with UTF-8↔UTF-16
  conversion). An orchestrator entry point needs these to run build
  steps relative to a project root.
- **`ae run <file.ae> -- <args>` now forwards `<args>` to the program.**
  Previously `ae run` dropped everything after the script name, so a
  config-is-code entry point couldn't receive arguments; now everything
  after a literal `--` reaches the program's `argv` (like `cargo run --`),
  each token double-quoted so a spaces-containing argument stays one
  token through the launcher. This is what makes
  `ae run supervisor.ae -- make -j8` work.
- **`examples/applications/build-supervisor.ae`** — a runnable
  build-supervision entry point that composes the new primitives
  (`os.getcwd` + `os.run_supervised`) into the exact shape of aeb's bash
  job-control trampoline (own process group, forward Ctrl-C, wall-clock
  timeout, group-reap of leaked children) — in one `run_supervised`
  call. Demonstrates the "config IS code" pattern: the file is the
  supervisor config and the entry point.
- **`std.signal`** — POSIX signal-number constants (`SIGHUP`, `SIGINT`,
  `SIGQUIT`, `SIGILL`, `SIGABRT`, `SIGFPE`, `SIGKILL`, `SIGSEGV`,
  `SIGPIPE`, `SIGALRM`, `SIGTERM`) as zero-arg functions, so callers pass
  `signal.SIGTERM()` to `os.kill` instead of hard-coding 15. Limited to
  the signals whose numbers are stable across every POSIX target;
  platform-divergent ones (SIGUSR1/2 etc.) are intentionally omitted.
  (On Windows most have no effect — `os.kill` forces termination
  regardless of the number.)
- **Full-suite macOS `leaks(1)` gate** (`tests/run_macos_leaks.sh`,
  `tests/leaks_known.txt`, `tests/leaks_exclude.txt`): the gate now sweeps
  the entire `tests/regression` suite (previously 16 curated programs),
  with an audited per-test allow-list (only the OpenSSL artifact) and a
  subprocess-spawning exclude-list (those are leak-checked on Linux CI,
  where `leaks(1)` would hang on the forked child). A test absent from the
  allow-list must report zero, so a new leak fails the gate locally
  instead of slipping to CI.

### Changed

- **CI image** (`docker/Dockerfile.ci`): install `pkg-config`,
  `libssl-dev`, `zlib1g-dev`, and `libpcre2-dev` so `make docker-ci`
  actually links and exercises crypto / compression / regex — previously
  those modules compiled to stubs (`pkg-config --libs` resolved empty),
  so the base64/hash/regex tests silently failed with "openssl
  unavailable".
- **No-leak RSS probes** (`heap_tracker_return_escape_no_leak`,
  `bytes_finish_return_no_leak`): measure current resident RSS (mach
  `task_info` / `/proc/self/statm`) instead of `getrusage`'s monotonic
  peak `ru_maxrss`, which captured transient allocator peaks under churn
  and flaked the bound; current RSS drops when memory is freed.
**Workflow**: New changes go under `## [0.230.0]`. When a PR merges to
`main`, the release pipeline automatically replaces `[current]` with the
next version number before tagging the release.

## [0.230.0]

### Fixed

- **`spawn_sandboxed`: kernel-level fence for `clone3`/`vfork` —
  `fork:*` deny is no longer a paper tiger against modern toolchains**
  (`runtime/aether_spawn_sandboxed.c`,
  `tests/integration/sandbox_clone_fence/`, issue #668). The LD_PRELOAD
  libc-symbol fence on `fork`/`vfork`/`clone`/`clone3` was bypassed by
  any caller that issues the underlying syscall directly — including
  glibc's own `__vfork` (an inline `syscall` instruction with no libc
  symbol indirection at all) and any program calling
  `syscall(SYS_clone3, …)` via raw asm. Modern gcc on Linux uses both
  paths to spawn `cc1`/`as`/`ld`, so before this fix a
  `spawn_sandboxed` call denying `fork` happily let gcc spawn its full
  toolchain anyway. `spawn_sandboxed` now installs a seccomp-bpf
  filter on the child side (post-fork, pre-exec) that traps
  clone/clone3/fork/vfork with `EPERM` regardless of how they're
  invoked, when `fork:*` is not in the grant list. Kernel-level
  enforcement, immune to call-site games. Uses
  `prctl(PR_SET_NO_NEW_PRIVS, 1)` + `prctl(PR_SET_SECCOMP,
  SECCOMP_MODE_FILTER, …)` — required Linux ≥ 3.5 (already an
  effective constraint for the preload). x86_64-only filter; on other
  archs the BPF program falls through to ALLOW so the existing
  libc-level fence remains the only fence there (documented gap).
  Fail-closed: if the seccomp setup fails (kernel too old, etc.) the
  child aborts with `exit(126)` and a diagnostic — a `spawn_sandboxed`
  call that asked for containment we can't deliver must not silently
  exec. Existing `tests/integration/sandbox_toolchain/` continues to
  pass because it grants `fork:*` (toolchain builds intentionally
  spawn cc1/as/ld); the new `sandbox_clone_fence/` covers the
  no-fork-grant case with two probe binaries — one using libc
  `vfork()` (inline syscall) and one using `syscall(SYS_clone3, …)` —
  both denied without the grant, libc `vfork()` allowed when the
  grant is present. Closes #668; doc updated in
  `docs/containment-sandbox.md`.

## [0.229.0]

### Fixed

- **`spawn_sandboxed` no longer corrupts the parent stack when the
  contained child uses `vfork()`** (`runtime/libaether_sandbox_preload.c`,
  `tests/integration/sandbox_toolchain/`). Removed the LD_PRELOAD `vfork()`
  wrapper: glibc's `vfork()` has a strict contract (the child must call
  only `_exit()` or `execve()`, and the calling function must not
  return) because vfork shares the parent's stack until the child execs.
  Wrapping the symbol in a regular C function violates that — the child
  returns *through our wrapper frame* on the shared stack, corrupting
  the parent. The visible effect was a sandboxed child running the
  `ae`/`aetherc` → `gcc` → `cc1`/`as`/`ld` toolchain failing (`Build
  failed`, `rc=-1`) — and, for a deeper aeb orchestrator child, segfaults
  in the parent after the `wait4`. The `fork()` and `execve()`
  interceptors continue to gate process creation (`fork()` is safe to
  wrap; the dangerous follow-up is the `execve` which we still see); a
  bare `vfork()+_exit()` was never the threat model. Reported by aeb as
  the upstream blocker for `aeb --sandbox <target>`. New regression test
  exercises the full `ae build` toolchain under `spawn_sandboxed`;
  verified pre-fix → FAIL, post-fix → PASS.

## [0.228.0]

### Added

- **`contrib/tinyweb` ships with `make contrib` / `make
  install-contrib`** (`Makefile`, `tests/scripts/contrib_build.sh`).
  Tinyweb is a DSL-style HTTP server library (port of
  [Tiny](https://github.com/phamm/tiny)) covering routing /
  filters / WebSocket (RFC 6455 handshake) / SSE / static files /
  cookies — see `contrib/tinyweb/README.md` for the surface. It was
  always present in the repo but explicitly excluded from
  `make install-contrib` (the `rm -rf .../contrib/tinyweb` trim) and
  from the `contrib_build.sh` catalogue. Now wired up:
  `probe_tinyweb` is a trivial always-succeed (libc only — SHA-1 +
  base64 for the WebSocket-Accept handshake live in
  `ws_handshake.c`); catalogue entry compiles that one .c into
  `libaether_tinyweb.a`; install path mirrors `module.ae` +
  `README.md` + `ws_client.ae` (client utility) into
  `share/aether/contrib/tinyweb/`. `example_*.ae` and `test_*.ae`
  are trimmed by the standard filter set (the latter now extended
  to cover `.ae` test files, not just `.sh` drivers — tinyweb is
  the only contrib module that ships `test_*.ae`).

  Default `make contrib` (no `MODULES=` filter) now builds 9
  modules instead of 8.

- **`aetherc --emit=ast` carries literal call arguments**
  (`compiler/aetherc.c`,
  `tests/integration/aetherc_emit_ast/`). Follow-up to v0.226.0's
  `--emit=ast` (aeb's supply-chain veto consumer asked for this in
  `veto-emit-ast-args.md`). Two changes:
  (a) The emit short-circuit now runs **after** `optimize_ast`
  rather than before it, so const-foldable numeric args
  (`do_stuff(1 + 2)` → `do_stuff(3)`) arrive as literals on the
  wire. String-literal `+` concat isn't valid Aether (the parser
  rejects it), so the practical impact today is numeric folds;
  future fold passes for `string.concat(lit, lit)` would be
  picked up automatically.
  (b) Each `function_call` node now carries an `args[]` array:
  one entry per positional/named arg in source order, with
  `{"literal":"<value>"}` for primitive literals
  (string/int/int64/float/bool — including `AST_NAMED_ARG`
  unwrap to the value side) and `{"computed":true}` for
  anything else (identifier refs, expressions, function calls,
  interpolations). Trailing closures / DSL builder bodies are
  filtered out — they're not args in the veto sense. The array
  is always emitted (even empty) so aeb's policy walker can
  rely on `obj["args"]` being a defined shape.

  Per the design doc, aeb fail-closes on any `{"computed":true}`
  arg regardless of contents; no expression-tree provenance is
  surfaced — the literal-vs-not distinction is the entire
  requirement, keeping `--emit=ast` from drifting toward a
  dataflow/taint analyzer. 7 new cells in
  `tests/integration/aetherc_emit_ast/` cover string/int/multi
  literals, identifier-ref-is-computed, mixed literal+computed
  in source order, the const-fold case, and the empty-args edge.

## [0.227.0]

### Added

- **`aetherc --emit=ast` — post-typecheck AST as JSON for aeb's
  supply-chain veto** (`compiler/aetherc.c`,
  `tests/integration/aetherc_emit_ast/`). A third post-typecheck
  walker beside `--list-functions` and `--diagnose=ownership`,
  emitting the program's AST in a small filter set
  (`import_statement`, `extern_function`, `function_call`) as
  stable JSON on stdout. The compiler holds zero veto policy; it
  just hands aeb the AST it already built. aeb's `lib/veto`
  consumes this and applies operator-owned deny rules
  (`deny extern` / `deny exec` / `deny net` / `deny import`)
  against an operator policy `.ae`. Designed in
  `veto-enhancements.md` (cross-repo note from the aeb-side
  Claude), greenlit in `veto-enhancements-reply2.md`.

  Per-node fields: `kind`, `file` (source-of-origin, NULL for
  compiler-synthesised nodes), `line`. For
  `function_call`: `callee` is the resolved binding (post-merge,
  post-alias-resolution canonicalisation) — `import std.os` plus
  `os.system(...)` emits `callee: "os_system"`, defeating the
  alias-spelling tricks a regex would miss. Indirect calls
  (function-pointer dispatch, no static target) emit
  `indirect: true` instead of `callee`; aeb fail-closes on those.
  For `extern_function`: `name` plus `variadic: true` flag on
  varargs externs. For `import_statement`: `module` path, plus
  `alias` (for `import X as Y`), `selected[]` (for `import X (a,
  b)`), `glob: true` (for `import X (*)`).

  Exit codes are conventional: 0 = AST emitted OK, non-zero =
  parse / typecheck / IO failure. aeb's `--vet` treats non-zero
  as fail-closed (no AST ⇒ no veto-clearance ⇒ no build).

  20 cells in `tests/integration/aetherc_emit_ast/` cover the
  qualified-call canonicalisation case, selective imports
  (`selected[]` + bare-call canonicalisation), glob imports,
  variadic extern flag, per-node file/line, and the fail-closed
  exit on parse and typecheck errors.

## [0.226.0]

### Added

- **`contrib.templating.liquid` — sandbox gate verification**
  (`tests/integration/liquid_sandbox_gate/`,
  `contrib/templating/liquid/README.md` correction). Pure shell-driver
  test that asserts the `--emit=lib` capability gate covers the
  Liquid module transitively. Cases: (1) `--emit=lib` without
  `--with=fs` rejects `import contrib.templating.liquid` with the
  standard capability-empty error (because the module imports
  `std.fs` for `{% include %}` / `parse_file`); (2) `--emit=lib
  --with=fs` accepts the import; (3) direct `import std.fs` without
  `--with=fs` still rejected (control); (4) `--emit=exe` (default)
  accepts the import — only `--emit=lib` applies the gate. The
  Liquid README earlier claimed the gate was "not yet wired up";
  this PR's investigation showed the gate IS transitive and works
  correctly, so the README is corrected and pinned with a test.

## [0.225.0]

### Added

- **`contrib.templating.liquid` — filter polish + README + LLM.md
  pointer** (`contrib/templating/liquid/module.ae`,
  `contrib/templating/liquid/README.md`,
  `tests/integration/liquid_filter_polish/`, `LLM.md`). Three small
  slices:
  (a) **`truncate: N, "ellipsis"`** — second arg overrides the
  default `"..."` ellipsis (`truncate:"10","…"` → 7 body bytes + the
  3-byte UTF-8 ellipsis). 1-arg form keeps its existing behaviour;
  empty-string ellipsis is accepted (= hard cut). When `N <=
  length(ellipsis)` the ellipsis itself is cut to fit.
  (b) **`default: VAL, allow_false:"true"`** — Shopify-compat
  widening of the fallback set. Without `allow_false`, fires only
  on the empty string (existing). With it, fires on `""`,
  `"false"`, `"nil"`, `"null"` (the string-encoded Liquid falsy
  set). Recognises both the bare `default:"alt","true"` 2-arg form
  and the keyword-prefixed `default:"alt",allow_false:"true"` form.
  (c) **`contrib/templating/liquid/README.md`** — first user-facing
  doc. Covers install, supported tags / filters / typed bindings /
  layouts, public surface, sandbox story, what's NOT supported,
  performance notes. Closes the "is this ready for downstream"
  documentation gap.
  (d) **`LLM.md`** gets a one-line idiom: "Operator-supplied Liquid
  templates → `contrib.templating.liquid`. Don't hand-roll
  Mustache." 15 new integration tests in `liquid_filter_polish/`
  bring the Liquid suite to **312 across 22 directories**.

## [0.224.0]

This entry is backfilled — the release pipeline tagged 0.224.0
against an empty `[current]` section, so the substantive work
landed via PR #648 had no CHANGELOG entry at the time. Reconstructed
from the commit messages of `perf/windows-ci-and-leak-tail`.

### Fixed

- **`std.collections.string_list` owns copies of its elements** (#622
  follow-up; `std/collections/aether_stringlist.c`,
  `std/collections/module.ae`). Previously the list stored the caller's
  raw pointer with `string_retain` / `string_release` for lifetime —
  but those are no-ops on a non-magic `char*`, so every plain-`char*`
  element (a `string.concat` / `split` result) was never reclaimed:
  the list couldn't free it and the caller's free was suppressed by
  the `@retain` annotation. The list now owns an independent
  NUL-terminated copy of each element (`sl_dup_item`, caps-accounted)
  and frees its own copies on `string_list_free`.

- **`std.fs` / `std.io` / `std.os` owned-heap string returns now
  classified for auto-free** (`compiler/codegen/codegen_stmt.c`).
  `@heap` only parses on tuple positions, so these single-value
  `-> string` externs were missing from the by-name heap classifier
  and the caller never freed the result. Added: `path_clean`,
  `path_rel`, `io_read_file_raw`, `file_read_all_raw`, `os_exec_raw`,
  `os_run_capture_raw`. Leak counts went 10→0 (lexical paths),
  5→0 (file io), 4→0 (fs_move/copy), 3→0 (atomic rename).

- **`std.ulid` / `std.ksuid` scratch buffer leaks** (`std/ulid/module.ae`,
  `std/ksuid/module.ae`). The id generators allocated intermediate
  `bytes` buffers that were only read from (never handed to
  `bytes.finish`) and so were never reclaimed — a leak per generated
  id. Both now `bytes.free()` their intermediates. Test leaks 6→0.

- **`test_string_seq_combinators` leak tail** (`tests/regression/
  test_string_seq_combinators.ae`). Test created seqs that weren't
  freed and passed `split_to_seq` results inline to `seq_concat` with
  no owning local to reclaim. Added explicit frees and captured the
  inline operands. 67→32 leaks; residual 32 are the
  StringBuilder-finish ownership-through-wrapper-chain gap, under
  investigation. Also classified `aether_strbuilder_finish` as
  heap-returning. Rewrote a recursive `string.concat` accumulator
  as an iterative `std.strbuilder` join (amortised-O(1)).

- **`test_string_format` arg-list leaks** (`tests/regression/
  test_string_format.ae`). Format-args `std.list` instances `a1..a5`
  were never freed. Added `list_free` for each. 13→2 leaks; residual
  2 are a `from_int` value stored into a generic borrowing
  `std.list` (escape analysis clears its heap flag at the `list_add`
  sink) — the borrowed-container-holds-a-heap-value class, tracked
  with the other container-ownership items.

### Changed

- **Windows CI tuning** (`.github/workflows/ci.yml`). Reverted an
  oversubscription attempt that made the Windows job slower (NPROC=8
  thrashed the 4-vCPU runner's RAM during the heavy
  openssl/nghttp2/zlib/pcre2 statically-linked builds — one link
  ballooned to ~11 min). Use the runner default (nproc=4) and raise
  the timeout backstop to 45 min (above the natural ~32-36 min
  runtime, fires only on a real hang). Kept the Defender real-time
  scan exclusion. Root cause for remaining slowness: `ae build`
  links the full TLS/HTTP stack into every test exe unconditionally;
  cutting that needs conditional per-test linking or `lld`, tracked
  as a follow-up.

## [0.223.0]

### Added

- **`contrib.templating.liquid` — Phase 1 typed value model (scalars)**
  (`contrib/templating/liquid/module.ae`,
  `tests/integration/liquid_value_basics/`). First step toward
  replacing the string-only context with a typed value model. New
  public setters `context_put_int(ctx, key, n)`,
  `context_put_float(ctx, key, f)`, `context_put_bool(ctx, key, b)`,
  `context_put_nil(ctx, key)` bind typed values under an internal
  `v:<key>` namespace; the legacy `lookup` reads `v:<key>` first
  (stringified via `value_to_string`) so `{{ x }}` interpolation works
  transparently against typed bindings without touching any existing
  filter or tag code. A typed binding shadows a same-named legacy
  string binding regardless of bind order. Also exports a small set
  of constructors (`value_nil`, `value_bool`, `value_int`,
  `value_float`, `value_str`), inspectors (`value_kind`,
  `value_payload`, `value_get_int`, `value_get_float`), and the
  canonical stringifier (`value_to_string`) for downstream code that
  wants to build / inspect packed values directly. **Phase 1 is
  scalar-only** — array and object bindings (`context_put_array` /
  `_object`) and the corresponding `items[0]` / `user.name` path
  walker are deferred to Phase 2 (need a heap-retention story for
  the std.list backing storage that survives across the
  `mem.ptr_to_long` round-trip in the packed-value encoding). 13 new
  integration tests cover int / float / bool / nil round-trip,
  typed-shadows-legacy ordering, negative + zero ints, and the
  `value_to_string` round-trip on each kind.

## [0.222.0]

### Fixed

- **`contrib.templating.liquid` — restored engine code missing from
  PR #651** (`contrib/templating/liquid/module.ae`). PR #651 shipped
  without the bulk of its Liquid engine implementation (~2.5k lines:
  the parser/renderer body, plus the `liquid_assign`, `liquid_filters_basic`,
  and `liquid_tags_simple` integration probes were left referencing
  functions that hadn't landed). This release restores the full engine
  body so those tests pass against real code. Committed `[skip actions]`,
  which is why no `## [0.222.0]` header was cut at the time; this section
  records the release. (The `git stash`/`pop` hazard that dropped the
  original 2400-line body is the cautionary case behind the
  "verify staged diff before commit" rule.)

## [0.221.0]

### Added

- **`contrib.templating.liquid` — Shopify Liquid templating engine v0**
  (`contrib/templating/liquid/module.ae`,
  `tests/integration/liquid_basics/`,
  `tests/integration/liquid_tags_simple/`). A pure-Aether port of the
  [Liquid template language](https://shopify.github.io/liquid/),
  sandbox-friendly (no reflection, no eval, explicit filter set).
  Opt-in `contrib/` module — not in `libaether.a`. Initial vertical
  slice: text passthrough, `{{ name }}` single-segment variable lookup
  against a string-keyed context, `{% comment %}...{% endcomment %}`
  (body suppressed), `{% raw %}...{% endraw %}` (body emitted verbatim
  with no interpolation), and a render-time error for any unsupported
  `{% xxx %}` tag. Public surface: `parse_string`, `context_new`,
  `context_put_string`, `context_free`, `render`. 15 integration tests
  across two directories cover the implemented behaviour; future tags
  (`if`/`for`/`assign`/...), filters, dotted attribute access,
  includes, layouts, and the JSON value model are tracked in
  `TODO.md` and will land with their own tests as each layer is built.

- **`contrib.templating.liquid` — string-only filter pipeline + `{% assign %}`**
  (`contrib/templating/liquid/module.ae`,
  `tests/integration/liquid_filters_basic/`,
  `tests/integration/liquid_assign/`). OUTPUT bodies now accept
  `expr | filter[:arg[,arg]*]` chains. Implemented filters
  (string-in/string-out): no-arg `upcase`, `downcase`, `capitalize`,
  `strip`, `lstrip`, `rstrip`, `reverse`, `size`; 1-arg `append:"s"`,
  `prepend:"s"`, `default:"s"`, `truncate:"N"`; 2-arg
  `replace:"old","new"`. Both `'…'` and `"…"` quote styles work; a
  literal `|` or `,` inside a quoted arg does not split the pipeline.
  `{% assign NAME = EXPR %}` binds a name in the current context;
  EXPR is a string literal OR a variable lookup, optionally followed
  by a filter chain (`{% assign loud = name | upcase | append:"!" %}`).
  Unknown filter / bad arg count / malformed `assign` all surface as
  render-time errors. 26 additional integration tests (16 filter
  cases, 10 assign cases).

## [0.220.0]

### Added

- **Deterministic `*StringSeq` ownership** (`compiler/codegen/codegen.c`,
  `codegen_stmt.c`, `codegen_func.c`). Cons-cell sequence locals are now
  reclaimed automatically — freed on reassignment and at scope exit via
  `string_seq_free` — mirroring the heap-string ownership system. Because
  `string_seq_free` is a refcount *decrement*, this is safe even for
  structurally-shared spines (the documented `cons(x,t)`/`cons(y,t)`
  sharing contract is preserved; verified by `string_seq_shared_tail`).
  An owning-producer classifier distinguishes `cons`/`reverse`/`concat`/
  `take`/`drop`/`from_array`/`split_to_seq`/`retain`/`empty` (owned) from
  `string_seq_tail` (borrowed). `test_string_seq`: 5003 leaks → 0.

### Fixed

- **aetherc double-free crash on the `*StringSeq` tracking fields**
  (`compiler/codegen/codegen.c`). The `CodeGenerator` is field-initialised
  (not `calloc`'d); the new seq registry fields were left uninitialised,
  so the first `clear_seq_vars()` freed garbage pointers — a glibc abort
  on Linux (`aetherc` SIGABRT/SIGSEGV; 335 programs "Compilation failed")
  that macOS's allocator silently tolerated. Found with an
  AddressSanitizer-instrumented `aetherc`; root-caused to a missing field
  init.
- **`defer string.release(X)` double-free / use-after-free on magic
  AetherStrings** (`compiler/codegen/codegen_expr.c`). `string.from_int`/
  `from_long`/`new` return a refcounted AetherString that the heap-string
  tracker manages, so the value was freed twice — once by the explicit
  `string_release`, once by the function-exit defer-free. `string.release(X)`
  now lowers through the same flag-guarded form as the bare `release()`
  builtin for any heap-tracked local, freeing once and clearing the
  tracker. Verified leak-free under AddressSanitizer.
- **Heap-string leaks from `release()`, `print`/`println` arguments, and
  `==`/`!=` operands** (`compiler/codegen/codegen_expr.c`,
  `codegen_stmt.c`). `release()` now reclaims plain malloc'd `char*`
  results (string.concat/substring/to_upper/to_lower/trim) via a
  flag-guarded free; the print family and `release`/`string_release` no
  longer mark their argument escaped (they provably never retain it), so
  loop accumulators printed/released each iteration are reclaimed; and a
  heap-returning string expression used directly as a comparison operand
  is drained after the compare (the binary-operator analogue of the
  call-argument drain). `test_release_builtin` 102→0,
  `test_string_leak_loop` 1000→0, `test_fs_realpath` 30→0.
- **macOS `leaks(1)` gate could hang the CI job indefinitely**
  (`tests/run_macos_leaks.sh`). Two compounding causes: (1) no per-step
  timeout, and (2) the leaks output was captured with a `$(...)` pipe — a
  `leaks --atExit` run whose target program didn't exit cleanly orphaned
  that program still holding the pipe's write end, so the command
  substitution blocked **even if `leaks` was killed**. Fixed by capturing
  to a file (no orphan-reader dependency) and bounding every `ae build` +
  `leaks` step with a perl `alarm` wrapper that kills the whole **process
  group** (so `leaks` and its target die together), plus a `pkill` reap
  of any straggler and a 20-minute step cap in CI. A timed-out step now
  fails fast as `[FAIL] <test>: ... TIMED OUT`, and a per-test progress
  line makes the in-flight test visible in the live log. The gate can no
  longer hold a runner.
- **Regression tests leaked the containers they allocated** — added the
  missing explicit container frees (the codebase convention is explicit
  container ownership; only heap-string and `*StringSeq` *locals*
  auto-free). `test_json_object_iter` (`json.free` ×7, 16→0),
  `test_list_add_heap_alias_escape` (`list.free` ×3, 16→0),
  `test_destructure_reassign_over_heap_lhs` (`map.free` ×4, 23→0). Each
  verified leak-free and double-free-free under AddressSanitizer.
- **`std.url` working-buffer leak on the decode error paths**
  (`std/url/module.ae`). `url.decode` allocated a `bytes` working buffer
  and freed it only on the success path (via `bytes.finish`); the two
  malformed-percent-encoding error returns leaked it. Both now
  `bytes.free(buf)` before returning. `test_url` also gained the missing
  `string_list_free` calls for the lists returned by `url.parse_query` /
  `url.query_get_all` (caller-owned, per the explicit-container-ownership
  convention) — together taking `test_url` from 21 leaks to 10. The
  residual 10 are plain-`char*` query entries stored in a refcounted
  `string_list` (a `@retain` API): `string.concat` / `string.substring_n`
  return plain `malloc`'d `char*` that `string_retain` / `string_release`
  no-op on, so the list can't reclaim them — the same string-ABI boundary
  tracked for the dedicated magic-tagging follow-up, not closed here to
  avoid an unverified refcount-contract change.
- **`test_fs_positional_io` failed the Windows CI job** (`tests/
  regression/test_fs_positional_io.ae`). The #640 positional-I/O test
  hardcoded `path = "/tmp/aether_test_pio.bin"`; on Windows a bare
  `/tmp` resolves through `fopen` to `C:\tmp` (not created), so
  `fs.open(path, "w+")` failed and the whole CI run went red. Switched
  to the portable scratch-dir idiom the other `std.fs` tests already use
  (`io.getenv("TMPDIR")` → `"TEMP"` → `/tmp` fallback). The positional
  ops themselves were already Windows-correct (`fseek`+`fwrite`/`fread`,
  `_chsize_s`, `_commit`); only the path was wrong.

### Documentation

- **`string.array_get` borrow-warning now points at deterministic
  `*StringSeq`** (`std/string/module.ae`, `std/string/aether_string.c`).
  Following review feedback on the interaction with the #635 lifetime
  warning: the "make an owned copy" guidance now names `string_split_to_seq`
  / `*StringSeq` as the recommended shape, reclaimed *deterministically*
  by the compiler (auto-freed at scope exit, sharing-safe) now that this
  release lands that ownership — so escaping a split piece no longer risks
  dangling into a freed backing array.

## [0.219.0]

### Added

- **`std.fs.clean` / `is_within_base` / `rel` — lexical path ops** (#632).
  Pure-string path operations matching Go `filepath.Clean` / `Rel`
  semantics. `is_within_base` is the **security-critical** primitive
  every blob store / static-asset server / archive extractor needs to
  reject `../../etc/passwd` BEFORE `open(2)` — symlinks NOT followed
  (open-time concern, separate from this check). Implementation uses
  a segment stack to resolve `..` against the parent set, preserving
  unresolved leading `..` on relative paths (so `../../x` stays
  `../../x`). Closes the gap fbs-core hand-wrote 250 lines for in
  `lib/pathutil`.

- **`std.fs.pwrite` / `pread` / `ftruncate` / `fsync` — positional I/O**
  (#640). The POSIX positional family for out-of-order writers (rsync /
  zsync reconstruction, parallel chunk downloads, sparse-file build).
  Wraps `pwrite(2)` / `pread(2)` / `ftruncate(2)` / `fsync(2)` against
  the fd behind a `file_open_raw(path, "r+"/"w+")` handle. Loops on
  short transfers (POSIX permits returning fewer bytes than asked);
  EINTR-safe. Windows: `_chsize_s` / `_commit` + seek+write emulation
  (not thread-safe across handles, fine for single-threaded port
  servers). `pread` uses the same `(bytes, length, err)` tuple shape
  as `fs.read_binary` so destructuring stays consistent. Removes the
  zsync port's need for a sidecar `.c` of uniquely-named wrappers
  (Aether's compiler emits a clashing `pwrite` prototype if you
  `extern pwrite(...)` directly).

- **`std.http.request_body_read` — chunked-read request body** (#626,
  half). The fbs-core port asked for streaming HTTP body access; this
  ships the **API shape** — `request_body_read(req, offset, max) ->
  (bytes, n, err)` — over the already-buffered body. Lets porters
  write the chunked-iteration loop they want today, so their code is
  ready for the day the parse-loop reshape (handler runs before body
  finishes arriving) lands. The RAM-bounded internals are an open
  follow-up; the API surface is stable. The **other half** of #626
  (zero-copy response file serving) already shipped earlier as
  `http.serve_file(res, path)` — uses `sendfile(2)` when eligible
  (cleartext + HTTP/1.1 + no Range). Issue closed in full once the
  porter is pointed at the existing `serve_file` for downloads.

- **`std.cryptography.random_bytes`** (moved from `std.crypto`). The
  CSPRNG primitive now lives in `std.cryptography` alongside the
  hashes/HMAC/Base64 surface — single namespace for all
  cryptographic primitives. Callers update
  `import std.crypto` → `import std.cryptography` and
  `crypto.random_bytes` → `cryptography.random_bytes`. The previous
  `std.crypto` module ships in v0.218.0 only; deleted here.

### Changed

- **HTTP `serve_static` honours the `Range` request header** (#641).
  Pre-fix, `http.serve_static` / `http.serve_file` always responded
  `200 OK` with the entire file regardless of `Range` — the only
  place the server consulted `Range` was `sendfile_eligible()`, which
  used it to DISABLE the zero-copy fast path while still serializing
  the whole file on the buffered path. This blocked serving zsync
  downloads, resumable downloads, and media seeking.
  Now:
    - well-formed `Range: bytes=START-END` (or `bytes=START-`,
      `bytes=-SUFFIX`) → `206 Partial Content` + `Content-Range:
      bytes start-end/total` + `Content-Length: <slice>`
    - no Range header → 200 + `Accept-Ranges: bytes` (advertises
      slicing is supported for follow-up requests)
    - unsatisfiable / malformed range → `416 Range Not Satisfiable` +
      `Content-Range: bytes */total`
    - multi-range (`bytes=0-1,5-6` / multipart/byteranges) explicitly
      out of scope for v1 — single range covers zsync, resumable
      downloads, media seeking
  The sendfile fast path stays disabled for Range requests (the
  existing `sendfile_eligible` behaviour); the buffered path now
  honours the offset+length.

### Docs

- **LLM.md — `contrib/` directory called out + XML pointer added**.
  PR #638 closed five stdlib gaps that the fbs-core port reported, but
  the porter also hand-wrote XML parsing (#627) because LLM.md
  surfaced `contrib/host/*` and `contrib/aether_ui/` but never named
  `contrib/` as a place to look for stdlib-shaped modules.
  Added: a `contrib/` bullet under "Files/dirs worth knowing" with
  the always-check-contrib rule + a roster, and a specific XML idiom
  under "Idioms that keep biting" so a porter searching for "xml" in
  LLM.md finds `contrib.xml.expat`.

## [0.218.0]

### Added

- **`std.cryptography.hmac_sha256_hex` + `hmac_sha256_bytes`** — HMAC-SHA256
  (RFC 2104 / FIPS 198-1), thin veneer over libcrypto's `HMAC()`. Closes
  #631. Both shapes: hex-string for opaque token signing, raw-bytes for
  chained key derivation (SigV4, HKDF) where each round's output keys the
  next and round-tripping through hex would be wasteful. RFC 4231 test
  vectors verified in `tests/regression/test_cryptography_hmac_md4_md5.ae`.
  Closes "HMAC documented as out-of-scope" in std.cryptography header — it
  was a meaningful gap for any AWS-style auth port.

- **`std.cryptography.md4_hex` + `md4_bytes`** — MD4 digest with binary
  output (#637). Required for zsync's per-block strong checksum. Bound to
  `EVP_md4()` directly, not `EVP_get_digestbyname("md4")`, because on
  OpenSSL 3 MD4 lives in the legacy provider and the by-name dispatcher
  doesn't consult it. The implementation lazily loads BOTH the default and
  legacy providers on first MD4 use — loading just `legacy` would silently
  unload the default and break MD5/SHA-*. Documented as legacy (no
  collision resistance) in the wrapper docstring.

- **`std.cryptography.md5_hex` + `md5_bytes`** — MD5 (#631) as a documented
  first-class wrapper. Was previously reachable via
  `hash_hex("md5", ...)` (a "lucky workaround" per the issue); now blessed
  with its own wrapper for Content-MD5 / ETag interop. Documented as legacy.

- **`std.cryptography.sha1_bytes` / `sha256_bytes` / `md5_bytes` /
  `md4_bytes` / `hash_bytes`** — length-aware binary (raw-bytes) digest
  output across the algorithms (#637). Same `(bytes, length, err)` tuple
  shape as `base64_decode` and `fs.read_binary` so destructuring stays
  consistent. Required for any wire format that stores a fixed-width
  digest (zsync's truncated 3–16 byte MD4 per block, binary git object
  IDs, SigV4 string-to-sign concatenation) — hex doubles size and loses
  embedded-NUL semantics. TLS-tracked split-accessor pattern, independent
  buffer from the base64-decode slot so concurrent uses don't clobber.

- **`std.crypto.random_bytes(n)` — cryptographically-secure RNG** (#630).
  The CSPRNG missing from std (std.math.random is a PRNG, fine for
  sampling, NOT for secrets). Backed by:
    - Linux: `getrandom(2)` → `/dev/urandom` fallback
    - macOS / *BSD: `arc4random_buf(3)`
    - Windows: `BCryptGenRandom` (CNG)
  No OpenSSL dependency — the OS CSPRNGs are the right source. Same
  `(bytes, length, err)` tuple shape as the digest-bytes family.
  Regression test exercises 32-byte, 1024-byte, n=0, n=-1.

- **`std.uuid` — UUID v4 and v7 (RFC 9562)**. v4 is full-random; v7 is the
  modern time-sortable variant (48-bit Unix-ms timestamp + 74 bits of
  randomness) — preferable for primary-key use because it keeps B-tree
  indexes from fragmenting the way v4 keys do. Both produce the canonical
  36-char `8-4-4-4-12` hex form with version/variant bits patched per the
  RFC.

- **`std.ulid`** — ULID (https://github.com/ulid/spec): 128-bit
  lexicographically-sortable identifier encoded as 26 Crockford base32
  chars. Smaller wire size than UUID (26 vs 36) and time-sortable like
  UUIDv7. Use UUIDv7 when downstream systems already speak UUID; ULID
  when wire size matters and you control both ends.

- **`std.nanoid`** — NanoID: 21-char URL-safe identifier with 126 bits of
  entropy. No time component. Good fit for URL path / query slots where
  size and shell-safety matter and you don't want time leakage.

- **`std.ksuid`** — KSUID (Segment): 27-char base62 identifier, second-
  resolution timestamp + 128-bit payload. Predates ULID; included for
  Segment-ecosystem interop.

- **`std.tsid`** — TSID (Twitter Snowflake family): 64-bit time-sortable
  identifier, 13-char Crockford base32. Half the wire size of UUID/ULID
  (8 vs 16 bytes); use when ID size matters more than the full 128-bit
  entropy space.

- **`std.url` — RFC 3986 percent-codec + query parser** (#629). Three
  encoder flavors matching Go's `net/url` semantics for byte-exact
  parity (load-bearing for SigV4 canonical-request signing where any
  deviation invalidates the signature):
    - `url.encode(s)` — query-component (Go: `url.QueryEscape`)
    - `url.encode_path(s)` — path-segment (Go: `url.PathEscape`)
    - `url.encode_strict(s)` — strict unreserved-only
  Plus `url.decode(s)` (with `+` → space), `url.parse_query(s)`,
  `url.query_get(list, name)`, `url.query_get_all(list, name)` —
  correct multi-param + repeated-key handling, which is also the proper
  resolution for the http.get_query_param failure mode (#625): the URL
  module is the right place for the parser.

- **`std.os.now_unix_ms()`** — wall-clock Unix epoch in milliseconds.
  Required for time-ordered identifiers (UUIDv7, ULID, KSUID, TSID).
  CLOCK_REALTIME on POSIX; `GetSystemTimeAsFileTime` with the 1601→1970
  shift on Windows. Subject to NTP / manual clock adjustments by
  design — use `now_monotonic_ms` for measuring durations.

### Changed

- **`http.server_get` (and `server_post` / `server_put` /
  `server_delete` / `server_add_route`) — `user_data` slot documented
  as usable** (`std/http/module.ae`, `docs/http-handler-context.md`,
  `tests/integration/http_handler_user_data/`). Closes #633. The C
  side has always plumbed `user_data` end-to-end (the route remembers
  it and passes it back to the handler as the 3rd arg), but no
  example or test showed the struct-ptr → user_data pattern, so
  downstream porters (fbs-core) concluded the slot was unusable from
  Aether and dropped to a C shim of process statics. New
  `docs/http-handler-context.md` walks the
  `malloc → struct fields → ud as *YourStruct` pattern with a
  per-route DB-handle worked example. Integration test pins per-route
  isolation, null-ud compatibility, and re-call stability across
  handler invocations. No code change — purely docs + example.

## [0.217.0]

### Changed

- **`string.split` / `string.array_get` — lifetime warning added to
  module.ae and C source** (`std/string/module.ae`,
  `std/string/aether_string.c`). `string_array_get` returns a
  pointer that *borrows* from the array's storage; the natural
  pattern `v = string.array_get(parts, idx); string.array_free(parts);
  return v` silently use-after-frees. The externs in `std/string/
  module.ae` previously had zero lifetime documentation (the
  sibling `*StringSeq` API right below got a generous docblock).
  Added an explicit warning at the extern declarations, listed two
  safe escape patterns (`string.concat(v, "")` to make an owned
  copy, or `string_split_to_seq` for refcounted cells), and
  mirrored the warning at the C definition. No behaviour change —
  the existing pattern is still cheap when access is "build, walk
  once, free" within one scope; the doc clarifies when that's
  unsafe. Caught downstream by the aeb-side validator parsing
  tab-joined rule rows (ctr_notes.md OPEN #3).

### Fixed

- **`ae build` auto-links the tinygo bridge's transitive `-lffi`**
  (`tools/ae.c`, `tests/integration/host_tinygo/test_host_tinygo.sh`).
  Closes ctr_notes.md Finding 2. The six interpreter bridges
  (python/ruby/lua/perl/tcl/duktape) dlopen their host language at
  runtime and have no link-time deps of their own. `tinygo` is the
  first bridge whose `.a` carries undefined symbols — `ffi_*` from
  `tinygo_call_dynamic`, compiled in under `AETHER_HAS_LIBFFI`.
  The import auto-link previously stopped at the `.a` and required
  every consumer to add `link_flags = "-lffi"` to their
  `aether.toml`, defeating the "import is the trigger" promise in
  contrib/host/python/README.md §8. `prepare_host_bridge_imports`
  now probes the resolved `.a` with `nm -u | grep ffi_prep_cif`
  and only appends `-lffi` when the symbol is actually undefined.
  By-symbol (not by-language) so an end-host that received a
  bridge `.a` built without libffi never tries to link a missing
  `-lffi`. One `popen` per `ae build`; no measurable cost. The
  workaround `aether.toml` in this test is removed — the test now
  exercises the auto-link path it was previously masking.

- **`http.get_query_param` returns the requested key's value, not
  the last param's value** (`std/net/aether_http_server.c`,
  `std/net/aether_http_server.h`,
  `tests/integration/http_query_param_multi/`,
  `tests/integration/http_external_ptr/shim.c`). Closes #625.
  The previous implementation parsed `req->query_string` with
  `strstr` on every call and returned a pointer into a
  function-local **static** buffer; two sequential
  `get_query_param` calls overwrote the same buffer, so both
  returned pointers aliased the second call's value (e.g.
  `?alpha=AAA&beta=BBB` made `get_query_param("alpha")` and
  `get_query_param("beta")` both return `"BBB"`). Now lazily parses
  the query string once into per-key `query_keys`/`query_values`
  arrays on the request (mirroring how `header_keys`/`param_keys`
  already work) and looks up via `strcmp`. Each call returns a
  stable per-request pointer. Substring-of-another-key safety is
  now structural (exact `strcmp`) instead of leaning on the
  `&`-boundary check. The lazy-parse arrays are freed in
  `http_request_free`. Downstream impact: this broke S3 multipart
  routing in the fbs-core port (`?uploadId=…&partNumber=1` made
  `get_query_param("uploadId")` return `"1"`).

- **Build cache invalidates on edits to symlinked lib-dir entries**
  (`tools/ae.c`,
  `tests/integration/cache_symlinked_lib_edit/`). Closes #623.
  `compute_cache_key` previously only `stat`ed each `tc.lib_dirs[i]`
  directory itself — and a directory's mtime only bumps on
  create/delete/rename, NOT on an in-place edit of an existing
  entry. When the entry was a symlink to a file *outside* the lib
  dir (the natural shape when a test harness builds a flat
  `.ae_test_lib/` of symlinks to vendored modules), `sed -i` of
  the link target left BOTH the symlink's and the lib dir's mtime
  unchanged. The cache key stayed stable, `ae run` served a stale
  binary, and edits looked like no-ops until
  `rm -rf ~/.aether/cache`. The fix walks each lib dir's top-level
  `.ae` / `.c` / `.h` entries, `stat`s each (which follows
  symlinks), and folds the resolved target's mtime + size into a
  rolling FNV hash that joins the cache key. Per-dir entry count
  is also capped at 256 — well above any realistic stdlib/vendored
  count, but bounded so a runaway can't blow the key buffer. `ae`
  now also seeds `tc.lib_dirs[]` from `AETHER_LIB_DIR` at the top
  of `compute_cache_key` (matching the documented priority in
  `ae lib-path`); without that seed the entry walk wouldn't run
  when only the env var was set. Downstream impact: in the
  fbs-core port a correct fix to a module read as "did nothing"
  for ~30 minutes until the cache was cleared.

- **`contrib.host.tinygo` docs and integration test now use
  `go build`, not `tinygo build`, for native c-shared**
  (`contrib/host/tinygo/README.md`,
  `contrib/host/tinygo/module.ae`,
  `contrib/host/tinygo/examples/greet.go`,
  `examples/host-tinygo-demo.ae`,
  `tests/integration/host_tinygo/test_host_tinygo.sh`,
  `tests/scripts/contrib_build.sh`). TinyGo 0.34.0's
  `-buildmode=c-shared` is wasm-only ("buildmode c-shared is only
  supported on wasm at the moment"); the README, module header,
  example header, demo and integration test all documented
  `tinygo build -buildmode=c-shared` for native linux/macOS/Windows,
  which has never worked. The integration test silently `[SKIP]`ed
  on the failed `tinygo build` (line 34 — "tinygo c-shared build
  failed (likely target/version mismatch)"), so the end-to-end
  smoke test had been a no-op since v0.215.0 landed. Docs now show
  standard `CGO_ENABLED=1 go build -buildmode=c-shared` for native
  and reserve TinyGo for the wasm target. The bridge itself is
  unchanged — it's a runtime `dlopen` of whatever c-shared `.so`
  the toolchain produced. The integration test now hard-fails on
  build error instead of skipping (skip is now reserved for "no Go
  toolchain installed at all"). The `--with=tinygo` builder image
  already ships both `go` and `tinygo`, so no capability change is
  needed. The bridge module name `contrib.host.tinygo` is retained
  as the brand. Caught downstream by the aeb-side validator on the
  NUC after v0.216.0 image bake (ctr_notes.md "Finding 1").

## [0.216.0]

### Fixed

- **`--with=tinygo` layer — install upstream Go 1.23.4, not bookworm's
  Go 1.19** (`tools/docker/aether-build`). The original v0.215.0
  layer used `apt install golang-go`, which on debian:bookworm-slim
  installs Go 1.19. TinyGo 0.34.0's build-time Go-version gate
  requires ≥1.22; the image built green but `tinygo build` in
  user-program Phase 1 would fail with a version-rejection error.
  Layer now fetches a pinned Go tarball from go.dev
  (`--build-arg GO_VERSION=1.23.4`) and includes a `tinygo version`
  sanity-check RUN step so a broken Go/TinyGo pairing fails at
  IMAGE build time rather than at first user `tinygo build`.

## [0.215.0]

### Added

- **`aether-build --with=tinygo` — TinyGo c-shared sidecar capability**
  (`tools/docker/aether-build`, `tests/scripts/contrib_build.sh`,
  `Makefile`). New `--with=tinygo` token builds an
  `aether-builder-tinygo:slim` image with `libaether_host_tinygo.a` +
  TinyGo (pinned to 0.34.0 via `--build-arg TINYGO_VERSION=…`) +
  `golang-go` (cgo host Go) + `libffi-dev`. The bridge `.c` is pure
  dlopen via `std.dl` — TinyGo isn't needed to build the `.a`; it's
  needed at user-program build time when `aeb`'s `aether.tinygo_lib`
  builder shells `tinygo build -buildmode=c-shared` to produce a
  sidecar `.so`. The capability ships the toolchain in the builder
  image so the aeb-toolchain layer stays language-agnostic (same
  property the six interpreter bridges rely on). Closes the
  capability-set across all host-language bridges (python, ruby, lua,
  perl, tcl, duktape, tinygo).

  **Note**: the `golang-go` choice here was wrong (Go 1.19, rejected
  by TinyGo 0.34); see the `[current]` section above for the followup
  fix that switched to a pinned go.dev tarball.

### Changed

- **`install-contrib` ships `contrib/host/tinygo/`** (`Makefile`).
  Was previously stripped under the v1 "out of scope" comment that
  also nukes `host/java` + `host/go`. tinygo IS in scope — it's
  the first c-shared sidecar capability and its `module.ae` must be
  installable so `import contrib.host.tinygo` resolves in
  `aether-builder-tinygo:slim`.

## [0.214.0]

### Changed

- **`contrib.host.js` renamed to `contrib.host.duktape`; `js` kept
  as alias** (`contrib/host/duktape/` directory, `tools/docker/aether-build`,
  `tools/ae.c`, `tests/scripts/contrib_build.sh`, related).

  The bridge is honestly Duktape (the engine), not a generic JS
  capability. JS is the one slot where the host could in principle
  be served by multiple runtimes (Duktape / QuickJS / V8-node);
  naming by engine future-proofs the exact corner that's likeliest.
  Mirrors what python/ruby/perl/lua/tcl already do (lang == runtime).

  - Directory `contrib/host/js/` → `contrib/host/duktape/`.
  - C symbols `js_*` → `duktape_*`; `AETHER_HAS_JS` → `AETHER_HAS_DUKTAPE`.
  - Bridge .a renamed `libaether_host_js.a` → `libaether_host_duktape.a`.
  - Env var `${AETHER_JS_SONAME}` → `${AETHER_DUKTAPE_SONAME}`.
  - `aether-build --with=duktape` is canonical; `--with=js` is an
    alias and produces the same image (alphabetically canonicalised
    BEFORE the sort/name-derivation, so `aether-builder-duktape:slim`
    and the same cached layer).
  - **Back-compat**: `import contrib.host.js` keeps working via a
    thin shim module (`contrib/host/js/module.ae`) that re-exports
    the duktape bridge's C symbols under the `js_*` Aether names via
    `@extern("duktape_*")` annotations.
    `tools/ae.c`'s import-driven auto-link recognises `js` → links
    `libaether_host_duktape.a`. No `libaether_host_js.a` is built.

- **`contrib.host.perl` exports `perl.*` dotted shape; matches the
  other five bridges** (`contrib/host/perl/module.ae`).

  The aeb-side capstone-validation runs surfaced that `perl.run(...)`
  didn't resolve: the bridge's exports were prefixed `aether_perl_*`
  (chosen to avoid C-symbol collision with libperl's own `perl_run`/
  `perl_init`), so callers had to use the bare prefixed form.
  Every other bridge uses `<lang>.run(...)` — perl was the odd one.

  Fix: the C symbols stay `aether_perl_*` (still load-bearing for
  avoiding libperl collision at link time), but `module.ae` now uses
  `@extern("aether_perl_…") perl_…(…)` to expose them under the
  uniform `perl_*` Aether-side names. Three call shapes all work:
  - `perl.run(...)` — canonical, matches the other five bridges.
  - `perl_run(...)` — flat-prefix, same fn under a different surface.
  - `aether_perl_run(...)` — back-compat, original Aether-side name.

- **Bug 8: `--with=js` apt-pkg fix** (`tools/docker/aether-build`).

  The old `with_dev_pkg` mapped `js → libduktape-dev`, which doesn't
  exist on Debian bookworm — the correct package is `duktape-dev`.
  Fixed as part of the duktape rename (the new `duktape` and the
  legacy `js` alias both map to `duktape-dev`).

## [0.213.0]

### Changed

- **All six `contrib.host.*` dlopen bridges: hardcoded
  version-fallback lists removed; strict two-step contract**
  (`contrib/host/{python,ruby,lua,perl,js,tcl}/aether_host_*.c`,
  matching READMEs).

  The earlier dlopen rewrites carried short hardcoded soname
  fallback lists (e.g. `libpython3.{10..14}.so.1.0`,
  `libruby.so.3.{0..4}`) as defence-in-depth. In practice this is a
  maintenance treadmill — each new minor needs the list extended,
  and a list-miss looks identical to a runtime bug at the deploy
  site. Worse, "the fallback caught it" was lucky pattern-matching,
  not engineering: when a host's lib doesn't match a hardcoded
  name, the right answer is for the orchestrator to know how to
  probe.

  **New contract** for every bridge:
  1. Try `${AETHER_<LANG>_SONAME}` (orchestrator-supplied exact).
  2. Try `libfoo.so` (unversioned symlink, when present).
  3. Fail clearly. Error message names the env var AND gives a
     hint command the orchestrator can use to derive the soname.

  Hint commands per language (now in the bridge's stderr error
  message AND in each README):
  - **python**: `python3 -c 'import sysconfig; print(sysconfig.get_config_var("INSTSONAME"))'`
  - **ruby**: `ruby -rrbconfig -e 'print RbConfig::CONFIG["LIBRUBY_SO"]'`
  - **perl**: `perl -MConfig -e 'print $Config{libperl}'`
  - **lua**: `lua -e 'print(_VERSION)'` + derivation (no clean
    runtime probe; orchestrator's job)
  - **js (duktape)**: `ldconfig -p | awk '/libduktape\\.so/{print $1; exit}'`
    (duktape is embedded-only — no CLI to probe with)
  - **tcl**: `echo 'puts libtcl$tcl_version.so' | tclsh`

  **Verified locally on Debian 12 dev box:**
  - python + lua: still succeed via step 2 (their `libfoo.so`
    symlinks are present from `*-dev` packages).
  - ruby: now fires the new clean error (`AETHER_RUBY_SONAME=(unset)`,
    `libruby.so` absent — Debian only ships dash-versioned). Setting
    `AETHER_RUBY_SONAME=$(ruby -rrbconfig -e …)` to
    `libruby-3.1.so.3.1.2` resolves cleanly; binary prints
    `ruby 3.1.2`.

  Orchestrator implication: aeb-ctr's `pass_soname` helper should
  cover all six languages (currently has python/perl/tcl). Adding
  lua/ruby/js needs probe code in aeb-ctr's helper — see
  ctr_notes.md for the per-language hint commands. Bazzite NUC's
  ruby capstone GREEN (2026-06-04) happened to work via the now-
  removed fallback list; it'll need aeb-ctr's `pass_soname` ruby
  branch wired up before the next NUC run.

## [0.212.0]

### Changed

- **`contrib.host.{lua,perl,js,tcl}` bridges converted to dlopen**
  (`contrib/host/lua/aether_host_lua.c`,
  `contrib/host/perl/aether_host_perl.c`,
  `contrib/host/js/aether_host_js.c`,
  `contrib/host/tcl/aether_host_tcl.c`, all four READMEs).

  Completes the dlopen sweep for all six "interpreter" host bridges
  (python v0.209.0, ruby v0.211.0, now lua + perl + js + tcl). Each
  bridge .a has NO unresolved lib symbols at link time; end-user
  binaries have NO `DT_NEEDED libfoo` for the host language; binaries
  are ABI-portable across deploy-host minor versions.

  **lua dlopen contract:**
  - Tries `${AETHER_LUA_SONAME}` → `liblua.so` → `liblua5.{4,3}.so.0` /
    `liblua5.{4,3}.so` / `liblua-5.{4,3}.so` fallback list.
  - Macros like `luaL_dostring`, `lua_pop`, `lua_tostring`,
    `lua_pushcfunction`, `luaL_checkstring` re-expressed as inline
    static helpers over the dlsym'd underlying functions
    (`luaL_loadstring`, `lua_pcallk`, `lua_settop`, `lua_tolstring`,
    `lua_pushcclosure`, `luaL_checklstring`). Lua's symbol versioning
    (`lua_close@@LUA_5.4`) is dlsym-transparent.

  **perl dlopen contract:**
  - Tries `${AETHER_PERL_SONAME}` → `libperl.so` → `libperl.so.5.{40..30}`.
  - The bridge avoids SV-struct-layout dependencies entirely by
    going through `Perl_*` exported accessors instead of bit-tag
    macros:
      - `eval_pv(s, croak)`   → `Perl_eval_pv(my_perl, s, croak)`
      - `ERRSV`               → `Perl_get_sv(my_perl, "@", 0)`
      - `SvTRUE(sv)`          → `Perl_sv_true(my_perl, sv)`
      - `SvPV_nolen(sv)`      → `Perl_sv_2pv_flags(my_perl, sv, NULL, SV_GMAGIC)`
  - `SV_GMAGIC = 2` baked as a literal (stable across Perl 5.x —
    same shape as Ruby's `Qnil`-as-literal).
  - `pTHX_` context-passing macro replaced with explicit `my_perl`
    first argument.

  **js (Duktape) dlopen contract:**
  - Tries `${AETHER_JS_SONAME}` → `libduktape.so` →
    `libduktape.so.{206..208}` (Debian numeric soversion).
  - Three convenience macros re-expressed:
    - `duk_create_heap_default()` → `duk_create_heap(NULL,…)`
    - `duk_peval_string(c,s)` → `duk_eval_raw(c, s, 0, FLAGS)`
      where FLAGS = `DUK_COMPILE_EVAL | SAFE | NOSOURCE | STRLEN | NOFILENAME`
      (public DUK_COMPILE_* constants baked).
    - `duk_safe_to_string(c,i)` → `duk_safe_to_lstring(c, i, NULL)`.

  **tcl dlopen contract:**
  - Tries `${AETHER_TCL_SONAME}` → `libtcl.so` →
    `libtcl{9.0,8.6,8.5}.so.0` / `libtcl{9.0,8.6,8.5}.so`.
  - The cleanest of the six rewrites — Tcl's C API is conventionally
    non-macro, so no macro re-expressions needed. Plain `Tcl_*`
    function-pointer table.

  **End-user impact (all six bridges):**
  - `aether.toml` for a program importing any of `contrib.host.*` no
    longer needs `cflags` / `link_flags`.
  - Binaries built on one minor work on another (e.g. lua 5.3 → 5.4,
    perl 5.36 → 5.40, duktape 207 → 208, tcl 8.6 → 9.0).
  - READMEs rewritten with the dlopen contract + `${AETHER_*_SONAME}`
    fallback. All mirror `contrib/host/python/README.md` shape.

## [0.211.0]

### Changed

- **`contrib.host.ruby` bridge converted to dlopen** (`contrib/host/ruby/aether_host_ruby.c`,
  `contrib/host/ruby/README.md`).

  Mirrors the v0.209.0 python bridge work — every Ruby C-API symbol
  now resolved via `dlsym` at first call. The bridge .a has NO
  unresolved Ruby symbols (`nm -u` confirms); produced binaries have
  NO `DT_NEEDED libruby-3.X.so`. End-user binaries are ABI-portable
  across deploy-host Ruby minor versions.

  **Loading order** at first `ruby.run*` call:
  1. `${AETHER_RUBY_SONAME}` env var (orchestrator-supplied exact).
  2. `libruby.so` (Fedora/Bazzite-style flat symlink).
  3. Fallback `libruby-3.{4..0}.so` (Debian) + `libruby.so.3.{4..0}`
     (Fedora versioned).

  **Macro re-expression details:**
  - `RUBY_INIT_STACK` → explicit `ruby_init_stack(&local_VALUE)`
    call (`ruby_init_stack` is a real exported function; the macro
    just captured the stack-bottom local for us).
  - `Qnil` → baked `(VALUE)0x08` (`RUBY_Qnil` is a compile-time
    enum constant under `USE_FLONUM=1`, the default since Ruby 2.0;
    not an exported symbol — nothing to dlsym).
  - `NIL_P(v)` → direct `v != BRIDGE_QNIL` compare.
  - `StringValueCStr(v)` → `rb_string_value_cstr(&v)` direct call
    (the function the macro wraps).
  - `rb_intern` and `rb_funcall` field names prefixed `f_` —
    Ruby `#define`s these as macros (`rb_funcall` expands to a GCC
    `__extension__({…})` statement-expr block), so unprefixed
    struct field names would get textually rewritten and break the
    build. Same trap as `Py_None` in the python bridge.
  - `snprintf` from `ruby/subst.h` is undef'd; the bridge's scrub-
    string builder uses libc snprintf (plain `%s` format, no Ruby
    `%-extensions`).

  **End-user impact:**
  - `aether.toml` for a program that imports `contrib.host.ruby` no
    longer needs `cflags` / `link_flags` for ruby — `ae build` adds
    nothing libruby-related to the link line.
  - Binaries built on Ruby 3.1 work on a 3.2 deploy host and vice
    versa.
  - README rewritten to document the dlopen contract + the
    `${AETHER_RUBY_SONAME}` fallback.

## [0.210.0]

### Added

- **`aether-build --with=<lang>` — additive per-bridge image layers**
  (`tools/docker/aether-build`, `tests/scripts/contrib_build.sh`,
  `contrib/host/ruby/aether_host_ruby.c`,
  `contrib/host/python/README.md`).

  The toolchain image (`aether-builder:slim`) no longer ships hosted
  language bridges by default. Each `--with=<lang>` adds an additive
  apt `-dev` kit layer + `make contrib MODULES=<lang>` step, so the
  image name auto-derives from the alpha-sorted set (e.g.
  `aether-builder-python-ruby:slim`). Order of `--with=` flags
  doesn't matter — sorted before name + layer derivation, so podman's
  layer cache is maximally shared between
  `--with=python --with=lua` and `--with=lua --with=python`.

  Supported langs: `python`, `lua`, `perl`, `ruby`, `java`, `js`, `tcl`.
  Image-name override: `--image-name=<custom>`.

- **`make contrib MODULES=<lang>[,<lang>...]` — explicit-mode filter
  with hard-fail** (`tests/scripts/contrib_build.sh`).

  Default (`MODULES` unset) keeps build-all-tolerate-skip for dev
  convenience. Explicit (`MODULES=python`) builds ONLY the named
  modules and FAILS HARD on missing dep / compile / archive errors
  (exit 1 with named failures). Closes the v0.209.0
  "3 built, 4 skipped silently" footgun (ctr_notes.md Bug 6).

### Changed

- **Vendored multiarch headers retired** (`tools/docker/aether-build`).
  The Containerfile no longer clones `hosted-language-headers` into
  `/opt/aether/include/`. The vendored `pyconfig.h` / `luaconf.h` /
  `perl.h` turned out to be distro-generated build products that
  reference paths only the matching `-dev` package supplies
  (ctr_notes.md Bug 6, 2026-06-04). Each `--with=<lang>` layer
  now apt-installs the real `-dev` kit and uses ITS headers.

  - `HEADERS_REF` env var is no longer consulted; safe to remove from
    wrapper scripts that set it.
  - The vendored-fallback probe branches in `contrib_build.sh`
    (each `probe_<lang>` had a `[ -f "$VENDORED_INC/<lang>/<header>.h" ]`
    short-circuit) are removed; probes go straight to
    pkg-config / `python3-config` / etc.

### Documentation

- **`CONTRIBUTING.md` §8 / §9 narrowed** — distinguishes dlopen
  bridges (no user-side link flags needed; `${AETHER_<LANG>_SONAME}`
  fallback) from real-link bridges (still set
  `${AETHER_<LANG>_LDFLAGS}` per the existing env-var allowlist).
- **`contrib/host/python/README.md`** stays as the canonical example
  of the dlopen shape.

## [0.209.0]

### Changed

- **`contrib.host.python` bridge now `dlopen`s libpython at the deploy
  host's runtime** (`contrib/host/python/aether_host_python.c`,
  `tests/scripts/contrib_build.sh`, `tools/docker/aether-build`,
  `contrib/host/python/README.md`).
  Previously the bridge called CPython's C API directly, so the produced
  binary had `DT_NEEDED libpython3.X.so.1.0` baked in at build time —
  ABI-locking it to a specific Python minor version, and requiring
  `python3-dev` in the build environment. Now all CPython C API access
  goes through a `dlsym` function-pointer table; the bridge .a has no
  unresolved `Py_*` symbols (`nm -u` confirms); the produced binary has
  no libpython in its DT_NEEDED list.

  **Loading order** at first `python.run*` call:
  1. `${AETHER_PYTHON_SONAME}` env var (orchestrator-supplied exact
     soname, e.g. `libpython3.14.so.1.0`).
  2. `libpython3.so` (unversioned symlink — runtime-package on
     Fedora-likes / Bazzite; -dev-only on Debian-likes).
  3. Fallback list `libpython3.{10..14}.so.1.0`.

  Clean error to stderr if none load, naming the env-var escape hatch.

  **End-user impact:**
  - `aether.toml` for a program that imports `contrib.host.python` no
    longer needs `cflags` / `link_flags` for python — `ae build` adds
    nothing libpython-related to the link line. README rewritten to
    drop the `${AETHER_PYTHON_LDFLAGS}` recipe (now obsolete for
    python specifically; the `${VAR}` mechanism stays for bridges
    that link real libs).
  - Binaries built today on Python 3.11 work on a 3.14 host, and vice
    versa. The deploy host just needs *a* Python 3 runtime; no minor
    matching needed.

- **`tests/scripts/contrib_build.sh` probes the vendored
  `/opt/aether/include/<lang>/` headers before falling back to
  pkg-config / `python3-config`** (the `tools/docker/aether-build`
  image populates this dir from `hosted-language-headers`).
  Lets the contrib bridge .a's build in the slim image without
  installing any distro `-dev` package. Existing dev-tree builds
  (no vendored dir, pkg-config present) are unaffected — same six
  bridges built, same behaviour.

- **`tools/docker/aether-build` image now runs `make install-contrib`
  after vendoring the host headers** (the Containerfile heredoc in
  `tools/docker/aether-build`).
  The image ships the bridge `.a` archives at
  `$PREFIX/lib/aether/libaether_host_<lang>.a`, where `ae build`'s
  v0.208.0 auto-link expects them. Combined with the dlopen rewrite
  above, this makes the Bazzite/aeb-ctr "compile in container, run on
  host with that host's Python" chain end-to-end clean.

## [0.208.0]

### Added

- **`ae build` links `libaether_host_<lang>.a` automatically when a
  program `import contrib.host.<lang>`**
  (`tools/ae.c`,
  `tests/integration/host_bridge_import_link/`,
  `contrib/host/python/README.md`).
  Previously the import resolved the bridge headers (so the program
  compiled), but the bridge static archive was NEVER linked — so the
  produced binary failed at runtime with `undefined symbol: python_run`
  (the BRIDGE's own function, not a CPython symbol). Users had to
  repeat the import as `link_flags = "-laether_host_python"` in
  `aether.toml` — busywork easily forgotten (ctr_notes.md Bug 4 trace,
  2026-06-03).

  Now: import-driven. `import contrib.host.python` in the entry .ae
  queues `libaether_host_python.a` onto the link line in the correct
  position (before `-laether` / `tc.runtime_srcs` so the bridge's
  runtime references resolve). A pure-Aether program with NO
  `contrib.host.*` import does NOT link any bridge — critical, since
  blanket-linking would force a hello-world to dlopen libpython at
  runtime even when it doesn't use Python.

  **Scope:** entry-file-only. Only the .ae passed to `ae build` /
  `ae run` is scanned for `import contrib.host.<lang>`. If your
  imported module in turn imports a host bridge, you must still write
  the import in your entry file. Widening to transitive imports later
  is purely additive.

  **Hard error if `.a` is missing:** `ae: cannot find
  libaether_host_<lang>.a — required by ` `import contrib.host.<lang>` `…`
  points at `make contrib` (dev) or `make install-contrib` (installed).
  Silent fallback was what created Bug 4 in the first place.

  **POSIX-only:** the host bridges aren't compiled on the Windows
  matrix.

## [0.207.0]

### Added

- **`aether.toml` `[build]` `cflags` / `link_flags` support `${VAR}`
  expansion against the process environment**
  (`tools/ae.c`,
  `tests/integration/aether_toml_env_var_expansion/`,
  `contrib/host/python/README.md`).
  Values like `link_flags = "${AETHER_PYTHON_LDFLAGS}"` now substitute
  the named env var at build time. Use case: container-built / host-
  deployed flows (aeb-ctr on Bazzite, etc.) where the deploy host's
  linker flags are known only to the orchestrator, not to the
  toolchain image. The orchestrator probes the host once, passes the
  string in via `-e VAR=…`, and the in-container `ae build` consumes
  it through this expansion.

  **Security:** name allowlist — only `${AETHER_*}` is honoured (e.g.
  `${AETHER_PYTHON_LDFLAGS}`, `${AETHER_RUBY_LDFLAGS}`). Anything
  else warns to stderr and expands to the empty string, so an
  attacker who can write arbitrary env vars cannot hijack the link
  line via `${PATH}`, `${LD_PRELOAD}`, etc. The allowlist is
  conservative for v1; widening can come under review.

  **Other rules:** unset `${AETHER_*}` warns + expands empty (clarity
  failure mode); `\$` is a literal `$`; bare `$VAR` (no braces) is
  NOT expanded; shell `$(...)` command substitution is NOT supported
  (would let any `aether.toml` exec arbitrary commands at build time).

### Fixed

- **`tools/docker/aether-build` headers-clone failure on Bazzite** (PR
  #600): the `hosted-language-headers` clone used `git clone --depth 1`
  then `git fetch --depth 1 origin <SHA>`, which requires the remote
  to honour `uploadpack.allowAnySHA1InWant`. GitHub usually does but
  not reliably for arbitrary commits that aren't ref tips. Symptom on
  Bazzite: `fatal: reference is not a tree: <sha>` during
  `--rebuild-image`. Fix: drop `--depth 1` from the clone; full-history
  clone (~14 MB) then `git checkout <SHA>`. Applied to both the
  embedded Containerfile heredoc in `tools/docker/aether-build` and
  the modular helper `tools/docker/scripts/clone-host-headers.sh`.

## [0.206.0]

### Fixed

- **`tools/docker/aether-build` Bazzite follow-up — SELinux+crun
  auto-detect, mount-option separator** (PR #598).
  - The `--userns=keep-id` flag (in any form, bare or explicit
    uid/gid) crashes Bazzite's crun with `readlink ``: No such file
    or directory`. Both forms fail identically. v0.205.0 added an
    `AETHER_USERNS=` escape hatch but defaulted to keep-id, so the
    headline Bazzite target was still broken out of the box. This
    release auto-detects SELinux+crun hosts and drops the userns
    flag by default; explicit override still works.
  - The `:Z` SELinux mount label was built with a leading comma
    (`SELABEL_SUFFIX=",$SELABEL"`), producing `-v "$OUT:/out,Z"`
    which podman parsed as container path `/out,Z` rather than
    `/out` with label `Z`. Build wrote into the image's ephemeral
    `/out` and `~/aebuild/out/` stayed empty. Fix: `mount_opts()`
    helper that emits colon-first then comma-subsequent
    (`:ro,Z` / `:Z` correctly per case).

## [0.205.0]

### Fixed

- **`tools/docker/aether-build` Bazzite SELinux + crun keep-id, plus
  `--aeb` signpost** (PR #596).
  - On Bazzite (rootless podman + SELinux enforcing + crun), bare
    bind mounts hit `SELinux denies the read` (surface: misleading
    `not found`); add `:Z` to relabel. Refuses to relabel `$HOME`
    so `$WORK == $HOME` now emits a clear "use a subdir" message.
  - `--userns=keep-id` crashes Bazzite's crun; introduced
    `AETHER_USERNS=` env var to opt out (defaulted to keep-id; the
    follow-up in v0.206.0 flipped the default for Bazzite).
  - `--aeb` mode now emits a signpost message instead of dead-ending
    when aeb isn't installed: points at aeb's own
    `Containerfile.aeb-toolchain` layer, where the seam gets filled.

## [0.204.0]

### Added

- **`tools/docker/aether-build` — single-file bootstrap script for
  immutable-host development** (PR #594,
  `tools/docker/aether-build`).
  Curl-installable shell script that bootstraps a slim
  `aether-builder` container image (Containerfile is heredoc'd
  inline), then dispatches `aether-build app.ae` /
  `aether-build --aeb <target>` into it. Target use case: Bazzite
  and other immutable-by-design Linux hosts where you can't `apt
  install gcc` but can run rootless podman. Image carries
  vendored `contrib/host/{python,lua,perl,ruby,js}` headers from
  the pinned `hosted-language-headers` repo, so end-user programs
  using `import contrib.host.<lang>` compile without per-language
  install toggles. Runtime libraries (libpython, libruby, etc.)
  stay on the deploy host (the binary `dlopen`s them lazily).

## [0.203.0]

### Added

- **`os.now_local()` and `os.now_local_iso8601()` — local wall-clock
  time**
  (`std/os/aether_os.{c,h}`, `std/os/module.ae`,
  `tests/regression/test_os_now_local.ae`). The companion to
  `os.now_utc_iso8601()` (issue #524-era), but in local time with
  explicit timezone offset.

  `os.now_local()` returns a `*LocalTime` struct populated by a
  single `localtime_r` (POSIX) / `localtime_s` (Windows) call —
  `year`, `month`, `day`, `hour`, `minute`, `second`, `nanos`,
  `tz_offset_minutes`. Fields are atomic with each other: one
  C-side filler does the whole `gettimeofday` + `localtime` +
  timezone calculation, then writes the struct.

  `os.now_local_iso8601()` formats the same data as an RFC-3339
  string: `"2026-05-31T13:42:07+01:00"`, or `"...Z"` when the
  local offset happens to be zero (UTC).

  The C-Aether boundary uses a shared `extern`-ABI-style struct:
  Aether allocates `sizeof(LocalTime)` (8 packed `int` fields,
  32 bytes), passes the ptr to `os_now_local_fill_raw(out: ptr)`,
  the C side casts back to its mirror struct and writes. Matches
  the `extern struct` pattern PR #527 added; first use in
  `std/*/module.ae`. C-side layout asserted via `_Static_assert`.

  Windows-specific: `tm_gmtoff` is GNU/BSD only; the Windows path
  uses `_get_timezone` (which is "seconds WEST of UTC" — opposite
  sign from gmtoff) plus a DST adjustment when `localtime_s` flags
  `tm_isdst > 0`. POSIX is the simpler path (`localtime_r` +
  `tm_gmtoff` direct).

- **`os.platform()` — current OS as a string**
  (`std/os/aether_os.{c,h}`, `std/os/module.ae`,
  `tests/regression/test_os_platform.ae`). Closes the gap
  CONTRIBUTING.md "Coding for portability" hints at: the `$OS`
  env-var trick distinguishes Windows from POSIX but can't tell
  Linux from macOS from FreeBSD. `os.platform()` returns a flat
  lowercase string matching Go's `runtime.GOOS` and Rust's
  `std::env::consts::OS` exactly:

      "linux", "darwin", "windows", "freebsd", "openbsd",
      "netbsd", "dragonfly", "solaris", "wasm", "unknown"

  Picked at compile time from the toolchain's predefined macros
  (`__APPLE__`, `__linux__`, `_WIN32`, `__FreeBSD__`, etc.); never
  fails. Typical use:

      if os.platform() == "windows" { /* Windows-only path */ }

  For "do I have POSIX features?" the recommendation in the doc
  is still to probe the specific capability (e.g.
  `net.pipe_open() < 0`) rather than infer it from the platform
  name, but `os.platform() != "windows"` is a reasonable
  shorthand.

## [0.202.0]

### Added

- **Design doc: distinct types** (issue #480;
  `docs/distinct-types.md`). Draft design for Nim-inspired zero-
  runtime-cost nominal wrappers: `type Path = distinct string`,
  `type FD = distinct int`, etc. The doc frames the open design
  questions (cast spelling, operator inheritance, extern signature
  interaction, pattern-matching destructure, interpolation
  behaviour, layered distinct) with options + tradeoffs for the
  maintainer to resolve. Implementation is gated on those
  resolutions; this PR is the design-doc-only artefact that the
  issue's acceptance criteria asks for.

### Fixed

- **Parser: line-leading operator no longer folds into previous
  expression** (issue #528;
  `compiler/parser/parser.c`,
  `tests/regression/test_parser_line_leading_statements.ae`).
  PR #527 added a targeted guard that stopped the binary-expression
  loop when `*` started a new source line and was followed by
  `IDENT IDENT` (a `*StructName name` typed-pointer declaration).
  This generalises the guard into `is_newline_led_statement` so the
  loop also breaks for `*ident = ...` deref-store and is easy to
  extend for any future token that's both an infix operator and a
  valid statement-leading token. The recogniser is intentionally
  conservative: it only breaks when the post-operator tokens form a
  recognisable statement shape, so legitimate multi-line expressions
  (`sum = a` ↵ `    + b`) still compile. Other hazards documented in
  #528 (`-x`, `+x`, `(expr)`, `[a, b]` as line-leading statements)
  remain theoretical — the helper has the structure to add them
  reactively when/if they bite.

- **Typechecker rejects bare-int → Duration at parameter-passing**
  (issue #586; `compiler/analysis/typechecker.c`,
  `tests/regression/test_duration_param_strictness.ae`,
  `tests/integration/duration_param_strict_error/`). PR #583's
  Duration landing only enforced strict typing in comparisons
  (`dur > 5` errors) but accepted bare ints at parameter passing
  (`set_timeout(req, 5)` silently reinterpreted as 5 nanoseconds).
  This bit PR #583 itself: three callers passing plain ints to a
  newly-Duration-typed parameter compiled cleanly, then raced on
  macOS where the 5ns connect-timeout fired before the connection
  completed. The typechecker now rejects the call at compile time
  with a diagnostic that names the unit-suffix forms (`5ns`, `5us`,
  `5ms`, `5s`, `5m`, `5h`, `5d`, compound forms like `2m30s`). Rule
  applies to user-defined functions, builder functions, and extern
  functions; covers numeric scalar types (int, long, uint64,
  uint32/16/8, float). Same rejection shape as the existing
  comparison check.

  Side-fix: extern function symbols had `symbol->node = NULL`
  during global registration, which silently disabled the existing
  extern argument-type validation block at the call site (and would
  have disabled the new Duration check too). Externs now wire their
  AST node into the symbol so both validation passes actually fire.

## [0.201.0]

### Added

- **`contrib.xml.expat` probe — 8 XML-feature coverage cases**
  (`tests/integration/contrib_xml_expat/probe.ae`). Expands the
  integration probe from 8 → 16 sub-cases: nesting depth tracking
  (6-deep with siblings, balanced to 0), mixed content (text and
  child elements interleaved in document order), whitespace-only
  text events (pretty-printed XML produces text events for the
  inter-element whitespace), entity decoding (`&amp;` / `&lt;` /
  `&#65;` / `&#x42;` all resolved by libexpat before reaching the
  text handler), UTF-8 in element names + attribute values + text
  (`<café leño="árbol">日本語 🎉</café>` round-trips multibyte
  byte-for-byte), CDATA payload delivery (markup chars inside
  `<![CDATA[ ... ]]>` pass through raw via the regular text
  handler), real-world RSS fragment (extract title/link/description
  with the `current_field` pattern), and real-world SVG fragment
  (count `<rect>` elements and sum their `width` attributes).

  Implementation note: variables captured by `parse_with`'s handler
  closures are hoisted to function scope rather than declared inside
  per-case `{ ... }` blocks, sidestepping a known Aether codegen
  issue where heap-promoted strings / strbuilder ptrs captured from
  a nested block crash on the next event when the block exits.

## [0.200.0]

### Documentation

- **CONTRIBUTING.md — POSIX typedef portability + stdlib param-type
  grep recipe** (PR #585, `CONTRIBUTING.md`).
  - **§5b**: documents the MinGW gap for POSIX-only typedefs
    (`suseconds_t`, `ssize_t`, `off_t`); recommends casting through
    the underlying integer type (`long`, `intptr_t`, `int64_t`)
    rather than the typedef name, which silently breaks on the
    Windows matrix.
  - **"Changing a stdlib parameter's TYPE"**: when a PR widens or
    replaces a stdlib parameter's type (canonical case
    `int → Duration`), the typechecker doesn't always reject the
    old call shape — grep all callers in `tests/`, `examples/`,
    `std/` before merging. Codifies the rule we hit on PR #583.

## [0.199.0]

### Added

- **First-class `Duration` type with literal suffixes** (issue #524;
  `compiler/parser/lexer.c`, `compiler/parser/parser.c`,
  `compiler/parser/tokens.h`, `compiler/ast.{h,c}`,
  `compiler/analysis/{typechecker.c,type_inference.c}`,
  `compiler/codegen/{codegen.c,codegen_expr.c,codegen_stmt.c,codegen_actor.c}`,
  `compiler/aetherc.c`, `std/http/{module.ae,client/module.ae}`,
  `std/net/aether_http.{c,h}`, `tests/syntax/test_duration_literals.ae`,
  `tests/syntax/test_duration_http_timeouts.ae`,
  `tests/integration/duration_type_error/`).
  Numeric literals with a time-unit suffix (`ns`, `us`, `ms`, `s`,
  `m`, `h`, `d`) are now `Duration` values stored as `int64_t`
  nanosecond counts. Compound forms supported (`2m30s`, `1h15m`).
  Arithmetic: `dur + dur` / `dur - dur` → `Duration`,
  `dur * scalar` / `dur / scalar` → `Duration`, `dur / dur` →
  `float` (ratio), `dur <op> dur` → `bool`. Comparing a `Duration`
  against a plain number is a type error (no silent coercion).
  Component accessors `.ns` (int64) and `.us`/`.ms`/`.s`/`.m`/`.h`/
  `.d` (float ratio) read the value in that unit. Interpolation and
  `println` render via `_aether_duration_repr` which picks the
  largest readable unit and trims trailing zeros (`2500ms` prints
  `2.5s`). FFI: across the `aether_<name>` ABI boundary a `Duration`
  lowers to a plain `int64_t` (nanoseconds), so C/Python/Java
  consumers see an integer, not an opaque type. HTTP timeout APIs
  now accept `Duration` directly: `client.set_timeout(req, 30s)`,
  `http.get_with_timeout(url, 5s)`,
  `http.server_set_keepalive(srv, 1, 0, 5s)`,
  `http.server_shutdown_graceful(srv, 100ms)`. Sub-second
  precision is preserved through to the socket layer:
  `select` uses `tv_sec + tv_usec` (microsecond resolution),
  `SO_RCVTIMEO`/`SO_SNDTIMEO` use `struct timeval` (μs) on POSIX
  or a `DWORD` millisecond count on Winsock (sub-ms rounds up so
  it doesn't degrade to "infinite" via `DWORD=0`). Server-side
  keepalive idle and graceful-shutdown deadline use ms precision
  (their underlying poll / spin-wait loops are ms-granular).
  Wrappers go through `.ns` (int64) end-to-end — earlier drafts
  routed through `.ms` (a float ratio that silently truncated
  `1500ms` to `1`).

- **`contrib.xml.expat` — SAX-style XML parsing via libexpat**
  (`contrib/xml/expat/{aether_xml_expat.c,module.ae,README.md}`,
  `tests/integration/contrib_xml_expat/`). Thin Aether veneer over
  libexpat's native streaming SAX model: caller registers
  per-event handlers (start element / end element / character
  data), then feeds bytes into the parser; libexpat invokes the
  registered handlers as it walks the input.

  Surface (namespace is `expat.*` per the contrib path):
  - Parser lifecycle: `expat.parser_new()` /
    `expat.parser_free(p)`.
  - User-data slot: `expat.set_user_data(p, ud)` /
    `expat.get_user_data(p)` — per-parse state for the raw
    bare-fn handler surface.
  - Handler registration (raw surface): `expat.on_start(p, cb)` /
    `expat.on_end(p, cb)` / `expat.on_text(p, cb)`. Caller
    materialises the C function pointer at the call site with
    `my_handler as fn(args) -> void` — same qsort-style idiom as
    `tests/regression/test_fn_address_via_as_fn.ae`.
  - Closure builder veneer (ergonomic DX layer):
    `expat.parse_with(input) { bind_start(|name, atts| {...})
    bind_end(|name| {...}) bind_text(|text, len| {...}) }` —
    captured-state closures replace the user-data struct
    boilerplate, capturing locals from the enclosing scope
    directly. Routes the three closures through a C-side
    handler-set dispatcher so each event hits its own captured
    env (libexpat's single user_data slot can't carry three
    independent envs natively).
  - Drive: `expat.parse(p, buf, len, is_final) -> int` (1 ok, 0
    error) or the one-shot `expat.parse_full(p, buf, len)`.
  - Error reporting: `expat.error_string(p)` /
    `expat.error_line(p)` / `expat.error_column(p)`.
  - Attribute walking: `expat.attr_count(atts)` /
    `expat.attr_name(atts, i)` / `expat.attr_value(atts, i)` —
    hides libexpat's *2 pair-striding from Aether-side loops.

  Build: pkg-config-detected libexpat at link time. The wrapper
  is NOT bundled into `libaether.a` — same opt-in contrib-tier
  pattern as `contrib.sqlite`: consuming projects list
  `contrib/xml/expat/aether_xml_expat.c` in their
  `aether.toml`'s `extra_sources` and add
  `link_flags = "-lexpat"` to `[build]`. Install libexpat from
  the system package (`libexpat-dev` on Debian/Ubuntu,
  `expat-devel` on Fedora, `brew install expat` on macOS). The
  integration test probes via pkg-config (with a gcc-link
  fallback) and SKIPs cleanly when libexpat is absent.

  Test: `tests/integration/contrib_xml_expat/probe.ae` covers
  eight sub-cases under one process: well-formed-doc event
  counting, attribute walking, parse-error reporting with
  line/column, multi-chunk streaming parse, two-parser
  independence (handlers don't cross-contaminate state across
  parsers), and three closure-veneer cases (single-handler
  closure capture, three-handler fan-out, missing-handler
  graceful path).

  Not in v1: namespace-aware parsing (`XML_ParserCreateNS`),
  DOM-tree construction (this is streaming SAX only),
  comment/PI/CDATA/external-entity handlers. See README.md for
  the full list.

## [0.198.0]

### Added

- **`os.args_seal()` / `os.args_sealed()` — one-shot runtime gate
  on the argv accessors** (`runtime/aether_runtime.{c,h}`,
  `std/os/module.ae`, `tests/regression/test_os_args_seal.ae`,
  `docs/stdlib-reference.md`). After `os.args_seal()` returns, the
  argv-reading accessors all behave as if argv had never been
  initialised: `aether_args_count()` reports `0`,
  `aether_args_get(i)` returns null, `os.argv0()` returns `""`,
  `aether_argv_raw()` returns null. Idempotent — re-calling is a
  no-op. There is no unseal; the flag is sticky for the process
  lifetime, and a subsequent `aether_args_init()` (e.g. from an
  embedder path) also refuses to (re-)populate.

  Intended use: once `main()` has consumed the CLI flags into
  config state, call `os.args_seal()` to prevent any later code
  in the process (imported libraries, plugin callbacks, untrusted
  Aether modules running in-process) from reading the original
  argv. Complements the compile-time `hide` / `seal except`
  scope directives — those deny *lexical* access, this denies
  *runtime* access. A co-operative Aether-side gate, not a kernel
  boundary: the OS still has argv in process memory (Linux
  `/proc/self/cmdline`, macOS sysctl) and code that goes around
  the Aether accessors can still read it. Pair with the
  LD_PRELOAD libc sandbox for stronger guarantees.

  Implementation is one sticky flag + a store-zero in the runtime;
  the existing accessors fall through their null/out-of-range
  early-return paths and return their empty-state values naturally
  — no extra branches in the hot paths.

## [0.197.0]

### Fixed

- **`builder name(...) with <factory>` did not module-resolve the
  factory across an import boundary** —
  `compiler/aether_module.c`,
  `tests/integration/builder_with_factory_cross_module/`. The
  builder lowering emits `_bcfg = <factory>()` at every call site,
  but the import resolver renamed the builder's name to
  `<ns>_<builder>` and left the `with`-factory annotation as the
  bare unprefixed name. Consumer translation units then emitted
  `_bcfg = mkfac()` (unresolved bare symbol) and gcc/ld failed
  with "undefined reference to mkfac" even though `mkfac` was
  exported from the builder's module. Filed in
  `aether/new_aevg_asks.md` ASK 1 as the **blocker** for the AeVG
  surface DSL (`window` / `render_to` / `record` live in the
  `aether_ui` library and are called from example/transpiler-
  output files).

  Fix in two layers:
  1. `rename_intra_module_refs` now rewrites the
     `AST_BUILDER_FUNCTION` annotation alongside the body's
     `AST_FUNCTION_CALL` / `AST_IDENTIFIER` values, so the cloned
     builder carries `annotation = <ns>_<factory>`.
  2. `collect_intra_module_callees` + `prune_collect_calls` now
     treat the builder's annotation as a real call-dependency, so
     the factory's body gets pulled into the program AST and
     survives the mark-and-sweep dead-code prune. Without this,
     the factory's symbol stayed declared but undefined.

- **Closure-call trampoline arg-slot shift miscompiled bare
  named functions wrapped in `_AeClosure`** —
  `compiler/codegen/codegen.{c,h}`, `compiler/codegen/codegen_func.c`,
  `compiler/codegen/codegen_internal.h`,
  `compiler/codegen/codegen_expr.c`,
  `tests/regression/test_fn_bare_adapter.ae`. Reported in
  `aether/new_aevg_asks.md` ASK 3 (and the LAYER 2 follow-up
  in `fn_return_float_cast.md`).

  Pre-fix: at every coercion site where a bare named function got
  wrapped as `(_AeClosure){.fn=name, .env=NULL}`, the wrapped
  fn's REAL C signature was `R(args)` but the closure-invocation
  trampoline called `.fn` as `R(void* env, args)` — incompatible-
  pointer-conversion UB that shifted every argument by one
  register/stack slot:
    - `call(my_fn, 3, 7)` (where `my_fn(w, h) -> int`) returned
      `3` — NULL shifted into `w`, `3` into `h`, `7` dropped.
    - `call_it(my_double, 3.0) -> float` returned `0` (xmm0 holds
      the real arg but the env shift puts NULL in front).
    - string returns segfaulted; ptr returns read garbage.

  Fix: codegen synthesises a per-bare-fn env-ignoring adapter
  `_aether_bare_adapter_<name>(void* env, args) -> R` at file
  scope. The wrap site stuffs the **adapter's** address into `.fn`
  (not the bare fn's). The trampoline calls `adapter(env, args)`;
  the adapter ignores `env` and forwards `args` to the real fn —
  ABI shapes match on both sides. Adapters are registered via a
  pre-walk that mirrors the lazy coercion-site registration and
  emitted alongside closure definitions before main, so calls
  through them work regardless of where in the program the wrap
  site appears. Adapter forward decls and bodies live together
  in the same emission block — no chicken-and-egg ordering. Closes
  the LAYER 2 follow-up to PR #574 (the bare-fn arg/return-slot
  hazard that PR documented but couldn't close without per-
  signature adapter generation).

### Added

- **Qualified struct-literal syntax: `module.Type { field: value, ... }`** —
  `compiler/parser/parser.c`,
  `tests/integration/qualified_struct_literal/`. Pre-fix the parser
  only accepted the bare-name `Type{...}` form, so a struct
  imported with `import shapes` (vs `import shapes (Circle)`) had
  no way to be literal-constructed cross-module without the
  selective-import companion line. Filed in
  `aether/new_aevg_asks.md` ASK 2 from the AeVG port.

  Fix: parser detects the `IDENT . IDENT { IDENT :` (or
  `IDENT . IDENT { }`) lookahead pattern at the primary-expression
  dispatch and lowers it the same way as the bare-name form, just
  with the module prefix dropped from the type name. Module struct
  definitions land in the program AST by their bare struct name
  (matches how the import resolver clones them), so the lowered
  literal references the same C type either way.

## [0.196.0]

### Documentation

- **`docs/closures-and-builder-dsl.md` — LLM quick-reckoner +
  DSL-with-scope attribution** (PR #576,
  `docs/closures-and-builder-dsl.md`).
  Pins down the trailing-block / `_ctx` / `builder…with` semantics
  in a form an LLM (or human reader) can use as a single-page
  reference. Captures the rules that the existing builder-DSL code
  enforces but had nowhere documented: when `()` is required for a
  trailing block, how `_ctx` injection scopes through nested
  builder calls, how `builder...with` differs from a plain factory
  call. Companion to the `[builder DSL]` memory notes.

## [0.195.0]

### Fixed

- **Closure-call trampoline emitted `int`-return cast for any return
  type → segfault on string-returning closures, GCC warning on
  ptr/struct returns** — `compiler/codegen/codegen_expr.c`,
  `tests/regression/test_fn_closure_call_return_types.ae`. Reported
  in `aether/fn_return_float_cast.md` from the AeVG (CVG → Aether)
  port (initially float-only, widened 2026-05-30 to "any non-int
  return"). The trampoline that invokes a `fn`-typed parameter's
  `_AeClosure.fn` defaulted the cast's return-type slot to `int`
  when the call's `node_type` was UNKNOWN — which it always is for
  a bare-`fn` parameter (no signature info to read). String-typed
  closures **segfaulted** (int reinterpreted as `AetherString*` →
  heap-tracker dereferenced a non-heap value); ptr/struct returns
  worked by ABI luck on x86-64 SysV (both int and void* live in
  rax) but GCC warned and Windows LLP64 would truncate.

  Fix: extend the return-type fallback chain. The cast's return
  type now resolves in order: (1) the call's `node_type` if known;
  (2) the closure_arg's declared `return_type` (typed
  `fn(args) -> ret` signature); (3) the **enclosing function's**
  declared return type — handles the bare-`fn` shape
  `f(cb: fn) -> R { return cb(...) }` that has no signature to
  read. Argument types also resolve from the closure_arg's
  declared `param_types[]` when available. Default-to-`int` only
  fires when none resolve. The same fix surface that landed for
  string/ptr returns also tightens int return cases by surfacing
  the right type at the C boundary.

  **Known follow-up (NOT in this PR):** float-returning bare-fn
  calls remain broken under -O2. When a bare named function is
  wrapped at the coercion site as `(_AeClosure){.fn=name,
  .env=NULL}`, the wrapped fn's REAL C signature is `R(args)` (no
  env), but the trampoline calls it as `R(void*, args)` — incompat-
  ible-pointer-conversion UB that -O2 folds to undefined (typically
  zero for non-int values via floating-point return registers). Real
  fix needs a per-bare-fn adapter wrapper at the wrap site (a
  codegen-emitted `_aether_bare_adapter_N(void* env, args) -> R`
  that ignores env and forwards). Tracked separately; the
  string-segfault and GCC-warning shapes are closed by this PR.

## [0.194.0]

### Added

- **`std.string.from_int_radix(value, radix) -> string`** —
  inverse of `to_int_radix` (which landed in v0.193). Renders
  `value` in base `radix` (`[2, 36]`); empty string on invalid
  radix; `'-'` prefix for negatives. C runtime is a hand-rolled
  digit loop because libc snprintf only covers `%x` / `%o` / `%d`.
  `long long` parameter, not C `long`, for LLP64 safety (the same
  invariant `string_to_long_raw` documents — PR #562). Headline
  use case: hex color emit for animation interpolation
  (`r.toString(16).padStart(2, '0')` patterns). Pair with the new
  `pad_start` below for fixed-width hex bytes.
- **`std.string.pad_start(s, total_width, pad_char)` and
  `string.pad_end(...)`** — pad to a fixed width with a single-byte
  char code (`48` for `'0'`, `32` for `' '`). If `s` is already
  `>= total_width` chars, returns a fresh copy (no truncation;
  same allocation behaviour as the pad path so callers release
  uniformly). `pad_char` is a char code (not a multi-char string)
  to match `string_char_at`'s convention and avoid string-alloc
  overhead in tight loops.

### Fixed

- **`fn`-typed value at struct-field assignment rejected by
  typechecker** — `compiler/analysis/typechecker.c`,
  `compiler/codegen/codegen_expr.c`,
  `tests/regression/test_fn_struct_field_assignment.ae`. The
  Option B1 fn↔ptr boxing already in `[current]` covered call-
  site argument positions, but `h.cb = c` (where `cb: ptr` and
  `c: fn`) was rejected at typecheck with E0200. The CVG port hit
  this for 17 callback fields in `grammar_element.ae`
  (`onClick`, `bindFill`, `whenPredicate`, etc.). Fix: add
  `fn ↔ ptr` to `is_type_compatible`, and at the
  AST_BINARY_EXPRESSION assign-emit (`=`) box a `fn`-shaped RHS
  (or bare named function) with `_aether_box_closure(...)` when
  the LHS is `ptr`. Mirror of the call-site coercion path. Now
  the boxing/unboxing surface is uniform across call-site args,
  struct-field assignments, and (already supported) `as fn` casts.

- **`fn`-typed parameter / `ptr` slot coercion silently miscompiled
  to bad C** — `compiler/codegen/codegen_expr.c`,
  `tests/regression/test_fn_ptr_coercion.ae`. The type-checker
  accepted three coercion shapes at call sites — bare named
  function → `fn` param, `fn` local → `ptr` param, `ptr` (boxed
  closure) → `fn` param — but C codegen emitted incompatible types
  (e.g. `int (*)(void)` into a slot expecting `_AeClosure`, or
  `_AeClosure` into a slot expecting `void*`) and gcc rejected the
  program. Downstream (the aether-ui CVG port's `whenStack` pattern)
  worked around it by declaring callback params as `ptr`, losing
  captured-state closures through forwarding.

  Fix (Option B1 from the filing — make `fn ↔ ptr` actually work):
  the existing `_aether_box_closure` / `_aether_unbox_closure`
  runtime primitives are now wired at the coercion site. `fn → ptr`
  heap-allocates an `_AeClosure` struct and passes the pointer; bare
  named function → `fn` param emits a struct literal
  `(_AeClosure){ .fn=..., .env=NULL }`; `ptr → fn` unboxes back to
  the struct. Captured-state closures now survive forwarding through
  `ptr`-typed slots (lists, maps, struct fields, opaque externs).
  The pre-existing direct `bare-fn → ptr` direct-arg path remains
  backwards-compatible (existing call sites unchanged).
- **`ae cflags --libs` did not emit transitive dep flags
  (`-lpcre2-8`, `-lssl -lcrypto`, `-lz`, `-lnghttp2`)** —
  `tools/ae.c`, `tests/integration/ae_cflags/test_ae_cflags.sh`.
  `cmd_build` already passed `AETHER_OPENSSL_LIBS` / `_ZLIB_LIBS`
  / `_NGHTTP2_LIBS` / `_PCRE2_LIBS` to gcc, but `cmd_cflags` (the
  documented entry point for external builds via `gcc your.c
  $(ae cflags)`) silently omitted them. So downstream consumers
  of `std.regex` got `undefined reference to pcre2_*` at link
  time, and consumers of `std.cryptography` / `std.http` TLS got
  `SSL_*` failures, even though `libaether.a` was compiled with
  those deps — the archive's symbol table held unresolved
  references the consuming link had to satisfy. Now matched.
  Empty-string guard preserves the no-pkg-config case (a build
  without `libpcre2-dev` sets `PCRE2_LDFLAGS :=` empty; the
  macro expands to `""`; emit nothing). Test extended to nm
  the local `libaether.a` and assert each detected symbol
  family has its matching `-l` flag.

## [0.193.0]

### Added

- **`std.string.to_int_radix(s, radix) -> (long, string)` — base-N
  integer parse** (`std/string/{module.ae,aether_string.c,aether_string.h}`,
  `tests/regression/test_string_to_int_radix.ae`). Companion to the
  existing `to_int` (base-10 only). Headline use case: hex color
  decode (the `parseInt(hex, 16)` patterns ports keep hand-rolling),
  plus binary masks, octal mode bits, and any other base-N field
  that previously needed a hand-coded loop. Radix is 2..36 (strtoll's
  range). Returns `long` (int64), not `int`, because radix-16 / -36
  values hit beyond int32 quickly and because the canonical use cases
  (32-bit ARGB colors, file offsets, flag masks) want the full
  64-bit width. Caller passes the digit-only substring — no `"0x"`
  / `"0b"` prefix recognition (caller-controlled is the less surprising
  default for the color-parser shape this serves). C runtime is
  `strtoll` so the int64 invariant holds on Windows LLP64 too — same
  lesson PR #562 pinned for `string_to_long_raw`.

### Fixed

- **`std.string.from_float` returned garbage** — symmetric ABI mismatch
  to the v0.190.0 `to_float` fix (`std/string/aether_string.c`,
  `std/string/aether_string.h`,
  `tests/regression/test_string_from_number_widths.ae`).
  `string_from_float` declared its parameter as C `float` (32-bit) but
  Aether's `float` lowers to C `double` (64-bit), so the caller pushed
  8 bytes and the C side read only the low 4 as IEEE-754 binary32 —
  mostly mantissa, so the output was nonsense. The smoking gun:
  `from_float(1.0)` serialised as `"0"` because `double(1.0)`'s low 32
  mantissa bits are all zero (reads back as `+0.0f`); `from_float(3.14)`
  came out `"1.26444e+11"`; `from_float(0.0001)` came out
  `"-1.8891e+26"`. Now declared as `double` to match the Aether ABI.
  No format-string change needed (`%g` already prints `double` — that's
  the varargs default). Same bug class as PR #562 (`to_long` /
  `to_float` Windows-truncation fixes), opposite direction; whoever
  fixed the parse path didn't audit the emit path. The new regression
  test pins all four numeric emit paths (`from_int` / `from_long` /
  `from_float`) so the next iteration of this bug surfaces immediately.

## [0.192.0]

### Added

- **Tier-A stdlib data primitives for the aether-ui CVG port** (one
  batched PR; each item independently small but blocking the same
  downstream porting effort, so shipped together to conserve CI
  minutes):
  - **`std.floatarr` — fixed-size packed-double buffer** (new
    `std/floatarr/module.ae`, new
    `std/collections/aether_floatarr.c`,
    `std/collections/aether_collections.h`, `Makefile`,
    `tests/regression/test_floatarr.ae`). Float twin of `std.intarr`
    with identical API shape (raw externs + Go-style wrappers; raw
    `_unchecked` variants for hot loops). Aether's `float` lowers to
    C `double`, so the element type is `double` end-to-end. Motivating
    use case: SVG path-command arg storage (`number[]` in
    `normalizer.ts`), rasterizer edge tables, bbox corner accumulators,
    blur kernel coefficients — every `number[]` site the CVG port was
    boxing via `*Pt`/`std.list` for one scalar each.
  - **`std.math.pi / tau / e / deg_to_rad / rad_to_deg`** as zero-arg
    function-constants (`std/math/{module.ae,aether_math.c,aether_math.h}`,
    `tests/regression/test_math_constants.ae`). Aether has no
    const-expression evaluator at the call site, so the established
    convention is `math_pi()` → `return <literal>` (C inlines it).
    Replaces 24+ CVG sites that re-derived `Math.PI` / `Math.PI/180`
    from first principles per-file.
  - **`bytes.copy_from_bytes(dst, dst_off, src, src_off, length)`**
    (`std/bytes/{module.ae,aether_bytes.c,aether_bytes.h}`,
    `tests/regression/test_bytes_copy_from_bytes.ae`). Two-buffer
    copy with distinct source and destination AetherBytes pointers
    (memmove-based with bounds checks). Companion to the existing
    `copy_within` (same buffer, RLE-overlap semantics) and
    `copy_from_string` (string → bytes). Motivating use case:
    two-pass separable Gaussian blur — `pixels → temp → pixels` —
    where `copy_within` is wrong because the buffers are distinct
    allocations.
  - **`os.now_monotonic_ms()` / `os.now_monotonic_ns()`** —
    monotonic clock primitives
    (`std/os/{module.ae,aether_os.c,aether_os.h}`,
    `tests/regression/test_os_monotonic_clock.ae`). POSIX
    `clock_gettime(CLOCK_MONOTONIC)`; Windows
    `QueryPerformanceCounter` with cached `QueryPerformanceFrequency`
    (split-multiply on the ns path to avoid int64 overflow on
    long uptimes). The value-domain is opaque (epoch is
    boot / process-start / arbitrary); only *deltas* are meaningful.
    Returns 0 on no-filesystem (`AETHER_NO_FILESYSTEM`) builds. Use
    for animation tick loops, frame-time budgets, microbenchmarks
    — anything where wall-clock NTP-jumps would corrupt elapsed-time
    measurement. **C signature is `int64_t`, NOT C `long`** — same
    LLP64 / 32-bit-`long` hazard PR #562 fixed for
    `string_to_long_raw`; documented inline so the next addition
    starts from the right invariant. CONTRIBUTING.md adds a
    portability note for the pattern.

## [0.191.0]

### Added

- **`std.regex` — Perl-compatible regex via libpcre2-8**
  (`std/regex/{module.ae,aether_regex.c}`, `Makefile`, `tools/ae.c`,
  `docs/bootstrap-from-source.md`). Full PCRE2 surface: captures
  (numbered + named), `$1`/`$2`/`${name}` substitutions, Unicode,
  look-around, alternation, non-greedy, character classes, flags
  (`FLAG_CASELESS`/`MULTILINE`/`DOTALL`/`EXTENDED`/`UNGREEDY`). Go-style
  wrapper API (`regex.compile`, `compile_flags`, `matches`, `find`,
  `captures` + accessors, `find_all` + accessors, `replace`,
  `replace_all`, one-shot `matches_lit` / `find_lit`, `last_error`).
  PCRE2 is auto-detected via `pkg-config libpcre2-8` (mirroring the
  existing OpenSSL/zlib/nghttp2 pattern); without `libpcre2-dev` the
  build still succeeds and every entry point returns a clean
  "built without libpcre2-8" via `regex.last_error()`. Test:
  `tests/regression/test_regex.ae`.

## [0.190.0]

### Fixed

- **`std.string.to_long` truncated 64-bit values to 32 bits** — two
  independent causes, both fixed (`std/string/module.ae`,
  `std/string/aether_string.c`, `std/string/aether_string.h`):
  1. The Aether wrapper used an inferred multi-return (`-> {`), which
     collapsed the `long` value slot to 32-bit `int`, so
     `to_long("4294967296")` returned `0` on every platform. Fixed with an
     explicit `-> (long, string)` return type. (Underlying compiler
     limitation: inferred multi-return doesn't distinguish `long` from
     `int`; explicit annotation is the std idiom. Worth a separate look.)
  2. The C parse path used C `long` + `strtol`, which is only 32-bit on
     **Windows** (LLP64) — so even after (1) it still truncated there,
     writing 4 bytes into Aether's 8-byte slot. `string_to_long_raw` /
     `string_get_long` / `string_try_long` now use `long long` + `strtoll`
     (64-bit on every platform), matching what Aether's `long` lowers to.
     This also fixes the `string_to_long_raw` raw extern on Windows.
- **`std.string.to_float` returned garbage** (`std/string/aether_string.c`,
  `std/string/aether_string.h`). `string_get_float` returned a 32-bit C
  `float`, but Aether's `float` type is 64-bit (lowers to C `double`), so
  the result register was read as a double — `to_float("2.5")` came back
  `5.3e-315`. Now returns `(double)v` (parsing still at float precision;
  use `to_double` for full precision). `to_int` / `to_double` were already
  correct. Regression: `tests/regression/test_string_to_number_widths.ae`.

## [0.189.0]

### Added

- **`std.http` request-header iteration** (`std/net/aether_http_server.c`,
  `std/net/aether_http_server.h`, `std/http/module.ae`). New accessors let
  an Aether route handler enumerate **every** received request header, not
  just look one up by name (`http_get_header`):
  `http_request_header_count(req)`, `http_request_header_name(req, index)`,
  `http_request_header_value(req, index)` (plus `http.request_header_*`
  Go-style wrappers). Wire order, duplicates preserved, no
  sort/dedup/canonicalize; out-of-range index returns `""`; same 50-header
  cap as parsing. This was the last `std.http` gap blocking a pure-Aether
  rewrite of the Servirtium VCR recorder (`vcr_request_header_iteration_wish.md`)
  — the analog of `http_request_body_length` for faithful capture. Test:
  `tests/integration/http_request_header_iter/`.

## [0.188.0]

### Removed

- **`std.http.server.vcr` — the Servirtium VCR engine has moved out of
  the stdlib** into its own repository,
  [`servirtium-vcr`](https://github.com/aether-lang-org/servirtium-vcr),
  now its authoritative home alongside its 12 language bindings. VCR had
  served its purpose here — it shaped and hardened the HTTP server
  (port-0 binding, background/quiet serving, chunked decode, the
  `--emit=lib` PIC runtime and `aether_vcr_embed_*` C ABI) — and now
  belongs with the cross-language bindings it serves. Removed:
  `std/http/server/vcr/` (module + C), the 14 `test_vcr_*` tests + tape
  fixtures + `vcr_embed_abi/` C-driver, and the `svn_*` / climate VCR
  tests (all ported to the monorepo). Nothing in the language or the rest
  of the stdlib imported it, so the stdlib surface is unaffected. The
  `std.http.client` de-chunk coverage the VCR work motivated stays as a
  VCR-free test (`tests/integration/http_client_dechunk/`). Historical
  CHANGELOG entries (0.181/0.182/0.183/0.187) are retained.
  `docs/http-vcr.md` is now a pointer to the monorepo.

## [0.187.0]

### Added

- **VCR embed ABI: two missing wraps + strict-ignore ergonomics**
  (`std/http/server/vcr/embed.ae`, `docs/http-vcr.md`,
  `vcr_embed_surface_and_strict_ignore_wish.md`). The
  `aether_vcr_embed_*` C ABI now exposes
  `start_playback_url` (replay a tape fetched over HTTP) and
  `stop_and_flush_or_check` (the non-destructive `.actual`-sibling drift
  variant), reaching parity with the `vcr` module surface the four
  Servirtium bindings wrap. New `aether_vcr_embed_strict_ignore_common_headers()`
  drops the volatile request headers stock HTTP clients send
  (`User-Agent`, `Accept`, `Accept-Encoding`, `Connection`) from strict
  matching in one call — the underlying lever,
  `vcr.remove_header(FIELD_REQUEST_HEADERS, name)`, is now documented as
  the "ignore this header when matching" mechanism.

### Fixed

- **`vcr.load_url` use-after-free** (`std/http/server/vcr/module.ae`).
  It freed the HTTP response before parsing the body string borrowed from
  it, so an HTTP-sourced tape parsed as empty ("no '## Interaction '
  headers"). The body is now copied before the response is freed. Caught
  by `load_url`'s first test coverage (the embed `start_playback_url`
  round-trip).

### Changed

- **Embedded VCR runs quiet** (`std/http/server/vcr/{aether_vcr.c,module.ae,embed.ae}`).
  The embed ABI enables a quiet mode (`vcr.set_quiet`) so an embedded
  library never writes `vcr.load*` diagnostics to the host's stdout; the
  failure reason stays retrievable via `vcr_embed_last_error()` (now
  falling back to the new `vcr.load_error()`). Foreground `import
  std.http.server.vcr` callers are unchanged (quiet defaults off). The
  interactive "Press Ctrl+C" banner was already silenced for background
  servers in 0.185.0.

## [0.186.0]

### Added

- **`get.sh` — remote one-liner installer** (`get.sh`, `README.md`).
  `curl -sSL https://raw.githubusercontent.com/aether-lang-org/aether/main/get.sh | sh`
  fetches a pinned source tarball, builds the toolchain, and `make
  install`s it — no clone, no tests. Mirrors aeb's installer: env knobs
  `AETHER_REF` (tag/branch/SHA; default = latest `vX.Y.Z` tag, falls back
  to `main`), `PREFIX` (default `~/.local`, sudo-free), `CC`. Prereqs are
  just a C compiler + GNU make + curl/tar (Aether compiles to C, so no
  toolchain bootstrap dependency). Complements the existing clone-time
  `install.sh` (editor extension, `ae version` management, `~/.aether`
  layout) and `docs/bootstrap-from-source.md` (from-HEAD developer flow).

## [0.185.0]

### Changed

- **A backgrounded `std.http.server` now runs as a quiet embedded
  server** (`std/net/aether_http_server.c`, `std/net/aether_http_server.h`,
  `tests/integration/http_server_background_quiet/`). Started via
  `http_server_start_background_raw` (the embedding / `ae`-CLI background
  path), the server no longer prints the interactive `Server running at
  …` / `Press Ctrl+C to stop` banner — it's driven by
  `http_server_stop`, not a TTY — and its detached accept/worker threads
  now block async signals (`pthread_sigmask`; SIGURG, SIGINT, SIGTERM,
  SIGPIPE, …; synchronous fault signals SEGV/FPE/BUS/ABRT/TRAP/ILL are
  left deliverable). So a library/embedded server's worker threads never
  intercept a process-directed signal meant for the host application.
  Foreground servers (`http_server_start_raw` / `ae run` of a server)
  are unchanged — banner and signal handling as before. Reported by aeb
  (`std-http-server-background-sigurg-poisons-harness.md`): a
  backgrounded server left lingering under a sandboxed agent/CI harness
  poisoned the *foreground* command's exit accounting (SIGURG → 144).
  Note the exit-144 itself is the supervising harness's accounting of a
  lingering child process, not Aether crashing — Aether installs no
  SIGURG/SIGINT handler; this change removes the interactive-CLI
  behaviours from the embedded path (the requested quiet/embedded mode),
  but a harness should still *reap* a backgrounded server rather than
  leave it running past the command.

## [0.184.0]

### Fixed

- **A heap string passed to a wrapper that stores it (`f(v: string) {
  map.put(m, k, v) }`) is no longer freed out from under the container**
  (`compiler/codegen/codegen_stmt.c`, `compiler/codegen/codegen_expr.c`,
  `compiler/codegen/codegen_internal.h`,
  `tests/regression/test_map_value_storing_wrapper.ae`). The escape
  analysis judged a callee parameter purely by its declared type:
  `ptr`/unknown params escape (the callee may stash the pointer),
  `string` params are read-only (`string.length`, `print`, …). But a
  user wrapper with a `string`-typed value parameter that puts it into a
  `std.map`/`std.list` *does* let it escape — so a heap local passed in
  was never marked escaped, the caller's function-exit defer-free
  reclaimed it, and the map slot dangled: a later `map.get` returned
  freed (often overwritten) memory. Silent data corruption, no
  diagnostic, liveness-sensitive (a `println` that kept the value live
  hid it). Reported by aeb, whose `build.fail(ctx, reason)` stored the
  reason into a nested status map and read garbage back later
  (`heap-string-map-value-use-after-free-multi-tu.md`; reproduces
  single-TU too, not only across cloned-import TUs as first thought).
  Fix: `callee_param_escapes_via_body` looks through the callee's body —
  if the parameter reaches an escaping sink (a `ptr`/`@retain` argument,
  a container put, transitively another storing wrapper, or a return),
  the argument escapes even when typed `string`, so the caller keeps the
  buffer alive. Read-only wrappers are unaffected (the walk bottoms out
  at externs like `string.length`, which don't escape). Recursion-guarded
  and conservative on overflow (over-marking is a leak, never a UAF).

### Upgrade notes

This release marks *more* heap-string values as escaping: a value handed
to a wrapper whose body stores it (into a map/list, a `@retain` sink, or
a return) is no longer freed at the caller's scope exit. The previous
compiler freed it there while the container still held the pointer — a
use-after-free returning garbage on a later read.

The change only ever *keeps a value alive longer*; it cannot turn
working code into a crash. If your project worked around this by storing
container values as literals only, or by routing reason/label text
through disk markers (as aeb did), you can revisit those workarounds —
the in-memory heap-value-into-container path is now read-safe. Note the
value is kept alive by *not freeing it at the caller*: when the container
doesn't own the value (a non-`@retain` `map_put_raw`/`list_add_raw`
path), it now leaks rather than dangles — the safe direction, tracked
separately in `heap-temp-into-owned-map-leak.md`. No pre-upgrade action
is required.

## [0.183.0]

### Added

- **`docs/bootstrap-from-source.md`** — a from-`HEAD` build/install guide
  for *consumers* (not contributors): clone → `make ae && make install`
  → optional `make contrib && make install-contrib` (built only where dev
  libs are present), with an explicit "you don't need to run the test
  suite to use Aether" note, the downstream `$(ae cflags)` /
  `import contrib.X` link contract, and a dedicated LLM/automation
  recipe. Linked from the README documentation index.

### Fixed

- **`std.http.client` now decodes `Transfer-Encoding: chunked` response
  bodies** (`std/net/aether_http.c`,
  `tests/integration/vcr_record_chunked/`). The client returned the raw
  socket body including chunk framing (`13\r\nhello-from-upstream\r\n0\r\n\r\n`)
  for any response without `Content-Length` — extremely common for
  streamed / unknown-length upstreams. The new `http_dechunk` decoder
  runs when the response carries `Transfer-Encoding: chunked`, so the
  body is the decoded payload; gated on the header, so Content-Length
  responses are untouched, and malformed framing falls back to the raw
  bytes. This was corrupting VCR record-mode tapes (the recorder stores
  the client's response body), where it surfaced
  (`vcr_record_chunked_dechunk_wish.md`, reported by servirtium-dotnet):
  the tape now stores — and replay serves — the decoded payload. The C
  decoder is binary-safe (copies by length) and skips chunk extensions /
  trailers. New end-to-end integration test drives a raw-socket chunked
  upstream through both the client and the VCR record→replay path.

## [0.182.0]

_Docs only._ CHANGELOG version-drift reconciliation — restored the
`[0.180.0]` and `[0.177.0]` sections the release-rename had mislabeled
when `[current]` was absent at tag time — plus
`docs/notes/changelog-release-drift-note.md`, a handoff for the release-workflow
owner. No code changes.

## [0.181.0]

### Added

- **VCR embedding C-ABI — host the Aether record/playback server behind
  an FFI layer** (`std/http/server/vcr/embed.ae`,
  `std/http/server/vcr/aether_vcr.c`, `tests/integration/vcr_embed_abi/`).
  A thin wrapper module over the existing `std.http.server.vcr` surface
  (no new VCR semantics) that adds the embedding seam the raw module
  lacked: it starts the accept loop on a detached background thread
  (`http_server_start_background_raw` — `load()` deliberately doesn't
  listen), binds *synchronously* first so an OS-assigned port (port 0)
  is resolved before the start call returns, and returns caller-owned C
  strings (every result duplicated via `vcr_embed_dup`; freed with
  `aether_vcr_embed_free_string`). Built `ae build --emit=lib --with=fs,net
  std/http/server/vcr/embed.ae`, it exports `aether_vcr_embed_*`
  (lifecycle `start_playback`/`start_record`/`stop`/`stop_and_flush`
  [`_fail_if_changed`], introspection `port`/`base_url`/`tape_length`/
  `reset_cursor`, diagnostics `last_error`/`last_kind`/`last_index`/
  `clear_last_error`, and the `redact`/`unredact`/`remove_header`/`note`/
  `static_content` + format/clear mutations). The `vcr_embed_` prefix is
  the embed-only namespace (a bare `vcr_` would collide at link with the
  C runtime's `vcr_*` symbols). A C-driver integration test dlopens the
  `.so` and round-trips a playback (`start_playback` → resolved port →
  `base_url` → raw-socket `GET /ok` → recorded body → `last_kind` →
  `stop`). v1 contract: one active VCR server per process; NUL-terminated
  strings (binary bodies remain the existing tape limitation). Closes
  `vcr_embed_abi_wish.md` Part B; unblocks servirtium-dotnet's P/Invoke
  wrapper.

### Changed

- **The precompiled runtime archive (`build/libaether.a`) is now built
  `-fPIC`** (`Makefile`, `tests/integration/emit_lib_net/`). Without it,
  any `ae build --emit=lib` that pulled in a TLS-using runtime object —
  notably `std.http`'s embedded server (`aether_http.o`, whose
  initial-exec TLS emits an `R_X86_64_TPOFF32` relocation illegal in a
  shared object) — failed to link with `recompile with -fPIC`. The
  `--emit=lib` examples only imported `std.map`, so the gap went
  unexercised. Now `ae build --emit=lib --with=net` over a `std.http`
  module produces a loadable `.so` that `dlopen`s and round-trips
  (new `emit_lib_net` C-driver integration test). Codegen cost is
  negligible on x86-64/arm64 (most distros default to PIE); one archive
  serves both the exe and shared-object links. Unblocks
  `vcr_embed_abi_wish.md` Part A (servirtium-dotnet's embedded VCR `.so`).

### Added

- **`http_server_port(server) -> int`** (`std/net/aether_http_server.c`,
  `std/http/module.ae`, `std/net/module.ae`). Reports a server's
  resolved listening port. `http_server_bind_raw` now `getsockname()`s
  the socket after binding with port 0 (OS-assigned) and writes the
  kernel-chosen port back, so dynamic-port users — parallel test runners,
  the planned VCR embed layer — can discover the real port instead of
  reading back the `0` they requested (`vcr_embed_abi_wish.md` open
  question 3).

### Fixed

- **A user function that forges an imported export's mangled C symbol is
  now rejected at compile time** (`compiler/aether_module.c`,
  `tests/integration/import_export_collision_reject/`). A module export
  `ns.name` mangles to the flat C symbol `ns_name`; because `_` is a
  legal identifier character, a user top-level function literally named
  `ns_name` forges the *same* symbol. The module merge's
  `program_has_function` dedup then silently skipped cloning the import,
  so every `ns.name(...)` call bound to the user's function — clean
  compile, clean link, wrong function at runtime. This cost the avn port
  a multi-session "irreducible heap corruption" hunt: a local
  `proxy_opts_new()` shadowed `proxy.opts_new()` (both → `proxy_opts_new`),
  so `std.http.proxy`'s `mount_match` received a garbage opts pointer and
  segfaulted far from the cause
  (`user-identifiers-must-not-collide-java-style-scoping.md`). The new
  `check_namespace_prefix_collision` pass runs after imports are resolved
  but before merge (when the program's top-level functions are still
  exactly the user's) and emits a loud `error[E1001]` naming both
  symbols and the fix — the general form of the builder-vs-function
  (0.178.0) and selective-import-shadow (#436) rejections. This is the
  spec's "minimum fix" backstop (silent wrong-dispatch → 5-second
  compile error); full Java-style internal symbol qualification of every
  non-exported function is the larger follow-on, and it would interact
  with the `--emit=lib` ABI and the new binary-import resolver, so it is
  intentionally deferred.

- **Aliasing a live heap string into a loop variable no longer corrupts
  the original** (`compiler/codegen/codegen_stmt.c`,
  `tests/regression/test_alias_reassign_keeps_source_live.ae`). The
  alias-ownership "move" for `dest = src` (where `src` is a heap-tracked
  local) transferred `src`'s single freeing duty to `dest` and disowned
  `src` (`_heap_src = 0`). That move is a last-use optimization — sound
  only when `src` is dead after the alias. When `src` was read again
  (e.g. `content` read after a `rest = content` substring-reassign
  loop), the loop's next `rest = …` reassignment freed the buffer rest
  had taken from `content`, so `content` dangled — a silent
  use-after-free returning truncated/garbage bytes, no crash, no
  diagnostic. The move was always latent; 0.175's tuple-destructure heap
  classification began tagging `content, _ = io.read_file(…)` as
  heap-owned, which is what started triggering it (it broke aeb's
  `tools/extract-deps` scan/glob expansion the moment those tools were
  rebuilt under 0.180 — reported in `180-regression.md`). Fix: a
  conservative liveness check — when the aliased source is referenced
  anywhere besides the alias itself, codegen now emits a binary-safe
  defensive copy (`aether_uniform_heap_str(src, 0)`) instead of stealing
  the buffer, so `dest` owns an independent copy and `src` survives for
  its later reads. This is exactly the behaviour of the hand-written
  `rest = string.concat(content, "")` workaround, now automatic. The
  move optimization is preserved for the genuine last-use shape
  (`sorted = new_sorted; return sorted`), where `src` has no later use.

### Upgrade notes

This release loosens alias ownership for `dest = src` heap-string
assignments: when the source is still read after the alias, the alias
takes a defensive copy instead of moving (stealing) the source's buffer.
The previous version always moved, which freed the source out from under
its own later reads — a use-after-free, not a leak.

This change only ever *adds* a copy where a move was unsound; it cannot
flip working code into a crash. Downstream projects that carried a
defensive `rest = string.concat(content, "")` (or similar `+ ""`) copy
specifically to dodge this corruption can drop it — the compiler now
emits the copy itself, only where it is actually needed. There is no
required pre-upgrade action.

### Added

- **Consume a precompiled `--emit=lib` artifact as a first-class Aether
  `import`** (`tools/ae.c`, `tests/integration/binary_import/`). When
  `ae run` / `ae build` sees an `import foo` with no `foo` source module
  but a `libfoo.so` / `foo.so` on the search path, it reads that
  artifact's `aether_lib_meta()` catalog (the v2 closure-context schema)
  and synthesizes an Aether interface stub: an `@extern("<c_symbol>")`
  declaration for each function export and a trailing-block `builder`
  wrapper for each builder entry point (forwarding the block's config
  map to the library's `(..., _builder)` symbol). The stub is placed on
  the module search path, so the existing source-import machinery
  (typecheck, namespace prefixing, builder registration) rehydrates the
  library with full call-site fidelity — `foo.greet(x)` and the builder
  DSL `foo.section(t) { … }` read as if compiled in the same cycle. The
  artifact is linked in (absolute path + `-rpath`); the host's
  `-rdynamic` + static `libaether` satisfy its runtime symbols. This is
  the consumer half of the Aether-by-Aether library DX (the producer
  half is the v2 closure-context records, same release). Scope:
  function exports and builder DSL entry points are callable (a builder
  consumer also `import std.map` for the default config factory);
  higher-order exports taking a closure parameter are described in the
  metadata but not yet callable across the boundary (follow-on); POSIX
  only (gated on `dlopen`), Windows DLL hosting is a follow-up.

- **`--emit=lib` metadata now carries v2 closure-context records**
  (`runtime/aether_lib_meta.h`, `compiler/codegen/codegen.c`,
  `tools/ae.c`, `tests/integration/lib_meta_closures/`). The
  `aether_lib_meta()` catalog's previously-reserved `closure_count` /
  `closures` slots are now populated (schema bumps "1.0" → "1.1" when
  any closure record is present). Each `AetherLibClosure` describes a
  closure surface reachable from an export — the part the flattened C
  ABI drops — so a downstream *Aether* consumer can reconstruct a
  closure-with-context builder DSL at full fidelity (the "config IS
  code" / Groovy-grade library DX goal). Three record roles:
  `builder` (an export taking an injected `_ctx`, i.e. a trailing-block
  DSL entry point), `param` (a closure-typed `fn` parameter, rendered
  with its `|...| -> R` signature), and `literal` (a hoisted closure in
  an export's body, with the enclosing variables it captures and their
  Aether types). Records are computed over the full set of top-level,
  non-imported, non-`_`-suffixed functions — broader than the ABI
  function table, since a builder or `fn`-parameter function is exactly
  what the ABI gate excludes yet still has a linkable bare symbol.
  Everything stays `static const` in `.rodata` (no allocation, no
  parsing), and the layout is append-only so a "1.0" reader walks a
  "1.1" artifact unchanged. `ae lib-info` prints the closure surface
  (role, enclosing export, signature, captures). Closure-literal return
  types are best-effort (`?` when the body type-checks lazily);
  captures and parameter types are exact.

- **Regression coverage for the `std.map` opts-context → `std.http.proxy`
  FFI path** (`tests/regression/test_proxy_opts_map_mount.ae`). The avn
  project's closures-with-context builder rollout filed this as "Blocker
  2": avnproxy's builder/opts-map variant aborted at startup inside
  `aether_proxy_use_reverse_proxy_match` with `free(): invalid pointer`,
  while eleven hand-reduced isolated reproducers all passed valgrind
  clean — it only fired with the full combination of traits. The root
  cause was the `map_put_raw` → `map_put_string_owned` codegen misroute
  fixed in 0.179.0 (`test_map_put_raw_literal_value.ae`): a borrowed /
  non-heap string value put into a map was tagged owned, so `map.free`
  `free()`d a non-heap pointer, and where that pointer aliased state the
  proxy lib later touched, the corruption surfaced far away as an invalid
  free inside `mount_match`. With that misroute fixed, the whole opts-map
  → proxy path is clean. This test reproduces avnproxy's startup sequence
  (borrowed/heap URLs and pool pointers funnelled through a `std.map`,
  then `mount_match` + `mount_methods`, including the internal pool-rebind
  path that the backtrace aborted in) and runs clean under ASan — no
  use-after-free, no double-free. Unblocks the avn closures-with-context
  builder rollout (`closures-with-context-builder-blockers.md`).

## [0.180.0]

### Fixed

- **Trailing commas are now tolerated in `exports (…)` and selective
  import lists** (`compiler/parser/parser.c`,
  `tests/integration/trailing_comma_lists/`). `exports (a, b, c,)` and
  `import mod (a, b,)` previously emitted a spurious
  `error[E0100]: Expected IDENTIFIER, got RIGHT_PAREN` — the
  `do { expect IDENTIFIER } while (match COMMA)` loop re-entered after
  the trailing comma and hit `)`. Because the parser recovered and
  exited 0, the failure was intermittent: any program importing
  `std.http.proxy` (whose export list ends with a trailing comma)
  printed the error, mis-located onto the *importer's* file path with
  the proxy module's line number, and downstream build chains saw it as
  a "nondeterministic E0100 on rebuild." Reported by the avnproxy port
  (closures-with-context-builder-blockers.md, Blocker 1).

## [0.179.0]

### Added

- **Aether functions can now be C-variadic and consume their own
  varargs** (`compiler/parser/parser.c`, `compiler/analysis/typechecker.c`,
  `compiler/codegen/codegen_func.c`, `compiler/codegen/codegen.c`,
  `compiler/codegen/codegen_expr.c`,
  `tests/regression/test_va_list_consumer.ae`). A function declared with
  a trailing `...` (`f(fmt: string, ...)`) emits a C `...` signature; its
  body reads varargs via three intrinsics: `va_start()` (yields an opaque
  cookie; the prologue emits `va_list __ae_va; va_start(__ae_va, <last
  named param>)`), `va_arg(vap, T)` (lowers to `va_arg(*(va_list*)vap,
  ctype)`), and `va_end(vap)`. The call-site arity check accepts any
  trailing args beyond the named params for such callees. Closes #536;
  lets C ports (e.g. mquickjs's `js_vprintf` + its `mqjs_va_arg_*`
  helpers) move their printf-family routines fully to Aether.

### Fixed

- **The series-collapse loop optimizer no longer folds a loop whose body
  consumes varargs** (`compiler/codegen/codegen_stmt.c`). `va_arg` /
  `va_start` / `va_end` are now treated as side-effecting in
  `expr_has_side_effects`, so a `total = total + va_arg(vl, int)`
  accumulator loop is emitted as a real loop instead of a closed-form
  `addend * trip_count` (which read only one vararg). Surfaced while
  landing #536.

## [0.178.0]

### Added

- **Taking the address of an Aether function** is confirmed supported
  via the existing `funcname as fn(T1, ...) -> R` cast — applying it to
  a bare function name yields a real C-ABI function pointer (`ptr`),
  passable to `qsort`, callback tables, etc.
  (`tests/regression/test_fn_address_via_as_fn.ae`). No compiler change;
  this locks in the capability with a regression test so C ports can
  drop their `*_addr()` shim helpers.

- **`sizeof(T)` / `offsetof(T, field)` compile-time layout builtins**
  (`compiler/ast.h`, `compiler/parser/parser.c`,
  `compiler/analysis/typechecker.c`, `compiler/codegen/codegen_expr.c`,
  `tests/regression/test_sizeof_offsetof.ae`). `sizeof(StructName)` and
  `offsetof(StructName, field)` lower to C's own
  `((int)sizeof(struct StructName))` / `((int)offsetof(struct StructName,
  field))`, so the value can never disagree with the layout the C
  compiler actually produces. Motivating case: ports that mirror a C
  struct as an `extern struct` overlay previously hand-maintained byte
  offsets, which silently drifted from the real layout (the mquickjs
  port shipped a stale `344` where the field had moved to `138`,
  corrupting the heap). Spelled as call syntax over an identifier — not
  reserved words — so only the `sizeof(` / `offsetof(` shape triggers
  them.

### Fixed

- **A `builder` and a plain function sharing a name are now rejected at
  typecheck time** (`compiler/analysis/typechecker.c`,
  `tests/integration/builder_function_collision_reject/`). Both mangle
  to the same C symbol (`<module>_<name>`); previously one silently won
  and every call dispatched to it — clean compile, clean link, wrong
  function body, exit 0 (cost ~1h debugging an aeb `lib/ruby` build).
  Now emits `duplicate definition of '<name>': a builder and a function
  cannot share a name (both emit the same C symbol)` with the prior
  definition's line. Narrowed to builder-vs-function: two plain
  functions sharing a name remain legal (the multi-clause
  pattern-matching form).

## [0.177.0]

### Added

- **`std.mem.get_byte_sz` / `std.mem.set_byte_sz` provide size_t-indexed byte
  access** (`std/mem/module.ae`, `std/mem/aether_mem.c`,
  `tests/integration/std_mem_byte_access/`). These mirror `get_byte` /
  `set_byte` but take a `size_t` offset, avoiding one-off C shims in ports
  whose natural byte index is already `size_t`.

- **`std.http.proxy` can route by HTTP method and route pattern**
  (`std/http/proxy/module.ae`, `std/http/proxy/aether_proxy_middleware.c`,
  `tests/integration/http_reverse_proxy_pool/`). New `proxy.mount_methods`
  filters a proxy mount by a comma-separated method list, and
  `proxy.mount_match` combines a method list with std.http route-pattern
  syntax such as `/repos/:repo/info`. Non-matches pass through to later
  middleware, so first matching mount wins.

### Fixed

- **`@c_import` struct pointers now emit `struct Name*` for typedef-less C
  headers** (`compiler/codegen/codegen.c`, `compiler/codegen/codegen_expr.c`,
  `compiler/codegen/codegen_func.c`, `tests/integration/c_import_struct/`).
  Header-owned structs still emit no Aether typedef, but pointer casts,
  extern returns and extern parameters no longer assume the header also
  declares `typedef struct Name Name;`.

## [0.176.0]

### Fixed

- **`http_parse_request` preserves binary request bodies through embedded NULs** (`std/net/aether_http_server.c`, `std/net/aether_http_server.h`, `tests/runtime/test_http_server.c`). The server-side HTTP request parser was reading the body via `strdup(header_start)` and storing `strlen(req->body)` as `body_length`, both of which stop at the first NUL inside the body. Binary payloads — `Content-Encoding: x-lzf`, image uploads, gzip-compressed JSON, any `application/octet-stream` — arrived at the application handler truncated at their first internal `\x00`, even though the server's connection loop had already read the full `Content-Length` bytes off the wire and into its buffer. New length-aware entry point `http_parse_request_n(buf, len)` consults the parsed `Content-Length` header, clamps it against `len - (header_start - buf)` (so a client-supplied `Content-Length: 99999` with a 19-byte body can't memcpy past the actual buffer — defends against both buggy and malicious clients), and `memcpy`s exactly the clamped byte count. The string-shaped `http_parse_request(raw)` is preserved as a thin wrapper around `_n(raw, strlen(raw))` for legacy text-only callers. The wire-protocol accept loop now calls `_n` directly with the authoritative `request_total` it already knows. `http.request_body_length(req)` (already exposed as the binary-safe accessor — see `tests/syntax/test_http_request_body_length.ae`) now reports the wire-correct length end-to-end; `http.request_body(req)` returns a buffer the caller can `memcpy` `body_length` bytes from. Surfaced by the avn port (`std-http-body-truncates-at-nul.md`) where the gh-issue #2 lzf-wire-envelope smoke test rejected a 237-byte compressed POST body as 112 bytes server-side. Two new regression tests: `http_server_post_binary_body_with_embedded_nul` parses a hand-built request whose 12-byte body has three embedded NULs and asserts `body_length == 12` plus a full-buffer `memcmp`; `http_server_post_with_oversized_content_length_clamps_safely` exercises the clamp path (Content-Length: 25, body 19 bytes) — the test that caught the bug under ASan during the original PR review.

## [0.175.0]

### Fixed

- **`fs.read_binary` no longer leaks the bytes buffer at scope exit** (`std/fs/module.ae`, `std/fs/aether_fs.c`, `compiler/codegen/codegen_stmt.c`, `tests/regression/test_fs_read_binary_tuple_heap_reclaim.ae`). Three load-bearing fixes:

  1. `extern fs_read_binary_tuple(...) -> (ptr, int, string)` became `(string @heap, int, string)`. Position 0 was typed `ptr` but the C side returned an owned `AetherString*` from `string_new_with_length`; the tuple-destructure codegen (`AST_TUPLE_DESTRUCTURE` handler) consults `tuple_heap_flags[j]` populated by the parser only when an `@heap` annotation is in scope, so `ptr` carried no flag and every `bytes, _, _ = fs.read_binary(p)` site leaked its payload at scope exit. Matches the precedent set by `fs_realpath_raw`.

  2. `function_def_returns_heap_at`'s whole-tuple-passthrough handler (`return some_extern(...)`) only consulted user function definitions when resolving the callee. An extern callee — even one carrying `@heap` tuple-position flags — fell through to a hard veto, so every user-facing wrapper of the shape `f(x) -> { return some_extern(x) }` silently dropped its callee's heap classification. The handler now also consults `find_extern_declaration_by_name` and reads the extern's `tuple_heap_flags[position]` directly. Without this second fix, the `string @heap` annotation on `fs_read_binary_tuple` had no effect at `fs.read_binary` call sites — the wrapper was the only caller in-tree.

  3. `fs_read_binary_tuple` was calling libc `free(buf)` on a buffer that `fs_read_binary_raw` had allocated through `aether_caps_malloc` (with capacity `len + 1`). That bypassed cap accounting, leaving `aether_caps_used_bytes()` climbing by one buffer per call even after the AetherString itself was being released correctly. Now `aether_caps_free(buf, len + 1)`, matching the alloc side.

  Reported by the avn port (`tuple-destructure-heap-classification.md`) where the leak compounded into O(N²) RSS retention in avnserver — ~800 MB resident at 5000 commits, driven by recursive directory-blob reads through `rep_read_decoded`. In-tree callers (`std/cas/module.ae` ×2, `tests/regression/test_string_eq_on_aether_string.ae`, `tests/regression/test_fs_atomic_rename_stat_binary.ae`) already used the bytes as a `string`; the typed-as-`ptr` cover was load-bearing only for the leak. Regression test asserts `aether_caps_used_bytes()` delta stays bounded (≤ 64 KB) after 200 reads of a 10 KB file — pre-fix would have grown by ~2 MB.

### Upgrade notes

This release tightens `fs.read_binary`'s ownership contract: the first tuple element (`bytes`) is now a heap-tracked `string` that the destructured LHS owns and that the runtime reclaims at scope exit or on destructure reassignment. The previous version mis-typed it as `ptr`, so the buffer leaked at every call site — a slow leak that nonetheless kept the underlying AetherString alive indefinitely.

If your project destructures `fs.read_binary` (or any other call that ultimately bottoms out in `fs_read_binary_tuple`) and then stashes the `bytes` value into a long-lived alias — a `map.put` value, a struct field, a list element, an actor message, or a return through a wrapper that doesn't itself own the value — the previous compiler tolerated this as a leak; this release will free the alias when the destructure scope exits, leaving the long-lived holder pointing at freed memory. This shape is a **use-after-free** on the next read of the alias.

The same hazard affects any user-defined wrapper whose body is `f(x) -> { return some_extern(x) }` and whose extern carries `@heap` tuple-position flags — those wrappers previously hard-vetoed heap classification (so every caller leaked) and now correctly propagate ownership. If you have such a wrapper *and* a downstream caller that aliases its destructured result into long-lived storage, the same UAF shape applies.

**Recommended pre-upgrade play:**

1. Run `aetherc --diagnose=ownership` against your project. The diagnostic reports per-local string-ownership verdicts and will mark `fs.read_binary`-derived destructure targets as heap-owned. Cross-reference any heap-owned local that gets passed to `map.put`, written to a struct field, returned without an explicit retention, or sent to an actor.
2. For each such site, decide between: (a) cloning the bytes into a fresh owned copy at the alias site (`string.copy` or `string_new_with_length` if you need a length-aware copy), or (b) holding the source destructure scope open for the alias's lifetime. The leak previously did (b) by accident; (a) is the explicit form.
3. Re-run your project under ASan or Valgrind after the upgrade. A clean run on the *previous* aetherc release tells you nothing — the leak was masking the UAF. Only a sanitiser run against the new release surfaces the real hazard.

## [0.174.0]

### Changed

- **`io.stderr_write` / `io.stdout_write` are now one-arg `(data: string) -> int`** (`std/io/module.ae`, `tests/regression/test_io_unbuffered_writes.ae`). The two-arg form `(data, length)` made the caller hand over a byte count that `string` already carries; the wrapper now computes `string.length(data)` itself. The raw externs picked up the `_raw` suffix every other raw extern in `std.io` uses — `io_stderr_write_raw(data: string, length: int)` / `io_stdout_write_raw(data: string, length: int)` — and remain the entry point for callers that genuinely need a partial-length write (binary slices, embedded NULs, "drop the trailing newline" patterns). BREAKING: callers of the two-arg form must drop the length or, if the partial length was load-bearing (e.g. `n - 1` to skip a trailing `\n`), slice the input first or call the `_raw` extern directly. Surfaced by the avn port (`io-stderr-write-arity-change-undocumented.md`), which caught one silent behaviour trap (a `n - 1` write deliberately stripping a trailing newline) among five mechanical fixes.

### Added

- **C ABI scalar aliases — `size_t`, `uint32_t`, `intptr_t`, `time_t`, …** (`compiler/ast.h`, `compiler/ast.c`, `compiler/parser/parser.c`, `compiler/codegen/codegen.c`, `compiler/codegen/codegen_func.c`, `compiler/codegen/codegen_stmt.c`, `compiler/analysis/typechecker.c`, `compiler/aetherc.c`, `tests/regression/test_c_abi_scalar_aliases.ae`). C ports need exact C type spellings so an Aether `extern` prototype matches the system header byte-for-byte; a merely-equivalent Aether type yields conflicting prototypes or a wrong varargs ABI. Recognised aliases: `int8_t`/`uint8_t` … `int64_t`/`uint64_t`, `intptr_t`/`uintptr_t`, `size_t`/`ssize_t`/`off_t`, `time_t`/`pid_t`/`mode_t`. An alias is a normal type carrying a new `c_alias` field — the verbatim C spelling — while its underlying `kind` drives all typechecking, arithmetic and promotion. `extern strlen(s: string) -> size_t` now emits `size_t strlen(...)`; a `uint32_t` struct field emits `uint32_t f;` (correct width and unsigned). Implements "P0: C ABI Scalar Aliases" from `docs/redis-porting-language-gaps.md`.

- **Typed and qualified C pointers — `extern type`, `const ptr`, `cstring`** (`compiler/parser/parser.c`, `compiler/codegen/codegen.c`, `compiler/codegen/codegen_func.c`, `tests/regression/test_c_typed_pointers.ae`). Aether `ptr` emits `void*`, too lossy for Redis/mquickjs APIs — it conflicts with header prototypes using `const void *`, `char *`, or a named struct pointer, so those C functions cannot be declared `extern` directly. New surface: `extern type Name` declares an opaque, header-defined C type (emits the incomplete forward typedef `typedef struct Name Name;` — `Name*` is a usable pointer, field access stays a compile error); `*Name` typed pointers are now also usable as typed local declarations, not just params/returns; `const <type>` is a C-qualified type (`const ptr` → `const void*`, `const *T` → `const T*`); `cstring`/`cstring_const` spell mutable `char*` and explicit `const char*`. The extern-prototype emitter honours these so a declaration matches the system header exactly. Implements "P0: Typed And Qualified C Pointers" from `docs/redis-porting-language-gaps.md`.

- **`extern struct Name @c_import { ... }` — header-defined C struct interop** (`compiler/parser/parser.c`, `compiler/codegen/codegen.c`, `compiler/codegen/codegen_func.c`, `Makefile`, `tests/integration/c_import_struct/`). `extern struct` describes an Aether-*owned* C layout and emits a `typedef struct Name { ... } Name;` for it — which collides with the header when the struct (Redis `client`, `robj`, `dict`, …) is already defined in a C header the build includes. The `@c_import` attribute marks a struct whose layout is *imported from a C header*: Aether typechecks field access and `*Name` overlays against the declared fields, but the generated C emits no struct body and no typedef (not even the forward `typedef struct Name Name;`), so it never conflicts with the header. Field reads/writes emit ordinary `c->field` C against the header-defined type. Works across Aether module imports. Implements "P0: Header-Defined C Struct Interop" from `docs/redis-porting-language-gaps.md`.

- **`const NAME[] = [...]` — top-level static const array declarations** (`compiler/parser/parser.c`, `compiler/codegen/codegen.c`, `compiler/codegen/codegen_stmt.c`, `compiler/analysis/typechecker.c`, `tests/regression/test_const_array.ae`, `tests/regression/test_const_array_int.ae`). Adds a static-data form: `const atoms[] = ["null", "false", "true"]` at top level emits `static const char* atoms[] = {"null", "false", "true"};` in the generated C; `const sizes[] = [4, 8, 16]` emits `static const int sizes[] = {4, 8, 16};`. The existing scalar `const` form (`const N = 42` → `#define N (42)`) is unaffected. Indexed access (`atoms[i]`) works on both. Motivating use case: the mquickjs port's `atoms[]` predefined-atom table, which was the last data table blocking the move of `mquickjs_build.c` contents to Aether.

## [0.173.0]

### Added

- **`extern struct` accepts `union { ... }` and nested `struct { ... }` fields** (`compiler/parser/parser.c`, `compiler/parser/lexer.c`, `compiler/parser/tokens.h`, `compiler/ast.h`, `compiler/ast.c`, `compiler/codegen/codegen_func.c`, `compiler/analysis/typechecker.c`, `tests/regression/test_extern_struct_union.ae`). C structs that span an FFI boundary often contain unions — the mquickjs `JSPropDef` has six variants overlayed on the same 32-byte tail, and every Aether port of a C tagged-union shape (Lua `TValue`, JSON value, etc.) hits the same wall. Before this, porters declared one `u_first_ptr: ptr` field at the union's start offset and reached into the rest with `mem.get_byte(d, 16)` / `mem.get_float64(d, 16)` / `mem.get_ptr(d, 24)` — field names and union shape both lost. Surface: `u: union { f64: float, str: ptr, func: struct { length: byte, magic: ptr, ... } }`. Access via dotted notation: `d.u.f64` or `d.u.func.length`. C lowering emits a real `union { ... }` (and nested `struct { ... }` where present), so layout matches what any C author would write by hand. `union` is now a reserved keyword — only meaningful inside `extern struct`, since pure-Aether structs disallow it (would undermine memory safety). The typechecker carries a borrowed `compound_node` pointer on `TYPE_STRUCT` types representing anonymous compounds so chained access (`expr.u.func.length`) resolves without a global lookup.

- **`expr as T[]` — typed array view over a raw pointer** (`compiler/parser/parser.c`, `compiler/ast.h`, `compiler/codegen/codegen_expr.c`, `compiler/analysis/typechecker.c`, `tests/regression/test_extern_ptr_as_array.ae`). The `as` family already covers `as *StructName` (struct overlay) and `as fn(T1, T2) -> R` (function-pointer view). The third axis — typed element-pointer view — was missing, forcing every malloc'd typed buffer in a port to be indexed via `mem.set_int(buf, 4*i, v)` boilerplate. Now: `raw = malloc(4 * n); arr = raw as int[]; arr[i] = v`. Lowers to `((int*)(raw))[i] = v`, which C scales by `sizeof(T)` automatically — no offset arithmetic at user sites. No allocation, no bounds check; same trust-the-author posture as `as *T`. Covered element types: `int`, `byte`, `long`, `ptr`, `float`. The `mquickjs_build_define_props` function in the mquickjs port allocates three int-arrays (`ident_tab`, `hash_table`, `prop_hash`); each is a candidate site for this cast on the next pass.

### Fixed

- **`make install` / `install.sh` ship `contrib/host/<lang>/aether_host_<lang>.c` bridge sources** (`Makefile`, `install.sh`, `docs/install-layout.md`, `tests/integration/install_manifest/test_install_host_bridges.sh`). The install ran `find $PREFIX/share/aether/contrib -name '*.c' -delete` to trim source-tree noise — but that swept the host-bridge `.c` files with it. Result: a plain installed tree could resolve `import contrib.host.python` (the `module.ae` + `.h` shipped) but the link step had no `aether_host_python_*` symbols available. `make install-contrib` ships the matching `libaether_host_<lang>.a` archives, but that's a separate opt-in step gated on the per-language dev libraries — the plain-install user had no path to a link-suitable bridge. The trim now exempts `*/contrib/host/*/aether_host_*.c` (one find expression, not a behavioural carve-out for downstream consumers) so the 9 in-tree bridges (js, lua, perl, python, ruby, tcl, go, tinygo, aether) ship as source. No other contrib `.c` file slips through. `docs/install-layout.md` documents the carve-out alongside a new section on the `-DAETHER_HAS_SANDBOX` compile flag downstream consumers need to define when building against the shipped `libaether.a`.

## [0.172.0]

### Fixed

- **`switch` arms now break by default — no silent C-style fallthrough** (`compiler/codegen/codegen_stmt.c`, `tests/regression/test_switch_no_fallthrough.ae`). The parser had `switch`/`case`/`default` wired up since the language's first weeks, but the codegen emitted C `switch` with no `break;` at the end of each case — so any switch whose arms didn't all `return` fell through to `default` for every input. Only the early-return shape worked. Aether's `switch` is now no-fallthrough by default: codegen auto-inserts `break;` at the end of each case unless the tail statement is itself a control-flow exit (`return`, `break`, `continue`). This matches what every author has written so far — there are no existing `switch` statements anywhere in `std/` or `tests/regression/*.ae`, so no callers needed updating. The fall-through-by-default-because-C habit was a footgun; this lays it to rest.

### Tests

- **`tests/regression/test_for_loop_c_style.ae`** pins the C-style `for (init; cond; step) body` form. The parser has accepted it for a long time (`compiler/parser/parser.c:parse_for_loop`) but it was never exercised end-to-end. Now covered: typed init / inferred init / pointer-stride step. The empty-step form `for (init; cond;) { body }` is known-bad — the parser greedily pulls the next body statement into the step slot — and is documented as out-of-scope for this test; tracked separately.

### Added

- **`std.mem` gains bulk and endian helpers** (`std/mem/aether_mem.c`, `std/mem/module.ae`, `tests/regression/test_std_mem_bulk_endian.ae`). Per `docs/redis-porting-language-gaps.md` §P2: `mem.copy` / `mem.move` / `mem.compare` / `mem.set` cover libc `memcpy`/`memmove`/`memcmp`/`memset` over caller-owned buffers, and `mem.get_u{16,32,64}_{le,be}` / `set_u{16,32,64}_{le,be}` give unaligned-safe little- and big-endian load/store at a byte offset. The Redis port had been hand-rolling byte loops or reaching for libc shims for every ziplist / listpack / RDB binary walk; these wrappers cover both with one-line call sites. 32- and 64-bit loaders return `long` so the full unsigned range is representable — narrowing to Aether `int` truncates the same way C `(int32_t)u32` does. Each helper guards against NULL `p`.

### Fixed

- **`extern` declarations for ~50 more libc functions no longer collide with glibc prototypes** (`compiler/codegen/codegen_func.c`, `tests/regression/test_extern_libc_whitelist.ae`). The libc-conflict whitelist in `extern_name_is_libc_conflict()` was originally narrow (`strlen`/`strcmp`/`strcpy`/`strncpy`/`strcat` only). Calling other libc functions from Aether — `strdup`, `qsort`, `strstr`, `strchr`, `atoi`, `getenv`, `fwrite`, `strtol`, etc. — emitted a forward declaration derived from the Aether `extern` signature (`void* strdup(void*)`) which the libc header in the same TU refused (glibc declares `char* strdup(const char*)`). Every Aether-on-libc port had to wrap such calls in a one-line C shim. The whitelist now covers the common cases — process / signal (`fork`, `getpid`, …), stdio (`fread`, `fwrite`, `fseek`, …), the `str*` family (`strdup`, `strstr`, `strchr`, …), number conversion (`atoi`, `strtol`, `strtod`, …), `qsort` / `bsearch`, env access, POSIX file ops, time — so the user's `extern foo(...)` is still useful for call-site type-aware emission while the libc header's authoritative prototype reaches the C compiler. Filed by the mquickjs port (phase-3 of `mquickjs_build.c` to Aether), where it retired two C shims for `strdup` and `qsort` in `mquickjs/mquickjs_build.c`.

## [0.170.0]

### Fixed

- **Putting a non-heap string value into a `std.collections` / `std.map` map (or `std.list`) no longer crashes at container teardown** (`compiler/codegen/codegen_expr.c`, `compiler/codegen/codegen.c`, `compiler/codegen/codegen_stmt.c`, `tests/regression/test_map_put_raw_literal_value.ae`). The codegen auto-routes a string value into the owning variant (`map_put_string_owned` / `list_add_string_owned`) so a heap value is released at `map.free` — issue #467. The routing was gated on `is_heap_string_expr`, which for a bare identifier answers "is this name heap-*tracked*" — true for *every* assigned string variable, including one that only ever holds a literal (`sentinel = "1"`). Such a variable was tagged owned, and `map.free` then `free()`d a `.rodata` literal: `free(): invalid pointer` / `double free or corruption`. It hit both the raw API (`map_put_raw`) and the `map.put` wrapper, and the runtime `_heap_<name>` flag could not rescue it — a map/list value has escaped into the container call, so the reassignment wrapper that maintains the flag is suppressed and it reads stale. Filed by the avn project (`map-put-raw-rewritten-to-owned.md`); avn's `working_copy.wc_status` builds a tracked-path set with a dummy literal value and aborted every `avn status`. The owned-routing now resolves a bare identifier *structurally* — via `body_assigns_var_from_heap`, which is reliable for escaped variables — and only routes it to the owning variant when some assignment in the enclosing function body sets it from a heap source. A literal-only variable stays on the non-owning path; a genuinely heap variable still routes to owned, so #467's leak fix is preserved. Direct heap expressions (`string.concat(...)`, interpolation) are unaffected — they were always exact.

## [0.168.0]

### Fixed

- **The heap-source sweep is finished — `string_substring_n` and `string.from_int` / `_long` / `_float` / `_char` no longer leak** (`compiler/codegen/codegen_stmt.c`, `tests/integration/string_substring_n_no_leak/`). `string-new-with-length-heap-annotation.md` closed by asking for a sweep of every runtime function that mints a fresh owned buffer; only `string_new_with_length` was made intrinsic. `string_substring_n` was the next to bite — it returns a fresh `malloc`'d buffer (identical shape to the already-recognised `string_substring`) but was not in `is_heap_string_expr`'s set, so `x = string.substring_n(...)` was never freed and a `return string.substring_n(...)` wrapper leaked at every caller (avn's residual ~77 KB/commit RSS ramp — `util.slice`). `string_substring_n`, `string_from_int`, `string_from_long`, `string_from_float` and `string_from_char` are now all recognised heap sources (each `string_from_*` `string_new(...)`s a fresh refcounted AetherString). Filed by the avn project (`new_string_len_something.md` §1). Integration probe bounds RSS over 8000 wrapper-return + direct-call cycles.
- **Two closures in the same scope each declaring a heap-tracked local of the same name now compile** (`compiler/codegen/codegen_expr.c`, `tests/regression/test_closure_heap_tracker_two.ae`). Each closure body is its own C function and needs its own `int _heap_<name>` tracker declaration, but the codegen deduped tracker names across the whole enclosing function: the first closure got `int _heap_<name> = …;`, the second emitted a bare `_heap_<name> = …;` with no declaration in scope — a hard C compile error (`'_heap_<name>' undeclared`). The closure-body codegen now saves and resets the heap-string set per closure body, mirroring the existing per-closure reset of the declared-vars set. Filed by the avn project (`new_string_len_something.md` §2).
- **A closure that captures a variable whose name collides with a libc symbol (`dup`, `read`, `bind`, …) now compiles** (`compiler/codegen/codegen_expr.c`, `tests/regression/test_closure_captured_libc_name.ae`). The closure-capture lowering pushed captured-variable names through `safe_c_name` — which renames libc-colliding identifiers (`dup` → `ae_dup`) — at some emission sites (the env struct field, the prologue alias, the `_aether_make_closure` parameter) but not others (the closure body's use site and the construction-site argument, which stayed raw). The result was `ae_dup` referenced where only `dup` was declared: `error: 'ae_dup' undeclared`. `safe_c_name` is correct for an emitted *function symbol* (a real link-time collision), but a captured variable is a plain C struct field / local / parameter that needs no rename — it is now emitted raw and consistently across every capture site. Filed by the avn project (`new_string_len_something.md` §3).
- **Declarations inside a closure body — plain or tuple-destructured, at the top level or nested in an `if` / `while` — now type-check** (`compiler/analysis/typechecker.c`, `tests/regression/test_tuple_destructure_in_closure.ae`, `tests/regression/test_closure_nested_decl_scope.ae`). The typechecker's `AST_CLOSURE` handler walked the closure body with `typecheck_expression`, which has no statement cases — a declaration hit only the generic child-walk and its name was never registered in the closure scope, so every later use was `E0300 Undefined variable`. The 0.167.0 fix patched one statement kind at the body's top level (`AST_TUPLE_DESTRUCTURE`, per `tuple-destructure-in-closure-scope.md`); a plain `x = e` nested inside an `if` within the closure still slipped through (`new_string_len_something.md` §4). The closure body is now type-checked entirely through `typecheck_statement` — which registers declarations (plain and tuple alike) and recurses correctly into `if` / `while` / `for` / nested blocks — closing the class and subsuming the 0.167.0 narrow fix. Surfaces only for a non-`main` enclosing function, since `main`'s body already reaches a closure statement-wise; bundling tools that rename test files' `main()` to a per-file function hit it across a whole test suite. Reported by the avn project.

### Upgrade notes

This release continues the heap-ownership tightening of 0.167.0: more
runtime functions that mint a fresh owned buffer are now recognised heap
sources — `string_substring_n` and `string.from_int` / `_long` / `_float`
/ `_char` (joining `string_new_with_length` from 0.167.0). A value built by
one of these is now freed when the variable holding it is released; the
previous compiler classified those values non-heap and never freed them —
a silent per-call leak.

If your project takes such a value (`piece = string.substring_n(...)`,
`s = string.from_int(n)`, …) and also stores it into a longer-lived
structure (a struct field, list, or map) while keeping the original local —
which is later reassigned or goes out of scope — the previous compiler
tolerated this as a leak that kept the aliased buffer alive forever; this
release frees the buffer when the local is released, so the stored alias
becomes a dangling pointer. This shape is a use-after-free.

**Recommended pre-upgrade play:**

1. Grep for `string.substring_n` and `string.from_int` / `_long` /
   `_float` / `_char`, plus any user function that returns one of these
   (or a `string_new_with_length` value) in a tuple position.
2. At each, check whether the resulting string is stored into a structure
   that outlives the original local. If so, that structure must own an
   independent copy, not the alias.
3. Rebuild under the new compiler and exercise the affected paths under
   AddressSanitizer or valgrind — a freed-then-read alias surfaces
   immediately as a use-after-free rather than as the old slow leak.

## [0.167.0]

### Fixed

- **`string_new_with_length`-built return values no longer leak at every call site** (`compiler/codegen/codegen_stmt.c`, `tests/integration/string_new_with_length_no_leak/`). The 0.161.0 heap-ownership fix annotated `aether_bytes_finish` as a heap source, but `bytes.finish` is not the only AetherString constructor: `string_new_with_length` is the length-aware one (`bytes.finish` is itself `string_new_with_length` + copy), and `std.zlib` / `std.cryptography` / `std.lzf` / `std.fs` build their decode results with it directly, declaring the extern `-> ptr` with no `@heap`. The classifier never recognised it, so a function like `zlib.deflate` — `owned = string_new_with_length(raw, n); return owned, n, ""` — produced a tuple return whose string position was classified non-heap, and every caller leaked the decoded buffer (avn's commit bench ramped ~336 KB/commit through `zlib.deflate` and `crypto_b64_decode`). Reported by the avn project (`string-new-with-length-heap-annotation.md`, the follow-up to `heap-ownership-return-propagation.md`). Two coupled gaps closed: (1) `string_new_with_length` is now an intrinsic heap source in `is_heap_string_expr` — recognised by name like `string_concat` and interpolation, regardless of how a module spells the extern, so the next `-> ptr` declaration can't silently reintroduce the leak; (2) the per-position tuple-return classifier (`function_def_returns_heap_at`) was a strict AND-fold that classified a string position non-heap whenever *any* return site was a borrowed literal — exactly the `return owned, n, ""` vs `return "", 0, "err"` shape of every real decode function. It is now an OR-fold, and a matching `emit_tuple_return_position` routes each heap-classified position through `aether_uniform_heap_str` on *every* return path, so the literal error-path returns are malloc-duplicated and the caller frees uniformly — the same uniform-heap contract the single-value return path already ran. Integration probe runs 8000 interleaved success/error encode cycles and bounds RSS growth.
- **Tuple destructure inside a closure body now type-checks when the closure is enclosed by a non-`main` function** (`compiler/analysis/typechecker.c`, `tests/regression/test_tuple_destructure_in_closure.ae`). A `a, b, c = f()` destructure inside a closure body reported `E0300 Undefined variable` on every later use of the destructured names — but only when the closure was lexically inside a function other than `main`. The typechecker's `AST_CLOSURE` handler walked the closure body with `typecheck_expression`, which has no `AST_TUPLE_DESTRUCTURE` case, so the statement hit only the generic child-walk and the LHS names were never registered in the closure scope. A plain assignment registered fine (the handler special-cased `AST_VARIABLE_DECLARATION`), and a destructure at a function body's top level went through `typecheck_statement` and so worked — the gap was specifically destructure × closure. It surfaced only for a non-`main` enclosing function because `main`'s body is reached through `typecheck_statement`, so a `main`-level closure's body was already typechecked statement-wise; bundling tools that rename test files' `main()` to a per-file function flipped every closure-body tuple destructure into the failing case. Reported by the avn project (`tuple-destructure-in-closure-scope.md`). The closure-body loop now routes `AST_TUPLE_DESTRUCTURE` statements through `typecheck_statement`, whose existing handler arity-checks the RHS and registers each LHS name in the closure scope.

### Upgrade notes

This release tightens heap-string ownership: a function that builds a tuple
return value with `string_new_with_length` — directly, or via a `std.zlib` /
`std.cryptography` / `std.lzf` / `std.fs` decode function — is now classified
heap-returning at that string position, and the destructured value is freed
when the destructured local is released. The previous compiler classified
that value non-heap and never freed it — a silent per-call leak.

If your project destructures such a result (`data, n, err = zlib.deflate(...)`,
`out, err = cryptography.base64_decode(...)`, …) and also stores the string
element into a longer-lived structure (a struct field, list, or map) while
keeping the destructured local — which is later reassigned or goes out of
scope — the previous compiler tolerated this as a leak that kept the aliased
buffer alive forever; this release frees the buffer when the local is
released, so the stored alias becomes a dangling pointer. This shape is a
use-after-free.

**Recommended pre-upgrade play:**

1. Grep for destructures of `zlib` / `cryptography` / `lzf` / `fs` decode
   results, and any user function that returns a `string_new_with_length`
   value in a tuple position.
2. At each, check whether the destructured string element is stored into a
   structure that outlives the destructured local. If so, that structure
   must own an independent copy, not the alias.
3. Rebuild under the new compiler and exercise the affected paths under
   AddressSanitizer or valgrind — a freed-then-read alias surfaces
   immediately as a use-after-free rather than as the old slow leak.

## [0.166.0]

### Fixed

- **`try { } catch { }` codegen: non-local exits from the try body no longer leak the panic frame** (`compiler/codegen/codegen.h`, `compiler/codegen/codegen.c`, `compiler/codegen/codegen_stmt.c`, `tests/integration/try_catch_frame_pop/`). Closes #501.  The pre-fix codegen emitted `aether_try_pop()` unconditionally at the natural end of the try body block; a `return` (or `break` / `continue` leaving the loop the try is in) inside the body skipped it and leaked one frame per call.  After AETHER_PANIC_MAX_DEPTH (32) calls the runtime aborted with `aether: try/catch nesting exceeded 32 — aborting`.  Surfaced during the mquickjs port's `JS_Parse2` migration from a C-side `setjmp` shell to an Aether-side `try`/`catch` (the natural `try { return run() } catch r { return on_error() }` spelling aborted after 32 successful parses); the mquickjs port worked around it with a stash-in-local pattern documented in `mquickjs/ae/parse2.ae`.  The fix tracks a per-function `try_frame_depth` counter and a `loop_try_base[]` snapshot stack: every `AST_RETURN_STATEMENT` emission site emits `aether_try_pop();` calls draining `try_frame_depth` live frames before the C `return`, and every `AST_BREAK_STATEMENT` / `AST_CONTINUE_STATEMENT` drains only the frames pushed *inside* the current loop body (`try_frame_depth - loop_try_base[depth-1]`).  The catch-handler path is unaffected — the runtime pop runs before the handler in the existing codegen, and the counter is already back to caller's value while the handler is generated, so a `return` inside catch correctly drains zero frames.  Three-case integration test (`tests/integration/try_catch_frame_pop/`) covers return-inside-try (100 iterations), break-inside-try-inside-loop, and the false-positive guard (break-from-a-loop-inside-a-try, where the outer try frame must survive the inner break).  This was the first integration coverage of `try/catch` semantics; the only existing panic test (`panic_stack_trace`) exercises only the uncaught-panic process-abort path.
- **`try { } catch { }` codegen: locals modified inside the try body now lower as `volatile`** (`compiler/codegen/codegen.h`, `compiler/codegen/codegen.c`, `compiler/codegen/codegen_func.c`, `compiler/codegen/codegen_stmt.c`, `tests/integration/try_catch_frame_pop/volatile_across_setjmp.ae`). Issue #501 follow-up.  C99 7.13.2.1 says that a local declared in an enclosing scope of a `setjmp` and modified between `setjmp` and `longjmp` has indeterminate value after `longjmp` unless declared `volatile`.  The Aether try/catch codegen emits `AETHER_SIGSETJMP` + `aether_panic` (which siglongjmps) and was emitting plain `T name = init` for at-risk locals.  Under `-O2` the C optimizer kept such locals in a register that the siglongjmp clobbered; the catch handler then read the stale pre-try value.  Concrete repro:  `x = 0; try { x = 42; panic("p") } catch r { println("x=${x}") }`  prints `x=42` under `-O0` and `x=0` under `-O2`.  Same shape on struct locals (`b = Box{val:0}; try { b.val = 42; panic("p") } catch r { println("b.val=${b.val}") }`).  Fix: a per-function pre-pass (`mark_try_clobbered_vars`) walks the body, descends every `AST_TRY_STATEMENT`'s body, and records the bare-identifier root of every LHS it sees on AST_VARIABLE_DECLARATION / AST_ASSIGNMENT / AST_BINARY_EXPRESSION (op = `=` / `+=` / ...) / AST_COMPOUND_ASSIGNMENT nodes.  At each AST_VARIABLE_DECLARATION emission site, `try_volatile_qual_for(gen, name)` returns `"volatile "` when both `try_frame_depth == 0` (we're emitting at outer scope) and the name is in the per-function clobbered set; the prefix is concatenated before the emitted C type.  Conservative — a struct local where a heap-allocated sub-object is modified also gets `volatile`, which is harmless (the prefix only affects the storage of the local itself, not whatever it points at).  Inside-try-body declarations are unaffected because `try_frame_depth > 0` at those emission sites — those locals live and die inside the try body's C scope and never cross the siglongjmp boundary.  Regression test in `volatile_across_setjmp.ae` builds with `-O2` (the `ae build` default) and asserts both the scalar-clobbered path and a sanity-untouched local don't regress.

## [0.164.0]

### Added

- **`std.strbuilder` — `aether_strbuilder_vappend_format`, a C-only `va_list`-accepting companion to `append_format`** (`std/strbuilder/aether_strbuilder.{c,h}`, `tests/runtime/test_runtime_strbuilder.c`). v1/v2 of `std.strbuilder` cover Aether-side call sites with literal arguments; the inverse direction — a variadic *defining* function written in C that wants to delegate its formatting to strbuilder and route the result through Aether — had no bridge, forcing handwritten `va_start`/`vsnprintf`/`va_end` C to stay in the in-tree mquickjs port. Aether deliberately has no defining-varargs (`foo(fmt: string, ...) { ... }` is intentionally not a language feature — platform-specific `va_list`, ABI-fragile); `vappend_format` is the C→Aether bridge that lets that exclusion not block the port. A handwritten variadic C shim that already holds a `va_list` can now do `aether_strbuilder_vappend_format(b, fmt, ap)` and move all the surrounding logic (message building, error routing, throw semantics) to Aether. This is a pure C-FFI primitive — there is **no Aether-side wrapper**, because a `va_list` cannot cross the Aether boundary, so the `module.ae` `exports` list does not grow. `aether_strbuilder_append_format` is refactored to a thin wrapper that delegates to it, with the byte-output-identical guarantee covered by a regression test. The header documents the `va_list` consumption contract: the call consumes the caller's `ap` (matching the `vsnprintf` contract), the caller must `va_end` it, and a caller needing to format the same arguments twice (e.g. a retry with a fallback format) must take its own `va_copy` before the call. C-side regression harness covers six cases — byte-identity with `append_format`, the >256-byte slow path (both directly and via `va_copy` retry), the caller-owns-`va_end` contract, the `va_copy` retry pattern, and NULL/empty inputs. Target consumer: any downstream port that wraps a C library exposing `__attribute__((format(printf, ...)))` entry points (libcurl write callbacks, sqlite error messages, OpenSSL `BIO_printf`-style sinks) and wants the variadic C shim to delegate its formatting to Aether-side strbuilder logic rather than carry a parallel `vsnprintf` step. (The original ask cited the in-tree mquickjs port as the named downstream; on closer audit the mquickjs printf-shaped functions all delegate to `js_vprintf` — a hand-rolled Aether parser with a JSValue-aware `%o` extension — and so don't fit `vappend_format`'s plain-printf shape. The C→Aether bridge is still the right primitive; the mquickjs name was overstated and is corrected here.)

## [0.163.0]

### Fixed

- **`ae help` could not see project libraries — `--lib` rejected, `*.help.md` stdlib-only** (`tools/ae_help.c`, `tests/integration/ae_help/`). `ae help` is pitched (`docs/cic-help.md`) as the on-ramp for closure-DSL / config-as-code authors, but a config script's body is almost entirely library calls — and `ae help` rejected `--lib` outright (`unknown option '--lib'`), so every project-library call was reported as an undefined function and the output was pure noise for any non-trivial project. Filed by the aeb project (`aeb-ae-help-and-toolchain-feedback.md`). Two coupled gaps closed:
    - **`--lib` is now accepted** — repeatable, and PATH-style `--lib a:b` split into discrete entries, matching `ae build` / `aetherc`. `$AETHER_LIB_DIR` is honoured the same way. The resolved directories are forwarded to the wrapped `aetherc` compile (so library imports resolve) *and* folded into the Levenshtein / missing-import export catalog (so suggestions draw on library exports, not just stdlib).
    - **`*.help.md` hint files are read from `--lib` directories**, not only stdlib. `find_help_md_path` previously had an explicit `Stdlib only` cutoff; it now probes `<libdir>/<import-as-dir>/<basename>.help.md` for every resolved `--lib` entry after the stdlib miss. This is what lets a *library author* — not just the Aether core team — ship authoring-mistake hints alongside their module. Four new integration cases cover `--lib` resolution + lib-`*.help.md` surfacing, `--lib` acceptance, `$AETHER_LIB_DIR`, and PATH-style specs.

- **`ae help` no longer shows a library `*.help.md` hint twice when its module is imported more than once** (`tools/ae_help.c`, `tests/integration/ae_help/`). The hint loader runs once per `import` statement, keyed off the module name — so a module named in two `import` lines surfaced each of its hints twice. This is routine for closure-DSL files: the bare-setter rule means a script that uses a builder's setters needs *both* `import bash` (the builder verb) and a selective `import bash (script)` (the bare setter names), and every `bash` hint then appeared duplicated. Reported by the aeb project (`aeb-ae-help-and-toolchain-feedback.md` R2). The `*.help.md` loop now dedups by module name before loading, so each hint fires once regardless of how many `import` lines reference the module.

- **Bare `_` is now a per-use discard, not one shared type-bound variable** (`compiler/analysis/typechecker.c`, `compiler/codegen/codegen_stmt.c`, `tests/regression/test_underscore_discard.ae`). `_` behaved as a single ordinary variable whose C type was fixed by its first use, so a function that discarded a `string` at one site and an `int` at another emitted two conflicting `_` declarations and failed the C compile with `-Wint-conversion` (`out, _ = os.exec(...)` then `_ = os.system(...)`). Filed by the aeb project (`aeb-ae-help-and-toolchain-feedback.md` #4). `_` is no longer registered as a symbol in the `AST_VARIABLE_DECLARATION` typecheck (mirroring the existing skip in the tuple-destructure path), is skipped by `collect_heap_string_var_names` so a string-typed `_` destructure slot is never hoisted as a reusable `const char* _`, and a plain `_ = <expr>` now lowers to a discard — `(void)(<expr>)`, or `aether_heap_str_free(...)` when the RHS is a freshly-allocated heap string so the throwaway doesn't leak. Each `_` occurrence is now independent, matching Rust / Go / Python.

### Changed

- **`ae build` warns on a toolchain version mismatch instead of failing cryptically at link** (`Makefile`, `tools/ae.c`). A split install — a compiler of one version resolving a `libaether.a` of another, the classic case being a stale `current` symlink shadowing an older version dir — surfaced only as `undefined reference to aether_*` at the `ld` step, pointing nowhere near the cause; a maintainer (or LLM) burns real time before thinking to `nm libaether.a`. Filed by the aeb project (`aeb-ae-help-and-toolchain-feedback.md` #3) after adopting `std.strbuilder` against a 0.158 archive under a 0.161 compiler. `make install` now stamps the installed version into `lib/aether/VERSION` next to `libaether.a`; `ae build` reads that stamp and, when it disagrees with the compiler's own version, prints an explicit `toolchain version mismatch` warning naming both versions, the archive path, and the fix (`make install`) — before the build, so the diagnosis precedes any link error. Skipped in dev mode (compiler and archive are always built together there) and when the stamp is absent (an install predating the marker — absent means "cannot check", never breaks the build). Complements the stale-`current` install-time warning shipped in 0.162.0.

## [0.162.0]

### Changed

- **`make install` warns when a stale `$PREFIX/current` symlink would shadow the install** (`Makefile`). `make install` writes the flat layout (`$PREFIX/{bin,lib,include,share}/aether`), but `ae` resolves its toolchain through `$PREFIX/current/` whenever that symlink points at a tree containing `lib/aether` or `share/aether` — the `ae version use` version-manager hook (`tools/ae.c`). A `current` left over from an older install scheme or a past `ae version use` therefore *silently* shadows a fresh `make install`: the maintainer rebuilds, installs, and `ae` keeps linking the stale toolchain with no diagnostic — `ae` only warns on a *half*-populated `current`, never on a complete-but-stale one. The `install` recipe now ends with a check: if `$PREFIX/current` resolves to a real directory other than `$PREFIX` itself, it prints a loud warning naming the shadowing target and the two fixes (`rm` the symlink, or `ae version use`). Diagnostic only — no change to `ae`'s resolution order, so `ae version use` workflows are unaffected. Surfaced while reinstalling the toolchain for the strbuilder-v2 consumer (aeb): a stale `current -> v0.158.0` made `import std.strbuilder` fail at link despite a correct fresh 0.161.0 build.

## [0.161.0]

### Added

- **`std.strbuilder` v2 — `append_long`, `append_hex`, `append_codepoint`, `append_format`, `finish_with_length`, `reserve`, `truncate`, `clear`, `capacity`** (`std/strbuilder/aether_strbuilder.{c,h}`, `std/strbuilder/module.ae`, `tests/regression/test_std_strbuilder_v2.ae`, `tests/integration/std_strbuilder_csv/`). Closes #489. v1 (0.159.0) covered the loop-accumulator idiom `out = string.concat(out, chunk)` → `strbuilder.append`; working the in-tree mquickjs port and the avn bench through it surfaced a consistent shortlist of gaps that forced callers back into parallel snprintf shims and intermediate-allocation round-trips. v2 is purely additive — every v1 entry point and its behaviour is unchanged.
    - **`append_long(b, v)` / `append_hex(b, v, width)`** — 64-bit decimal and lowercase-hex append, the `long`/hex siblings of v1's `append_int`. Without them a 64-bit ID, timestamp, or hex-dump offset round-trips through `string.from_long` (a fresh AetherString the builder then memcpy's and discards). `append_hex`'s `width` is a *minimum* nibble count: 0 = natural width, >0 = zero-pad; a value wider than `width` grows the output rather than truncating. Negative values hex as their unsigned bit pattern.
    - **`append_codepoint(b, cp)`** — UTF-8 encode a Unicode scalar (1–4 bytes) directly into the builder, for parsers emitting escape sequences, source rewriters, and i18n template engines. Returns 0 (appending nothing) for the values UTF-8 cannot legally encode: negatives, the UTF-16 surrogate range 0xD800–0xDFFF, and anything past 0x10FFFF.
    - **`append_format(b, fmt, ...)`** — printf-style formatted append. The C body formats with `vsnprintf` into a 256-byte stack scratch in the common case, falling back to a single reserve-and-format-into-the-tail for longer output. Highest-leverage v2 addition — it removes the parallel C-snprintf step every dump / log / template caller kept. Implemented as a variadic `@extern` binding (see the cross-module `@extern` entry below) because an ordinary Aether wrapper cannot forward a `...` tail. `%s` arguments must be plain C strings / Aether string literals; `%d` takes `int`, `%lld` takes `long`.
    - **`finish_with_length(b) -> (ptr, int)`** — binary-safe finalise. Returns the raw data buffer and its exact byte length with NO NUL terminator appended, so embedded NULs survive as content — the shape binary-protocol assembly (CBOR, msgpack, frames, packed records) needs and which v1's NUL-terminated `string`-returning `finish` cannot give a strlen-based consumer. The caller owns the buffer and frees it with a plain libc `free()`; position 0 is `ptr`, so it is deliberately not codegen-heap-tracked. v1's `finish` is kept unchanged for the ASCII/UTF-8-text case.
    - **`reserve(b, additional)` / `capacity(b)`** — pre-grow the buffer so the next `additional` bytes of appends don't reallocate (a pure performance hint — the builder still grows on demand), and observe the current capacity (the companion to `length`, mainly for confirming a `reserve` took / watching the 2× doubling).
    - **`truncate(b, new_length)` / `clear(b)`** — pop accumulated content back to `new_length` bytes (only shrinks — a length past the current is rejected rather than exposing uninitialised capacity), or reset to empty. The buffer is retained, so a truncate-then-append / clear-then-append reuse loop keeps the doubling amortisation across iterations instead of paying a fresh `new` + `finish` every time. Critical for per-row builders and row-cancellation in emitters.

    Nine-case regression test plus a CSV-emitter integration test (`std_strbuilder_csv`) exercising the realistic combo — `append` / `append_byte` / `append_format` / `truncate` for row-cancellation / `clear` for buffer reuse, verified byte-exact through the `finish_with_length` `(ptr, len)` pair, with a 2000-cycle RSS bound.

- **`@extern("c_symbol")` declarations cross the `import` boundary, and may be variadic** (`compiler/aether_module.c`, `compiler/parser/parser.c`, `compiler/codegen/codegen_func.c`). Two gaps in the `@extern` feature (#234) surfaced while giving `std.strbuilder`'s `append_format` a clean module-qualified name:
    - **Cross-module visibility.** `@extern("c_symbol") name(...)` declarations were only resolvable in their *defining* file — `module_merge_into_program` clones Aether-native wrappers, constants, and structs into the consumer program but never extern nodes, so `import m` then `m.name(...)` raised `E0301`. The merge now also clones `@extern`-annotated extern nodes (those carrying a `c_symbol:` annotation — bare `extern` C bindings stay private as before), namespace-prefixing the Aether-side name while the `c_symbol:` annotation keeps codegen forwarding to the chosen C symbol. This matches #234's stated intent that the `@extern` name "lives in the module's public surface".
    - **Variadic `@extern`.** The bare `extern foo(fmt: string, ...)` form accepted a trailing `...` (0.160.0); the `@extern("sym") foo(fmt: string, ...)` form's parameter loop did not. The parser now accepts `...` there too, recording it as a `;varargs` suffix on the `c_symbol:` annotation (the `;` delimiter never occurs inside a C identifier, so codegen splits the symbol from the flag cleanly via the new `extern_c_symbol` / `extern_is_varargs` helpers).

### Fixed

- **Heap-ownership tracker leaked AetherString-shaped function return values** (`compiler/codegen/codegen.c`, `compiler/codegen/codegen_stmt.c`, `compiler/codegen/codegen_func.c`, `compiler/codegen/codegen_expr.c`, `std/bytes/module.ae`, `tests/regression/test_bytes_finish_heap_return.ae`, `tests/integration/bytes_finish_return_no_leak/`). Filed by the avn project as an unbounded ~430 KB-per-commit RSS ramp in a long-lived commit server (`heap-ownership-return-propagation.md`) — a 5000-commit bench ended at 2.57 GB RSS where classic `svnserve` plateaus at 31 MB. Any function whose return expression was `bytes.finish(...)` — or any wrapper chain bottoming out there — leaked its result at every call site. Two distinct gaps, both closed:
    1. **`bytes.finish` was not recognised as a heap source.** `aether_bytes_finish` was declared `-> ptr` with no `@heap` annotation, so `is_heap_string_expr` classified it non-heap; the interprocedural return-ownership summary (`function_def_returns_heap_string`) then propagated that false "non-heap" leaf up through every wrapper, and call sites emitted bare assignments with `_heap_x = 0` and no free. Fixed by annotating the extern `-> string @heap`, mirroring `aether_strbuilder_finish` — the existing cross-function classifier does the rest, no new analysis.
    2. **The heap-string tracker freed every tracked value with a plain `free()`.** Correct for the existing heap sources (`string.concat`, interpolation, `@heap` externs — all single-allocation libc-`malloc`'d `char*`), but `bytes.finish` returns a refcounted `AetherString*`: a struct *plus* a separately allocated data buffer. `free()` on the struct alone leaked the data. A new emitted `aether_heap_str_free` helper probes the AetherString magic header (byte-by-byte, short-circuit, ASan-clean — the same shape as `aether_uniform_heap_str`) and routes to `string_release` for AetherStrings, plain `free()` otherwise; all ten heap-tracker free sites across the four codegen files now go through it. The repro went from 39 bytes lost in 6 blocks to a valgrind-clean run.

### Upgrade notes

This release tightens heap-ownership tracking: a function whose return expression is `bytes.finish(...)` — directly, or through a wrapper chain — is now classified heap-returning, and the value it yields is freed at every call site, at scope exit or on the next reassignment. The previous compiler classified these values non-heap and never freed them, so every such call site leaked.

This is a leak fix, not a behaviour change for correct code. The escape analysis behind the function-exit free already suppresses the free for any value that escapes through a recognised edge — struct-field write, `list.add` / `map.put`, actor-message field, closure capture, or `return` — so code that hands a `bytes.finish` result to a long-lived holder through one of those edges is unaffected.

The narrow hazard: if your project routes a `bytes.finish`-derived value into a long-lived holder through a path the escape analysis does **not** recognise — hidden behind an opaque `ptr` cast, or written via raw `std.mem` — the previous compiler's leak kept the backing buffer alive; this release reclaims it when the local goes out of scope, and the stored copy dangles. That shape is a use-after-free. (Values with more than one reference are safe: `string_release` only decrements the refcount, and the buffer survives until the last reference drops.)

**Recommended pre-upgrade play:**

1. Rebuild under the new `aetherc` and run your test / integration suite under `-fsanitize=address` (or valgrind).
2. Look for use-after-free reports whose allocation stack passes through `aether_bytes_finish` / `string_new_with_length` — those are the newly-reclaimed `bytes.finish` buffers.
3. At each such site, give the long-lived holder its own reference (retain it, or copy the bytes) instead of borrowing the caller's now-reclaimed pointer.

## [0.160.0]

### Added

- **`std.mem` — caller-allocated raw-pointer access module** (`std/mem/aether_mem.c`, `std/mem/module.ae`, `tests/integration/std_mem_byte_access/`). Companion to `std.bytes` (Aether-managed mutable buffers). `std.mem` is for the case where the buffer is NOT Aether's — a raw `char*` from a C call, a malloc'd block, a buffer the platform handed back. Aether parses `p[i]` syntactically but lowers it to `*(void*)p` which the C compiler rejects with `error: void value not ignored as it ought to be`; the `std.mem` extern surface routes the same intent through same-effect extern calls. Module name is `std.mem` (not `std.ptr`) because `ptr` is a reserved keyword for the type itself. First consumer: the in-tree mquickjs port, which uses these accessors in every UTF-8 decoder, bytecode interpreter, packed-format parser, and JSValue tagged-pointer manipulation. v1 surface, grouped by purpose:
    - **Byte-level access:** `get_byte(p, i)` / `set_byte(p, i, v)` — single-byte read/write with NULL-pointer defence and low-8-bit masking on the setter. The foundational pair.
    - **Pointer slot access:** `get_ptr(p, off)` / `set_ptr(p, off, q)` — read or write a full `void*` slot at byte offset `off`. Lowers via `__builtin_memcpy` for unaligned safety.
    - **Width-sized integer slot access:** `get_int(p, off)` / `set_int(p, off, v)` for int32 slots, and `get_long(p, off)` / `set_long(p, off, v)` for size_t-/int64-sized slots (FFI out-params, struct field reads). The `_int` pair is essential because Aether's `int` is 32-bit but raw FFI structs often pack int32 fields at non-zero offsets; the `_long` pair handles platform-correct size_t access without the caller threading width-specific helpers.
    - **Sized typed-array accessors:** `get_int8/uint8/int16/uint16/uint32/float32/float64` plus matching setters — 14 accessors total. Lowered via `__builtin_memcpy` so unaligned reads from packed wire-format buffers (mquickjs bytecode, network frames) don't trap.
    - **Floating-point bit reinterpret:** `bits_of_float(d) -> long` and `float_from_bits(b) -> float` — reinterpret a double's bit pattern as int64 and vice versa, for hashing / serialisation / NaN-boxing schemes. Implemented via `union { double; uint64_t; }` so the round-trip is exact on every platform with IEEE-754. Named `bits_of_float` (not `float_to_bits`) to match the natural "X of Y" reading order used elsewhere in `std.mem`.
    - **Bit-counting intrinsics:** `clz32(x) -> int` and `clz64(x) -> int` — count leading zeros, with the conventional "input == 0 returns the width" contract (32 / 64). Wraps the appropriate `__builtin_clz` family.
    - **Wide integer division:** `udiv64_32(num: long, den: int) -> long` — unsigned 64-by-32-bit division. C compilers know this is a single `div` instruction on x86-64 and a single `udiv` on AArch64, but Aether's binary-op codegen always emits `int / int` or `long / long`; the helper exposes the mixed-width form for cases where it materially matters (JS numeric-string parsing, where the inner loop divides a 64-bit accumulator by a 32-bit base).
    - **Bare-fnptr invocation:** `call_fn3_int(fp, a, b, c) -> int` and `call_fn3_void(fp, a, b, c)` — call a C function-pointer of signature `int(void*, void*, void*)` (or void-returning equivalent) given a raw `ptr` to it. Bespoke per-signature shim layer; will be replaced by a general typed function-pointer dispatch when that Aether feature lands. Bridges the gap for now so callers like mquickjs's `mqjs_jscfdef_invoke` can stay tablful-dispatch instead of growing a switch per call site.
- **Variadic externs — `extern foo(fmt: string, ...) -> int`** (`compiler/parser/tokens.h`, `compiler/parser/lexer.c`, `compiler/parser/parser.c`, `compiler/codegen/codegen_func.c`, `tests/integration/aether_extern_varargs/`). Adds the trailing `...` marker on extern declarations, lowered to C variadic prototypes. Call sites pass any number of trailing args literally — `printf("x=%d y=%s", 42, "hi")` emits the same call straight through. Two spellings: `extern foo(fmt: string, ...) -> int` (named param + trailing varargs) emits `int foo(const char*, ...);`, and bare `extern bare_va(...) -> int` emits `int bare_va(...);`. v1 deliberately does NOT support Aether-side function-DEFINING varargs (no `va_list` consumption Aether-side) — that requires a platform-specific `va_list` type and is out of scope. The motivating use case is forwarding to printf-shaped C APIs (JS_ThrowSyntaxError, fprintf, snprintf) from Aether without a hand-written C shim per call site. Lexer: new `TOKEN_DOTDOTDOT` recognised as `...` (extends the existing `..` range-operator path with a third-`.` peek). Parser: `parse_extern_declaration` accepts trailing `...` either after a comma or as the sole "param" and tags the AST node with `annotation = "varargs"`. Codegen: appends `, ...` or bare `...` to the C prototype when the annotation is set; coordinates with the new `-Wstrict-prototypes`-clean emit (#0.153.0) so bare `(...)` does not collide with the no-arg `(void)` emission. Type-checker: no arity enforcement at extern call sites already, so variadic calls just work — no analysis change needed. Four-case integration test covers named+`...`, bare `(...)`, end-to-end printf execution, and a non-varargs extern sanity check.
- **Typed C function-pointer locals — `fn(T1, T2, ...) -> R` types and `expr as fn(...) -> R` cast** (`compiler/ast.h`, `compiler/ast.c`, `compiler/parser/parser.c`, `compiler/analysis/typechecker.c`, `compiler/analysis/type_inference.c`, `compiler/codegen/codegen.h`, `compiler/codegen/codegen.c`, `compiler/codegen/codegen_internal.h`, `compiler/codegen/codegen_expr.c`, `compiler/codegen/codegen_stmt.c`, `tests/integration/aether_fnptr_call/`). Lets Aether declare local variables (or cast `ptr` values to) typed C function pointers and invoke them directly. Source spelling: `fp = get_callback() as fn(int, int) -> int` followed by `result = fp(3, 4)`. Storage is `void*` in the emitted C; the call site injects a typed C function-pointer cast — `((int(*)(int, int))(fp))(3, 4)` — so the C compiler sees the correct signature without requiring per-signature typedef synthesis. Uses an `is_fnptr` discriminator on `TYPE_FUNCTION` so the existing closure (`_AeClosure`-shaped) path is unaffected; bare `fn` (no signature) still produces an Aether closure value. Parser: `parse_type()` accepts `fn(T1, T2, ...) -> R` (and `fn(T1, ...)` with implicit `void` return); the postfix `as` operator accepts the same fn-type as a destination alongside the existing `as *StructName` struct-overlay cast. Typechecker: validates that the operand of `as fn(...)` is a `ptr`-like value (`ptr` / `int` / `int64` / `uint64`); a fn-typed local call (`fp(a, b)`) is type-checked against the stored signature; the existing closure-call rewrite to `call(fp, args)` is gated on `is_fnptr=0` so it doesn't fire for void* fn-pointers, which have no `.fn` / `.env` pair to dispatch through. Codegen: a registry of fn-pointer locals (`fnptr_locals`, populated by variable-declaration codegen) is consulted at call sites to emit the matching `((R (*)(T1, T2))(name))(args)` cast inline; no per-signature C shims. Replaces the bespoke `mem.call_fn3_int` / `mem.call_fn3_void` per-signature shim layer for cases where Aether holds a C function pointer (vtable lookup result, signal handler table, qsort callback handoff, libcurl write callback, sqlite update hook, etc.) and needs to invoke it with type safety. v1 surface is local-variable / cast-result calls only — fn-typed function parameters and struct fields, while parsable, are not yet exercised by the integration test and are out of scope for v1; future work can expand the registration sites in `register_fnptr_local` to cover them. Five-case integration test: typed cast + invoke at call site, unary signature, void-returning signature (`fn(int)` with no `-> R`), end-to-end execution with C-side glue functions returning real values, and a backward-compat check that bare `fn` (no signature) still produces an `_AeClosure`-shaped local.
- **`extern struct Name { ... }` — declared C struct layouts with optional bitfield support** (`compiler/ast.h`, `compiler/parser/parser.c`, `compiler/codegen/codegen_func.c`, `tests/integration/aether_extern_struct/`). Lets Aether describe a C struct's layout once, then read and write its fields via the existing `*StructName` pointer-overlay path (`view.field` → `view->field` in emitted C) — no hand-written per-field accessor function per field. Source spelling: `extern struct JSObject { class_id: byte; extra_size: int }` followed by `obj = raw_ptr as *JSObject; obj.class_id = 7`. C bitfields use the trailing `: NN` width annotation per field — `is_unique: int : 1`, `len: int : 27` — emitted as `int is_unique : 1;` in the C struct so the C compiler handles bit-extract on access. Signed/unsigned bitfields are distinguished by the field type: `int : 1` reads as `-1`/`0` (1-bit two's-complement); `byte : 1` reads as `1`/`0` (unsigned). Layout is owned by Aether — the `extern struct` decl emits the same `typedef struct Name { ... } Name;` into the .gen.c that a regular Aether-side `struct` would, so both Aether and the surrounding C glue (compiled as a separate TU that carries its OWN matching struct definition, byte-identical by user-side discipline) see the same layout via the pointer round-trip. Motivating use case: the in-tree mquickjs port carries ~1700 hand-rolled C accessor functions (`mqjs_jsobj_class_id`, `mqjs_jsstring_is_ascii`, etc.) that exist purely to bridge a single field read or write; declaring the struct layouts once collapses every accessor call site to a native Aether `view.field` access against the void*-overlay-as-typed-pointer.  No new tokens — reuses the existing `extern` and `struct` keywords plus the existing colon and number-literal parsing for the bit-width annotation. AST: `AST_STRUCT_DEFINITION.annotation = "extern"` discriminator (same mechanism as varargs); `AST_STRUCT_FIELD.bit_width` slot (0 = plain field, >0 = C bitfield of that width, 1..64). Codegen reuses the existing `generate_struct_definition` path, with a small branch on `bit_width` to emit `type name : NN;` instead of `type name;`. Member access is unchanged — the existing `*StructName` overlay handles `view->field` for both plain fields and bitfields uniformly. Five-case integration test covers plain fields, signed-int bitfields (asserts -1/0 semantics for 1-bit signed), unsigned `byte` bitfields (asserts 1/0 semantics), end-to-end C-glue interop with separate-TU compilation, and a regression check that non-extern Aether-side `struct` definitions are unaffected.
- **`extern struct` trailing flexible-array field + `array_field + int → ptr` arithmetic** (`compiler/codegen/codegen_func.c`, `compiler/analysis/typechecker.c`, `tests/integration/aether_extern_struct_flex_array/`). Lets Aether describe C99 flexible-array tails on extern structs and read/write them with native `field[i]` syntax. Source spelling: `extern struct JSString { len: uint64 : 57; buf: byte[] }` followed by `(s as *JSString).buf[i] = byte_value` (write) or `b = (s as *JSString).buf[i]` (read). Codegen branch on `extern` annotation + last-field check: when an `extern struct` ends with a `T[]`-typed field with `array_size == 0`, emit `T name[];` (zero-byte field; trailing storage is the caller's responsibility) instead of the `T* name;` lowering that's correct for Aether-managed dynamic arrays in a regular `struct`. Plus a typechecker extension: `array_field + int_offset → ptr` and the symmetric `int + array_field → ptr` form. Used to materialise a pointer-to-element for handoff to memcpy-style C externs — `(s as *JSString).buf + idx` produces a `ptr` that downstream `mqjs_memcpy_w` etc. accept directly. Matches the C semantics where the array decays to a pointer at expression level. Four-case regression test covers header+flex-buf round-trip, variable indexing loop, byte-level layout verification, and confirmation that the flex tail contributes zero bytes to `sizeof(struct)`. First in-tree consumer: the mquickjs port's JSString `buf[]` field, eliminating ~50 hand-rolled `mqjs_jsstring_buf_*` C accessor call sites in favour of native indexing.
- **Statement-leading `(` no longer eaten as a function-call applied to the previous statement** (`compiler/parser/parser.c`, `tests/integration/aether_inline_cast_lhs/`). Aether doesn't require semicolons; this same "newline acts as a soft statement separator unless the next token clearly continues" heuristic is already used for trailing-block braces (`{` only attaches when on the same line as the call's `)`). Pre-fix, the postfix-call branch in `parse_postfix_expression` greedily treated every `(` after any expression as a function call, so:

      println("done")
      (p as *T).field = v

  parsed as `println("done")(p as *T).field = v` — one expression — producing a bogus `AST_FUNCTION_CALL` with the previous statement's expression as the (typeless) callee, and a typecheck error `Undefined function '?'`. The fix gates the postfix-call branch on the `(` being on the same source line as the previous token; a `(` on a fresh line breaks out of the postfix loop, letting `parse_statement` see it as the start of a fresh statement. Discovered when the in-tree mquickjs port's JSObject union-variant sweep needed `((p + offset) as *VariantStruct).field = value` at statement level for ~80 setter sites — pre-fix required hoisting each setter to a 2-line temp pattern. Four-case regression test covers bare `(p as *T).field = v`, nested-paren `((p + N) as *T).field = v`, RHS-in-expression read sites (already worked pre-fix, confirms no regression), and the post-call shape that drove the fix.
- **Top-level `const` declarations hoist same way `extern struct` typedefs do** (`compiler/codegen/codegen.c`). Mirror fix to the struct hoist landed in the previous commit. When module-merge clones an imported `const NAME = VALUE` into the consumer's program AST, the resulting AST_CONST_DECLARATION node lands AFTER consumer function bodies that may reference it — producing `error: 'mod_NAME' undeclared` at the C compile step. New pre-pass in `generate_program` emits all `#define mod_NAME (value)` lines right after the struct-body hoist, before function forward decls. The original in-place emit at the per-child loop is now a no-op for const declarations (already emitted in the hoist). Same shape as the struct hoist; both fixes together close the "imported top-level decl appears at end of merged AST" forward-reference gap. Discovered when the in-tree mquickjs port introduced a shared `const JSOBJ_U_OFFSET = 24` in `ae/mqtypes/module.ae` and the union-variant call sites failed to compile because the `#define` came too late.
- **`extern struct` declarations propagate across `import` boundaries + codegen hoist for forward-ref safety** (`compiler/ast.c`, `compiler/codegen/codegen.c`, `tests/integration/aether_extern_struct_import/`). A consumer can now `import sometypes` to bring an `extern struct` declaration into scope and cast `ptr` values to the imported `*StructName` without redeclaring the layout locally.  Two underlying fixes made this work end-to-end. (1) `clone_ast_node` was not copying the `bit_width` field added in the prior `extern struct` commit, so imported bitfield-bearing structs emitted with garbage widths (`unsigned char arg_count : 32726` — uninitialized memory).  Both `create_ast_node` (zero-init) and `clone_ast_node` (copy from source) now handle `bit_width` correctly. (2) Codegen previously emitted struct typedef bodies in source-AST order, which placed imported struct full definitions AFTER the function bodies that used `view->field` member access on them — producing `error: invalid use of incomplete typedef` at the C compile step.  A new pre-pass in `generate_program` hoists ALL `AST_STRUCT_DEFINITION` full-body emissions to the top of the file (right after the forward-typedef pass, before function forward decls), matching how a hand-rolled .c file would order them.  The original in-place emit at the per-child loop is now a no-op for struct definitions (already emitted in the hoist).  Four-case regression test (`tests/integration/aether_extern_struct_import/`) covers a "types" module exporting two extern structs (one with plain fields, one with bitfields), a second consumer module that imports the same layout, and main exercising direct field access, cross-TU read-after-write, cross-TU write-then-read, and bitfield round-trip — all against the same shared declaration. First in-tree application: the mquickjs port collapses three local `extern struct JSCFunctionDef { ... }` redeclarations to a single `ae/mqtypes/module.ae` declaration imported via `import mqtypes` (commit on the mquickjs side).

### Fixed

- **`TYPE_FLOAT` lowers to C `double` consistently across the codegen** (`compiler/codegen/codegen.c`). Before the fix, some paths emitted `float` (32-bit) for Aether's `float` type while struct-field, parameter, and return-type paths emitted `double` (64-bit) — producing silent ABI mismatches between Aether modules and between Aether and C. Aether's `float` has always been documented as IEEE-754 double-precision; this aligns every emission site with that contract. Most user code is unaffected because the inconsistency only bit FFI boundaries that crossed the divergent paths, but it surfaced as a hard-to-diagnose value corruption during the mquickjs port (an Aether-side `float` round-tripping through a C struct returned the truncated single-precision approximation).
- **`ptr +/- int` arithmetic now keeps `ptr` type** (`compiler/analysis/typechecker.c`). Prior to the fix, the typechecker resolved `ptr_val + offset` as `int + int` (silently coercing the pointer to int and truncating the high 32 bits on 64-bit platforms). The natural pattern `arr_base + idx * sizeof_T` returned a half-pointer that subsequent dereferences would page-fault on. Now `ptr + int → ptr` and `ptr - int → ptr` (with the symmetric `int + ptr → ptr` form also accepted). Both summands' types are checked: `ptr + ptr`, `ptr + long`, `ptr * int`, etc. still type-error as before. The mquickjs port hit this in every byte-level walk over malloc'd buffers; the fix unblocks the natural arithmetic spelling.
- **Struct-literal disambiguator respects `in_condition`** (`compiler/parser/parser.c`, `tests/regression/test_empty_if_body_var_compare.ae`). When the parser sees `if v < cmp { ... }` and the `{` is at end-of-line with an empty body, it was greedily trying to treat the whole `cmp { ... }` as a struct literal — failing to commit to the if-body brace. The fix gates struct-literal lookahead inside condition contexts (`in_condition`-aware) so the brace binds to the if's body. Regression test exercises the exact failing shape: `if cur < total { }` with the empty body on its own line.
- **Hex literal range probe uses the natural spelling** (`tests/integration/std_mem_byte_access/probe.ae`). Lifts a workaround that built the IEEE-754 negative-zero pattern (`0x8000000000000000`) via runtime arithmetic (`half + half`) because the lexer formerly clamped wide literals via `strtol`. Upstream's PR #457 wide-literal pipeline now flows the literal text through to codegen verbatim, so the negative-zero probe can write `0x8000000000000000` directly. Pure test cleanup; the underlying lexer fix is upstream's.
- **Codegen-side cleanup: variadic-extern parser flag composes with strict-prototype emit.** When `--Wstrict-prototypes`-clean emit (added in 0.153.0) marks an extern with no named params, it emits `(void)`. For variadic externs with no named params (bare `extern foo(...) -> int`), the codegen suppresses the `void` and emits `(...)` instead — the two emission rules were not co-designed because the strict-prototype work landed in main while the varargs work was in-flight on a feature branch. No user-visible behaviour change for non-varargs externs.
- **`emit_lib_primitives` integration test catches up to the TYPE_FLOAT contract** (`tests/integration/emit_lib_primitives/config.ae`, `tests/integration/emit_lib_primitives/consume.c`). The test's C consumer dlsym'd `aether_scale` as `float(float, float)` and called it with `3.0f`/`2.5f`. With TYPE_FLOAT lowering now uniformly emitting `double` (see the bullet above), the symbol's actual signature is `double aether_scale(double, double)` — the single-precision call site became an ABI mismatch and the test failed with `scale(3.0, 2.5) = 0.000000`. Updated to `double(double, double)` on both sides plus a comment refresh on the (now stale) `Aether float == C float` claim in `config.ae`. Aether-source surface keyword is still `float`; only the emitted C ABI moved.

## [0.159.0]

### Added

- **`std.strbuilder` — amortised-O(1) string append primitive** (`std/strbuilder/aether_strbuilder.{c,h}`, `std/strbuilder/module.ae`, `tests/regression/test_std_strbuilder.ae`, `tests/integration/std_strbuilder_no_leak/`). The canonical accumulator idiom `out = string.concat(out, chunk)` in a loop is O(N²) per byte — every concat reallocates and memcpy's the full prefix. The heap-string-tracker making each leaked-prefix prompt doesn't address the algorithmic cost. `std.strbuilder` fills that gap with a growing internal buffer (2× doubling on overflow): `b = strbuilder.new(cap_hint); strbuilder.append(b, chunk); out = strbuilder.finish(b)`. The API mirrors Java/Kotlin StringBuilder, Go's strings.Builder, Rust's String::push_str. Six append surface functions — `new`, `append`, `append_n`, `append_byte`, `append_int`, `length`, `finish`, `free` — cover the loop-build-and-emit shapes the avn 5000-commit bench's hot path drives (and the broader category: JSON / log / template / packed-record / path-URL assembly / CSV emit). Module is named `strbuilder` rather than `builder` because the latter is reserved in Aether for configure-then-execute DSL functions (`TOKEN_BUILDER`, `compiler/parser/parser.c:3686`). `finish()` hands the data buffer to the caller as a plain libc-freeable `char*` and frees only the wrapper struct; the heap-string-tracker's reassignment-wrapper-emitted libc-free reclaims the full output uniformly, matching the established `@heap`-extern contract (cf. `tests/integration/extern_single_value_heap/`). Buffer is `aether_caps_malloc`/`_realloc`'d throughout so a host's `aether_set_memory_cap(N)` bounds runaway accumulation. Nine cases covered by the regression test (loop accumulator, byte separators, int append, explicit-length binary, mixed-lifecycle reuse, edge cases on cap_hint) plus a 2000-build/finish RSS-bounded integration probe (12 MB growth pre-fix vs. < 1 MB post-fix as the discriminator). Feature filed by avn project; see `std-builder-request.md` for the motivating analysis.

- **`string.char_at_n` / `string.index_of_from_n` — length-aware accessors that skip the per-call strlen** (`std/string/aether_string.{c,h}`, `std/string/module.ae`, `tests/regression/test_string_char_at_n_and_index_of_from_n.ae`). Both `string.char_at` and `string.index_of_from` call `str_len(str)` on every invocation to bounds-check the index. When the haystack arrives as a `string` parameter at a function boundary, #297's auto-unwrap strips the AetherString header — `str_len` then falls through to `strlen`, and any caller looping over a multi-KB body pays the full O(N) scan per byte. avn's 5000-commit bench profile (filed in `string-length-aware-accessors.md`) showed `__strlen_avx2` accounting for 87%+ of CPU at the slow batches, driven by `dir_iter_next` walking a 250 KB dir-blob body whose length the iterator had already cached at construction. The new `_n` siblings let the caller pass the cached length and skip the strlen. Pure additive surface — existing callers unaffected. `string_char_at_n(str, known_length, index) -> int` and `string_index_of_from_n(str, known_length, substring, start) -> int` mirror the existing `string_substring_n` / `string_length_n` precedent: trust the caller's known_length absolutely, do not consult the AetherString header even if one is present. The needle in `index_of_from_n` is still strlen'd (small / fixed in practice — needles are byte separators or short tokens). Eight cases covered in the regression test (every-byte match, out-of-range clamp on char_at_n, trusted-length-shorter-than-buffer, basic hit, known_length-clips-search, start clamping both directions, oversized needle, parity with bare `index_of_from`).

## [0.158.0]

### Fixed

- **`p = p` self-assignment on a string parameter double-frees the caller's buffer** (`compiler/codegen/codegen_stmt.c`, `tests/regression/test_self_assign_param_doublefree.ae`). Hard correctness regression introduced by the Path B `is_heap_string_expr` extension that recognises bare-identifier-of-tracked-local as heap=1. The reassignment-wrapper's emission path runs `is_heap_string_expr(rhs)` to decide whether to set `_heap_<lhs> = 1` after the assignment. For `p = p`:

  ```c
  { const char* _tmp_old = p; p = p; if (_heap_p) free((void*)_tmp_old); _heap_p = 1; }
  ```

  Step (a) RHS=`p`, AST_IDENTIFIER, in heap-tracked-vars → `is_heap_string_expr` returns 1. Step (b) wrapper sets `_heap_p = 1`. Step (c) function-exit defer-free fires `free((void*)p)` — but `p` is a borrowed parameter; the caller still holds the same pointer and proceeds to read or free it → use-after-free or double-free (`free(): double free detected in tcache 2` under `MALLOC_CHECK_=3` / ASan).

  Trigger pattern (filed by avn during 0.157.0 rebuild, hits 27 call sites in their user code via `client.remote_free` and `repo_storage.rep_free`):

  ```aether
  // Common "no-op free shim" for libraries with refcount-backed
  // string lifetimes — the call is preserved for ABI continuity
  // after the C-side explicit-free API was replaced.
  noop_free(p: string) {
      p = p
      return
  }

  main() {
      s = string.copy("hello")
      noop_free(s)
      println(s)   // pre-fix: garbage or SIGABRT
  }
  ```

  Fix: peephole at the top of the reassignment-wrapper-emission branch — if `stmt->value == stmt->children[0]->value` (same identifier name, i.e. `lhs == rhs`), skip emission entirely. The self-assignment is semantically a no-op; emitting nothing keeps the heap-tracker in its pre-assignment state, which is correct (nothing actually changed). Guard fires before any wrapper-emission branch, so it covers every code path downstream (string-tracked, escaped, non-string, etc.) uniformly. Six lines.

  Considered and rejected: making parameters borrows-by-default (refuse to flip `_heap_<param>` to 1 unless annotated `@retain` / `@own`). Broader architectural change with category-of-bugs benefits but multi-PR scope; the peephole closes the surfaced regression cleanly and the architectural fix can land as a follow-up without conflicting.

  ### Upgrade notes

  No user code change required. Code using the `p = p` "no-op free shim" pattern (or any other `lhs = lhs` self-assignment, however unusual) was double-freeing pre-fix; post-fix the assignment is correctly a no-op. Behaviour change is strictly correctness recovery; no regression on any other shape.

## [0.157.0]

### Fixed

- **Two follow-up heap-string leak classes the post-0.156.0 codegen didn't catch** (`compiler/codegen/codegen_stmt.c`, `compiler/codegen/codegen_expr.c`, `tests/integration/heap_leak_cross_fn_recursion/`, `tests/integration/heap_leak_interp_as_arg/`). Filed by avn during downstream rebuild against 0.156.0; their 5000-commit bench stayed at O(N²) shape despite the heap-ownership-critical-fixes from #471. Two distinct gaps:

  **1. Cross-function + recursion + bare-identifier-return-of-tracked-local.** The classifier walker (`walk_returns_for_heap_check`) checked bare-identifier-return shapes against `is_heap_string_expr` only — which doesn't recognise AST_IDENTIFIER. A function whose body accumulates via heap-string calls and returns the accumulator via `return body` was AND-folded non-heap; the caller's reassignment wrapper set `_heap_<lhs> = 0`; the buffer leaked per call. avn's `rebuild_dir` hot-path shape:

  ```aether
  rebuild_dir(depth: int, fanout: int) -> string {
      body = ""
      while ... { body = reclist_append_unique_(body, ...) }   // heap calls accumulate
      if depth > 0 {
          sub = rebuild_dir(depth - 1, fanout)                  // recursive
          body = reclist_append_unique_(body, sub, ...)
      }
      return body                                               // bare-identifier return
  }
  ```

  The 0.156.0 design relied on `gen->heap_string_vars` to identify heap-tracked locals, but that set is in the CALLER's context when the classifier runs from a cross-function call site (e.g. `main` invoking `is_heap_string_expr` on `rebuild_dir`'s return analysis). The lookup missed the callee's local. Structurally the same bug shape avn reported against the 0.150 piece-2 implementation (perf_regression_quadratic_commit_path_followup.md) — same context-sensitivity, different mechanism.

  Fix: new `body_assigns_var_from_heap` helper walks the callee's function body directly to identify whether `<var_name>` has any heap-classified assignment, without consulting `gen->heap_string_vars`. The walker's bare-identifier-return clause uses this self-sufficient check, so the classification works regardless of which context invoked the classifier. Memoization on the function-def annotation still applies; the walk runs once per function. Combined with the existing `aether_uniform_heap_str` wrap at return statements, the caller now receives a heap-allocated buffer that gets reclaimed via the reassignment-wrapper free.

  **2. AST_STRING_INTERP subexpression in argument position leaks the interp's heap buffer.** Commit 39446f9 (in #471) added arg-temp wrapping for AST_FUNCTION_CALL subexpressions — `outer(call(...))` hoists the inner call into `_ad_N`, runs the outer, and frees `_ad_N` after. The wrapping was scoped to `AST_FUNCTION_CALL` only; AST_STRING_INTERP subexpressions in argument position got no temp, no free. `outer("seed-${i}")` shapes leaked the interp's malloc'd buffer per call — ~34 bytes/iter observed, 100k iter → 3.3 MB.

  Fix: two-spot extension to AST_STRING_INTERP:
  - `codegen_expr.c` substitution-in-generate_expression — `arg_drain_lookup` substitution now matches both AST_FUNCTION_CALL AND AST_STRING_INTERP, so the temp name replaces the inline interp in the outer call's emission.
  - `codegen_expr.c` arg-loop type filter — accepts AST_STRING_INTERP as well as AST_FUNCTION_CALL. The `is_heap_string_expr` and escape-marking gates downstream are unchanged (interp results are already classified heap there).

  Result: same statement-expression + free pattern that 39446f9 applied to nested-call args now covers interp args too. Test reduction: 3.3 MB → 168 KB across 100k iterations (the residual is allocator bookkeeping).

  Two new integration tests (`heap_leak_cross_fn_recursion`, `heap_leak_interp_as_arg`) cover both shapes with the standard RSS-bounded pattern, gated on POSIX `getrusage` with a Windows-MinGW fallback to smoke-only mode (same shape as the existing `heap_tracker_return_escape_no_leak`).

  ### Upgrade notes

  No user code change required. Both fixes are codegen-side; the language contract is unchanged.

  **What flips behaviourally:**

  1. Functions with the cross-fn + recursion + bare-identifier-return shape (avn's `rebuild_dir` and any code matching that pattern) now have their accumulator buffer correctly classified heap; the caller's reassignment wrapper sets `_heap_<lhs> = 1` and the buffer is reclaimed at the next reassignment / function-exit defer-free. Bench-time growth that was O(N²) under 0.156.0 returns to O(N) per call.
  2. `outer(<interp>)`-shape call sites now hoist the interp result into a temp and free after the outer call. Previously-leaked ~34 byte/call buffers are reclaimed. Aether code that captured a reference to the interp result via a `ptr`-typed parameter or other escape edge is unaffected (the escape walker's `call_arg_escapes` gate is preserved — escape-marked args skip the temp wrapping).
  3. `aetherc --diagnose=ownership` now reports `HEAP` for functions matching shape (1); previously it reported `NOT HEAP — ≥ 1 return literal/borrowed/unclassified`. Tooling expecting the pre-fix verdict for these shapes should treat the new HEAP verdict as the correct classification.

## [0.153.0]

### Added

- **`uint64` source type and wide prefixed-integer literals** (`compiler/parser/lexer.c`, `compiler/parser/parser.c`, `compiler/analysis/typechecker.c`, `compiler/analysis/type_inference.c`, `compiler/codegen/codegen_expr.c`, `tests/syntax/test_uint64_hex_literal.ae`). Adds `uint64` as a first-class declared type at every parser site that previously accepted `int` / `int64` (top-level functions, locals, params, struct fields, exports, actor state, patterns). The lexer no longer decimalises `0x` / `0o` / `0b` literals at scan time — the original literal text now flows through to codegen, so a 64-bit hex literal like `0xB5026F5AA96619E9` is no longer silently truncated by an intermediate `strtol`-and-snprintf step. Codegen translates the Aether-only `0o` and `0b` prefixes into C-acceptable forms at emit time (`0o777` → `0777`, `0b1010` → `0xa`; `0x` and decimal pass through unchanged) — this preserves the literal width through to the C compiler regardless of source spelling. `infer_from_literal` recognises prefixed integer literals and picks the narrowest integer kind that holds the value (`int` ≤ INT_MAX, `int64` ≤ INT64_MAX, otherwise `uint64`); plain decimal literals beyond INT_MAX get the same widening so bare large constants don't truncate in the emitted C either. Typechecker bit-op / shift-op inference (`infer_binary_type`) now uses `is_integer_scalar` + `wider_integer_kind` helpers, so mixed `int` / `int64` / `uint64` operands promote to the widest kind on both sides. Caveat: bare `1 << 63` is still `int`-wide UB — write `uint64 one = 1; one << 63` or use a wide-enough literal.

### Removed

- **Legacy `runtime/memory/memory.c` deleted** (`Makefile`, `runtime/memory/memory.c`). Dead code — no callers anywhere in the runtime / stdlib / compiler (verified by `grep` for every exported symbol). The file's `aether_free` libc-freed pool-interior pointers (heap-corruption primitive waiting for the first caller); `aether_calloc` had unchecked `count * size`; no cap-tracker integration. Supported allocators (`runtime/memory/aether_arena.c`, `runtime/memory/aether_pool.c`) route through `aether_caps_malloc/free` correctly and are unchanged. Removed from `RUNTIME_SRC` and the ci-portability runtime file list.

### Fixed

- **Argument-temp lifetime for nested heap-returning calls** (`compiler/codegen/codegen_expr.c`, `compiler/codegen/codegen_internal.h`, `compiler/codegen/codegen_stmt.c`, `tests/regression/test_return_escape_dup_arg.ae`). `f(heap_value(), heap_value())` and similar shapes produced two anonymous heap temporaries with no consumer — pre-fix this was a leak per call (10002 leaks across 5000 iterations confirmed by macOS `leaks(1)`). The codegen now wraps every call site that has heap-returning AST_FUNCTION_CALL subexpressions in argument position with a GCC statement-expression that hoists each into a named temporary, runs the parent call, and frees them after the parent returns. The wrap is suppressed when the corresponding callee parameter is storage-shaped (`@retain`, `ptr`, or unknown-typed) — the same `call_arg_escapes` gate the escape walker uses, so containers (`list_add_raw`, `map_put_raw`'s key, etc.) and opaque externs that stash the pointer don't lose ownership. Adds the `ArgDrainSub` registry + `arg_drain_mint_name` / `arg_drain_bind` plumbing so nested wraps mint distinct temp names by reserving names BEFORE recursing into `generate_expression`. Promotes `is_heap_string_expr`, `call_arg_escapes`, and `lookup_callee_param_kind` to internal-visible for the cross-module call from `codegen_expr.c`.

- **Return-statement codegen drains unreturned heap-tracked locals** (`compiler/codegen/codegen_stmt.c`, `tests/regression/test_return_escape_return_in_loop.ae`). A function with two return paths where only one returns a heap-tracked local — the avn `find_decorated(prefix, n)` shape with an early `return candidate` inside the loop body and a separate `return string.concat(prefix, "_fallback")` after the loop — pre-fix leaked the last iteration's `candidate` buffer on the after-loop path (3573 leaks across 5000 mixed iterations on macOS `leaks(1)`). The function-exit defer-free is suppressed for return-escape vars (otherwise the caller's pointer dangles), but the suppression was unconditional — now each return statement emits inline conditional frees for every return-escape var the statement does NOT return. The no-defer single-value path routes through a `_no_defer_ret` C local to observe `_heap_<name>` before the C `return` consumes the function frame.

- **Self-recursive heap-returning classification** (`compiler/codegen/codegen_stmt.c`, `tests/regression/test_return_escape_recursive.ae` already covered). The classifier's cycle-break (`heap_pending` annotation) returns 0 during a function's own analysis, which previously made `walk_join`-style accumulator-passing recursive functions classify non-heap and silently UAF when the base case `return param` returned a buffer that the recursive caller's arg-drain wrap immediately freed. The walker now treats `return f(...)` inside f's body as optimistically heap — sound because the uniform-heap shim's cold path malloc-duplicates literal returns; over-classifying costs at most one copy per literal-return path, under-classifying caused UAF.

- **Uniform-heap shim routes through `aether_caps_malloc`** (`compiler/codegen/codegen.c`). The cold-path duplication in `aether_uniform_heap_str` was bypassing the resource-caps tracker (`runtime/aether_resource_caps.h:99`). Hosts that arm `aether_set_memory_cap(N)` could not bound the literal-return-strdup path. The shim now forward-declares `aether_caps_malloc` and uses it for the dup; matching `free()` at the caller side remains plain libc-free per the caps API's documented "libc-compatible pointer" contract.

- **`heap_tracker_return_escape_no_leak` macOS unit normalisation** (`tests/integration/heap_tracker_return_escape_no_leak/shim.c`). The test's RSS-growth threshold compared `growth > 256` (KB) against `getrusage(RUSAGE_SELF).ru_maxrss`, which is in KB on Linux/BSD but in BYTES on macOS. The macOS noise floor of 32-128 KB allocator-zone bookkeeping compared as 32768-131072 "KB" and tripped the threshold spuriously. Shim now divides by 1024 on `__APPLE__`/`__MACH__` so the cross-platform threshold means the same thing on every host.

- **Uniform-heap return-escape contract — closes the v0.149 "lucky-UAF" trade-off** (`compiler/codegen/codegen.c`, `compiler/codegen/codegen.h`, `compiler/codegen/codegen_internal.h`, `compiler/codegen/codegen_stmt.c`, `docs/memory-management.md`, `tests/regression/test_return_escape_*.ae`). Heap-returning functions now transfer ownership cleanly across the call boundary: every return value is routed through a small inline helper `aether_uniform_heap_str(<expr>, <flag>)` so the caller's `free()` works regardless of which branch produced the value. The classifier's return-walk now OR-folds (a function is heap-returning if **any** return is heap, not only if **every** return is heap) and `is_heap_string_expr` recognises bare identifiers of heap-tracked locals — together these two changes mean the heap branch is no longer mis-classified non-heap and silently leaked. The shim is `AetherString`-aware: it byte-by-byte probes for the magic header (matches `is_aether_string`'s ASan-clean shape) and uses the stored length so length-bearing returns survive the cold-path duplication intact. The codegen's escape-mark plumbing now splits into two channels — *container-escape* (call-arg, struct-field, closure capture, `@retain`) suppresses the reassign-wrapper-free as before, while *return-only-escape* lets the reassign-wrapper-free fire so in-loop accumulator buffers like `sorted = string.concat(sorted, "x")` are reclaimed every iteration. The function-exit defer-free is still suppressed for both channels (otherwise the caller's pointer dangles). The first-declaration path for `lhs = <heap_tracked_rhs>` gains the alias-ownership transfer that already existed on the reassignment path, so `sorted = new_sorted; return sorted` moves the heap flag instead of duplicating it. Hot path is one load + one branch + one return (~2 ns). Pure-literal functions emit no shim at all — zero overhead. Ten regression tests under `tests/regression/test_return_escape_*.ae` cover bare-local, literal, mixed, runtime-mixed, user-fn-chain, recursive, hot-loop (100k iterations × 200-byte accumulator, the avn-bench shape), short-string, empty-string, and interpolation. All pass under ASan and under the macOS `leaks` tool with zero leaks reported — the avn-bench shape that previously leaked ~227 MB across 100k iterations now stays flat. Pass-through caveat: `identity(s) -> string { return s }` where `s` is a parameter is classified non-heap (parameters are not tracked locals) and the result is a borrow; force a heap copy with `string.concat(s, "")` if the caller needs to free.

- **`-Wstrict-prototypes`-clean codegen for zero-arg functions** (`compiler/codegen/codegen.c`, `compiler/codegen/codegen_func.c`, `tests/integration/strict_prototypes/`). Forward declarations, extern declarations, and function definitions for no-arg Aether functions now emit `(void)` rather than `()`. Without the fix every Aether `foo()` emits `int foo()` (unprototyped — accepts any args under K&R rules) instead of `int foo(void)`, which is immediately fatal under `-Werror=strict-prototypes` and warns under default modern gcc / clang. Regression test compiles a source containing one extern and one definition through `aetherc` and asserts the emitted C carries `(void)` at all three sites; also recompiles the emitted TU under `-Werror=strict-prototypes` to verify the warning category stays silent.

## [0.152.0]

### Added

- **`std.lzf` one-shot compression/decompression** (`std/lzf/`, `tests/integration/lzf_roundtrip/`). Vendors Marc Lehmann's BSD-licensed liblzf (`lzf.h`, `lzfP.h`, `lzf_c.c`, `lzf_d.c`) and wraps it with the canonical split-accessor / Go-style-tuple stdlib shape: `lzf_try_compress` / `lzf_get_compress_bytes` / `lzf_get_compress_length` / `lzf_release_compress` (+ inflate siblings) on the C side; `lzf.compress(data, length) -> (bytes, n, err)` and `lzf.decompress(data, length, output_length) -> (bytes, n, err)` on the Aether side. Binary-safe: input may contain embedded NULs (length-aware via `is_aether_string` dispatch in `lzf_unwrap_bytes`), output is materialised through `string_new_with_length` so the returned `AetherString` carries the byte count. Empty input / output is handled at the wrapper without invoking the codec (LZF defines no empty-stream form). `lzf.max_compressed_size(n)` exposes the worst-case bound for caller-side buffer sizing. LZF streams carry no length prefix, so `decompress` requires the caller to supply the expected `output_length` (caller's responsibility to persist alongside the compressed bytes, as Redis RDB does). Integration test covers text round-trip, binary payload with embedded NULs, short-output-length rejection, corrupt-input rejection, and empty round-trip. Use case: enables porting Redis's RDB-string compression path (and other size-vs-speed sensitive systems work where zlib is too heavy).

## [0.151.0]

### Fixed

- **Heap-string ownership: 0.149's escape-marked-LHS-alias-clear over-applied to return-escape, leaking buffers in heap-accumulator-return functions (avn's O(N²) perf regression)** (`compiler/codegen/codegen_stmt.c`, `tests/integration/heap_tracker_return_escape_no_leak/`). The 0.149 fix at the escape-marked-LHS-alias path emitted `{ <lhs> = <rhs>; _heap_<rhs> = 0; }` to keep the buffer alive for the escape recipient. That's the right shape when LHS is escape-marked because of a container edge (`list.add`'s `ptr` param, `map.put`, struct field write) — the recipient takes ownership. But the same emission applies when LHS is escape-marked because it's the function's return value, and in that case there is no recipient: clearing the source flag leaves the buffer unowned (callee's defer-free skipped because the source flag was cleared; caller's free skipped because the function isn't classifier-classified as heap-returning — the bare-identifier return `return sorted` isn't recognised by `is_heap_string_expr`). Result: every call leaks ~N bytes (the accumulator size); the avn bench (`rebuild_dir`, `sort_packed_by_first_field`, 5 sites total) showed O(N²) total bench growth — 71 ms/commit at 0.148, **1.4 s/commit at 0.149 batch 5**, RSS 6.8 GB vs 0.4 GB at commit 350.

  Trigger pattern (avn `rebuild_dir` / `sort_packed_by_first_field`):

  ```aether
  make_accumulator(n: int) -> string {
      new_sorted = ""
      while ... {
          new_sorted = string.concat(new_sorted, dir_entry_line(...))
      }
      sorted = new_sorted        // alias; `sorted` escape-marked by `return sorted`
      return sorted
  }
  ```

  At `sorted = new_sorted`, `sorted` is escape-marked (by the AST_RETURN_STATEMENT walker), so the codegen suppresses the reassignment wrapper. The 0.149 added a clear of `_heap_new_sorted` here to prevent the source's later wrapper-free from dangling a container recipient — but for this shape there is no container, just the return.

  Fix is a three-piece architectural change:

  1. **Always-transfer in escaped-LHS-alias emission.** Replace 0.149's source-flag-clear with `{ <lhs> = <rhs>; _heap_<lhs> = _heap_<rhs>; _heap_<rhs> = 0; }`. The heap flag IS the ownership token; moving it on alias preserves the invariant that exactly one variable claims the buffer. For container-escape sites the transferred `_heap_<lhs> = 1` sits unused (LHS's defer-free is suppressed by the escape mark), so no behavioural difference there. For return-escape sites the transferred flag carries the runtime heap-ness across the return.

  2. **Classifier extension** at `walk_returns_for_heap_check`. The classifier now treats `return <bare-tracked-local>` as heap (previously `is_heap_string_expr` only recognised function-call and string-interpolation RHS shapes — bare identifiers fell through to non-heap). Caller-side wrapper at `result = some_func(...)` sets `_heap_<result> = 1` and the next reassignment / function-exit defer-free correctly frees the returned buffer.

  3. **Runtime-uniform-heap shim at the return statement.** At each bare-identifier return of a heap-tracked local in a classifier-heap function, emit a runtime check: `if (!_heap_<retvar>) { /* strdup the literal */ }`. This handles the literal-path edge case — if internal control flow left the runtime flag at 0 (e.g. an `if cond { s = string.concat(...) }` branch wasn't taken), the function returns a literal at runtime even though the static classification is "heap". Without the shim, the caller's `free()` against a literal would crash. With the shim, the returned buffer is ALWAYS heap-allocated when the static classification says so.

  Combined effect: callee transfers ownership across the return boundary; caller takes it and frees correctly. avn's `rebuild_dir` bench stays flat (~0 KB growth across 500 iterations vs ~770 KB pre-fix). No UAF on any platform: Linux no longer relies on allocator-reuse latency (the buffer is genuinely owned, not freed-and-re-read), and Windows MSVCRT no longer scribbles the slot on free because there is no callee-side free. Container-escape sites (`list.add` / `map.put` / struct field with alias-RHS) keep working — the always-transfer is harmless there (defer-free for the escape-marked LHS is still suppressed).

  ### Upgrade notes

  This release closes a hard perf regression introduced in 0.149.0 by replacing 0.149's source-flag-clear with full ownership transfer + classifier extension + runtime-uniform-heap shim. No user code change required.

  **What flips behaviourally:**

  1. Functions matching the heap-accumulator-return pattern (alias into a local then `return <local>`) no longer leak the accumulator buffer per call. Bench-time growth that was O(N²) under 0.149 returns to O(1) per call.
  2. These functions are now classifier-classified as heap-returning; their callers will set `_heap_<result> = 1` and free the buffer at the next reassignment / function-exit defer-free. Code that explicitly held onto these return values past their assignment lifetime (passing them to a `ptr`-typed param, capturing them in a closure, writing them to a struct field, returning them) was already escape-handling-tracked and continues to skip the defer-free — no change there.
  3. The runtime-uniform-heap shim emits one strdup per bare-identifier-return-of-heap-tracked-local when the runtime flag is 0 (typically an empty initial value that survived to the return). Cost: one branch + occasional strdup at the return statement. No perceptible impact in typical hot paths (the avn bench's flat-RSS result holds at 128 KB growth across 500 iterations — within the 256 KB-allocator-bookkeeping threshold).
  4. `aetherc --diagnose=ownership` output for return-escape sites now shows `_heap_<lhs> = <transferred-from-rhs>` at the assignment line and the runtime-uniform-heap-shim emission at the return line. Tooling parsing the diagnose output for these sites should treat both as expected.

  No recommended pre-upgrade ladder for memory safety: this is a strict improvement over both 0.148 (latent UAF on the return-escape pattern, masked by allocator-reuse latency on Linux but visible on Windows) and 0.149 (leak on the same pattern, visible everywhere). User code that worked at 0.148 continues to work; user code that hit the 0.149 leak gets a proper fix.

## [0.150.0]

### Added

- **`ae help <script>` — offline diagnostics for closure-DSL config-IS-code scripts** (`tools/ae_help.{c,h}`, `tools/llm_shim.c`, `tools/ae.c` dispatch, `Makefile` LLM gating, `tests/integration/ae_help/`, `tests/integration/ae_help_privacy/`, `docs/cic-help.md`, `docs/build-system.md`, `README.md`). Closes #414 + #415. Six heuristic capabilities, all running on-machine with zero network access: (1) Levenshtein-matched suggestions over imported export sets — `lenght(s)` → `Did you mean: length (in std.bytes)?`; (2) YAML/HCL pattern detection inside closure-DSL blocks — `port: 9990` and `port = 9990` both rewrite to `port(9990)`; the block-context discriminator is `<ident>.<method> {` (closure-DSL) vs bare `main() {` (function body), so normal Aether assignments inside function bodies are NOT flagged; (3) missing-import detection against the live stdlib export catalog (scanned at startup from `std/*/module.ae`); (4) library-author-shipped `<name>.help.md` hint matching — sections with `Pattern: \`name\`` clauses fire when `name(` appears in the script; (5) top-level closure-DSL detection — `my_lib.serve { ... }` at file scope surfaces a `Wrap in main()` suggestion; (6) doc cross-links — every finding type maps to the relevant doc page for follow-up reading. Three output modes: human-readable (default), `--json` (single JSON object on stdout, escaped via a minimal in-file emitter), and `--fix` (prints the unified diff, prompts for confirmation, atomic write-then-rename; refuses on non-TTY stdin to keep CI safe). **Hard privacy contract**: no network calls (Linux CI `strace -e trace=network` guard verifies zero socket/connect/sendto/recvfrom calls during a multi-finding run), no file reads outside the script + its resolvable imports + co-located hint files, no execution of the script. The `--llm <weights.gguf>` opt-in escalation (#415) gates behind `AETHER_ENABLE_LLM=1` at build time; the default build omits the entire LLM module so the binary stays small and link line dependency-light. When enabled, the shim in `tools/llm_shim.c` targets the stable `llama.h` C API (`llama_load_model_from_file` → `llama_decode` greedy-sampling loop → token-piece stream to stdout). User supplies their own `.gguf` weights; the shim refuses to fetch / download / cache anything, and the privacy guard covers the LLM path too. The error-source design choice: rather than carving a parallel "diagnose mode" into the typer (~500 LOC of surgery), we run real aetherc with stderr captured and parse its already-structured `error[Eabcd]:` lines — zero risk of typer regressions from a parallel code path, and the operator sees exactly the same errors the build path would surface. 11-case integration test covers every capability + the `--fix` non-TTY refusal + the `--llm` flag-OFF clean rebuild message. Bare `ae help` (no script arg) keeps printing the usage banner; the disambiguator only claims the command when argv1 ends in `.ae` and the file exists.
- **Multi-entry `--lib` search path + `ae lib-path` introspection** (`compiler/aether_lib_path.h`, `compiler/aether_module.{c,h}`, `tools/ae.c`, `tests/integration/run_lib_path/`, `docs/module-system-design.md`, `README.md`). Closes #413. `ae run` / `ae build` / `aetherc` and the `AETHER_LIB_DIR` env var all accept PATH-style lists (`./lib:~/aether-libs` on POSIX, `./lib;C:/aether-libs` on Windows) AND compose with repeated `--lib` flags (`--lib a --lib b` ≡ `--lib a:b`). Left-to-right resolution with first-hit-wins; up to 8 entries (overflow warns and drops; tweakable via `AETHER_LIB_DIRS_MAX` if anyone hits it). Cache key now incorporates the lib path AND each entry's mtime, so a precedence flip (`--lib dirA:dirB` vs `--lib dirB:dirA`) and a `touch` inside a vendored module both invalidate the right cache slot. Default unchanged: when neither `--lib` nor `AETHER_LIB_DIR` is set, behaviour is identical to today (a single `lib/` root). Trailing slashes are normalised so `--lib ./lib/` and `--lib ./lib` dedup. The separator macro lives in a dedicated `aether_lib_path.h` so the compiler-side parser and the CLI-side appender share one source of truth without dragging AST headers into the tools build. The cache-key tokeniser uses `strtok_r` for re-entrancy. **New QoL command:** `ae lib-path` prints the resolved search order — same parser as the build path — for debugging "why isn't my import resolving?" without re-reading shell config. Seven-case integration test (the four documented surfaces, a precedence-flip case, a trailing-slash-normalisation case, and an `ae lib-path` introspection assertion). `ae help` and `aetherc --help` both grow a one-liner documenting the syntax; `ae help` lists the new `lib-path` subcommand.

### Fixed

- **User functions named after libc symbols no longer collide at link time** (`compiler/codegen/codegen.c`, `tests/integration/c_symbol_namespace/`). Closes #436 facet B. The codegen's `is_c_reserved_word` list grew from ~50 entries (C keywords + a handful of libc names) to the full POSIX.1-2017 + glibc/musl extension surface: every name in the libc network sockets API (`bind`, `listen`, `accept`, `connect`, `socket`, `select`, `poll`, `epoll_*`, `kqueue`, `kevent`, `recv*`, `send*`, `getsock*`, `setsock*`, `shutdown`, `inet_pton`, `inet_ntop`, `getaddrinfo`, `gethostbyname`), POSIX I/O (`open*`, `read`, `write`, `pread`, `pwrite`, `readv`, `writev`, `lseek`, `fcntl`, `ioctl`, `pipe*`, `dup*`, `f?sync`, `mkdir*`, `rmdir`, `unlink*`, `rename*`, `link*`, `symlink*`, `readlink*`, `chmod`, `fchmod`, `chown`, `fchown`, `lchown`, `stat`, `fstat`, `lstat`, `fstatat`, `access`, `faccessat`, `truncate`, `ftruncate`, `umask`, `fopen`, `fclose`, `freopen`, `fread`, `fwrite`, `fseek`, `ftell`, `rewind`, `fflush`, `fileno`, `feof`, `ferror`, `fprintf`, `fscanf`, `vfprintf`, `vfscanf`, `fgets`, `fputs`, `fgetc`, `fputc`, `ungetc`, `getc`, `putc`, `getchar`, `putchar`), process control (`fork`, `vfork`, `exec*`, `wait*`, `kill*`, `raise`, `signal`, `sigaction`, `sigprocmask`, `alarm`, `pause`, `sleep`, `usleep`, `nanosleep`, `getpid`, `getppid`, `gettid`, `getsid`, `getpgrp`, `getpgid`, `setpgid`, `setsid`, `setpgrp`, `getuid`, `geteuid`, `getgid`, `getegid`, `setuid`, `seteuid`, `setgid`, `setegid`, `abort`, `exit`, `_exit`, `_Exit`, `atexit`), memory + dynamic linking (`malloc`, `calloc`, `realloc`, `reallocarray`, `free`, `mmap`, `munmap`, `mremap`, `mprotect`, `madvise`, `msync`, `brk`, `sbrk`, `posix_memalign`, `aligned_alloc`, `valloc`, `dlopen`, `dlsym`, `dlclose`, `dlerror`, `dladdr`), string + stdio + memory ops (`strlen`, `strnlen`, `strcpy`, `strncpy`, `stpcpy`, `stpncpy`, `strcat`, `strncat`, `strcmp*`, `strchr`, `strrchr`, `strstr`, `strdup`, `strndup`, `strtok*`, `strtol*`, `strtoul*`, `strtod`, `strtof`, `strerror`, `sprintf*`, `vsprintf*`, `sscanf*`, `vsscanf*`, `printf`, `puts`, `gets`, `perror`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `memrchr`), time + env + misc (`time`, `clock`, `gettimeofday`, `clock_gettime`, `clock_settime`, `gmtime`, `localtime`, `mktime`, `asctime`, `ctime`, `strftime`, `getenv`, `setenv`, `unsetenv`, `putenv`, `clearenv`, `getcwd`, `chdir`, `fchdir`, `system`). Every hit gets transparently prefixed with `ae_` by the pre-existing `safe_c_name` helper — the Aether-side identifier keeps its natural spelling (`bind`, `read`), the emitted C symbol becomes `ae_bind` / `ae_read` and stays out of libc's way. Mechanism identical to the legacy handling of `read` / `write` / `malloc` / `free` (which were already in the list); the change is consistency — the rest of the POSIX surface gets the same treatment as the handful of names that were caught before. Aether-keyword collisions (e.g. `send` is reserved as an actor-messaging keyword) are caught one layer earlier at the parser, so they're not in this list; the codegen mitigation covers the names that ARE valid Aether identifiers but would clash with libc. The full architectural goal — module-qualified codegen for every Aether function (`my_module_bind` not `ae_bind`) — would break `--emit=lib` consumers who `dlsym()` for unprefixed symbol names; the list-based mitigation here is the safer middle path that closes the link-time collision class without changing the ABI shape. 14-case regression test (`bind`, `listen`, `accept`, `connect`, `recv`, `select`, `pipe`, `fork`, `kill`, `stat`, `chmod`, multi-collision coexistence, emitted-C-symbol verification, negative `bare-bind-not-defined`) — every case asserts the user's function runs (not libc's) AND for `bind` specifically that the emitted C is `ae_bind`.
- **Selective-import + local-def shadow no longer silently recurses** (`compiler/aether_module.c`, `tests/integration/selective_import_shadow/`, `docs/module-system-design.md`). Closes #436 facet A. A module (or main program) that selectively imports a name AND defines a local function with the same name used to compile successfully and then stack-overflow at runtime — the merger renamed the body's bare call to point at the local def, turning a thin import-forwarding wrapper into self-recursion with no compile-time signal. New orchestrator-level check (`check_selective_import_shadow` in aether_module.c, applied both per-module in `orchestrate_module` and to the entry-point AST in `module_orchestrate`) detects the pattern before merging and rejects with `error[E1000]` — a precise diagnostic naming the colliding local def's source location, the import line it shadows, and three concrete fixes (rename local, drop selective import, use qualified call). Hard error: the alternative (silent runtime stack overflow) is the bug being closed. Covers both `AST_FUNCTION_DEFINITION` and `export <fn>` shapes. Facet B (libc-symbol collisions) is now ALSO closed in the same PR — see the entry above.

## [0.149.0]

### Fixed

- **Heap-string ownership: two follow-up regressions from 0.147.0's alias-transfer fix** (`compiler/codegen/codegen_stmt.c`, `tests/regression/test_list_add_heap_alias_escape.ae`, `tests/regression/test_destructure_reassign_over_heap_lhs.ae`). The 0.147 fix closed the bare-`B = A` alias UAF by transferring `_heap_<rhs>` to `_heap_<lhs>` in the reassignment wrapper. Two adjacent shapes the wrapper didn't reach in 0.147 surfaced afterwards.

  **Regression A — alias into escape-marked LHS doesn't clear source's flag.** When LHS is marked escaped (e.g. about to be passed to `list.add` whose `ptr` parameter triggers escape-marking, or to `map.put`, or written to a struct field via #0.144's binary-expression-LHS edge), the codegen suppresses the reassignment wrapper entirely and emits a bare assignment — so the 0.147 ownership-transfer never fires. Subsequent reassignment of the source then runs `free()` against the buffer the escape recipient still references. Trigger pattern (typical "split-string, accumulate words into a list"):

  ```aether
  while string.length(rest) > 0 {
      nl = string.index_of(rest, "\n")
      line = ""
      if nl < 0 {
          line = rest                // bare-assign (line is escaped) — flag stays on rest
          rest = ""                  // wrapper sees _heap_rest=1, frees buf
      } else {
          line = string.substring(rest, 0, nl)
          rest = string.substring(rest, nl + 1, string.length(rest))
      }
      list.add(out, line)            // list keeps pointer to now-freed buf
  }
  ```

  Fix: in the escaped-LHS bare-assign path, when RHS is a bare identifier referring to a heap-tracked local (and not self-assignment), also emit `_heap_<rhs> = 0` to clear the source's ownership flag. The heap flag IS the ownership token; with the source's flag cleared, its later reassignment can't fire a wrapper-free against the buffer the escape recipient still holds. Net effect: the buffer stays alive (held by the recipient), and no wrapper-free fires against it. Strictly better than the pre-fix UAF.

  **Regression B — destructure-reassign-over-heap-LHS with non-string position type.** The tuple-destructure wrapper-emission gate at the AST_TUPLE_DESTRUCTURE codegen path required the destructure POSITION's type to be `TYPE_STRING`. `map.get` returns `(value: ptr, err: string)` — position 0 is `TYPE_PTR` (an opaque pointer from the map's internal storage, irrespective of the value's actual type). With `pos_is_string = false`, codegen fell through to bare assignment and the previous heap-string value's tracker flag never got cleared. The function-exit defer-free then ran `free()` against the map's borrowed storage. Trigger pattern:

  ```aether
  pkg_include = "${pkg_name}*"             // _heap_pkg_include = 1 (interp)
  pkg_include, _ = map.get(opts, "k")      // bare assign — flag stays at 1
  // ... fn returns ...
  // defer-free: free(pkg_include) against map's borrowed storage → SIGABRT
  ```

  Fix: widen the wrapper-emission gate to fire whenever the LHS is heap-string-tracked, regardless of the destructure position's type. The wrapper frees the previous heap-string value (correct — the LHS owns it) and sets the new heap flag to 0 for non-string positions (borrow assumption — the destructured value is an opaque pointer from the heap-string-tracker's perspective, never a fresh-malloc'd char* the source's wrapper should claim).

  Both fixes are surgical: A is one new branch in the escaped-LHS bare-assign emission; B is one widened gate condition in the tuple-destructure path. No changes to escape analysis, classifier, or function-exit defer-free machinery.

  ### Upgrade notes

  Both fixes close UAFs that previously surfaced as silent corruption (Regression A) or crash-on-function-exit (Regression B). No code change required in user code — the fixed compiler produces correct output for the canonical patterns that previously broke.

  **What flips behaviourally:**

  1. Code matching Regression A's pattern (heap-tracked local → escape-marked recipient → source reassigned) now keeps the buffer alive for the recipient's lifetime instead of freeing it mid-loop. Net result: items stored in lists / maps / struct fields via the split-loop pattern are now intact at read time.
  2. Code matching Regression B's pattern (heap-tracked LHS reassigned via destructure with non-string position 0 — typically `var, _ = map.get(...)` after `var = "${interp}"` or `var = string.concat(...)`) no longer aborts at function exit. The previous heap value is correctly freed at the destructure site.
  3. `aetherc --diagnose=ownership` now reports the correct `_heap_<lhs>` verdict for these two assignment shapes (was silently emitting stale flags pre-fix).

  Memory-pressure footprint: Regression A's fix shifts a freed-too-early buffer to a "stays alive until recipient frees it" shape. If your recipient never frees its members (e.g. a list you keep around for the program's lifetime), this is a leak — but strictly better than the silent UAF it replaces. The escape-analysis pass's job is to surface this trade-off; the wrapper-free skip is conservative-correct by design.

  No recommended pre-upgrade ladder. Sanitiser builds (ASan / Valgrind) over hot paths using the list-accumulate-strings and destructure-over-heap-var patterns will show fewer heap-use-after-free / invalid-free reports post-upgrade.

### Changed

- **`docs/c-interop.md` Best Practices**: added explicit note (point 3) against the "mirror stdlib symbols via `extern` declarations" anti-pattern. Older Aether had link-time issues with shared modules importing stdlib (Issue #309-era) that made the manual-extern shape look attractive as a workaround; those bugs are closed and the import-then-namespace shape (`import std.list` + `list.add(x)`) is the only supported path. Closes a doc-gap surfaced when a downstream porter (Aeocha) discovered its 68-line manual-extern block was technical debt rather than a feature.

## [0.147.0]

### Fixed

- **Identifier-alias of a heap-string + reassign-source no longer dangles** (`compiler/codegen/codegen_stmt.c`, `tests/regression/test_string_alias_reassign_uaf.ae`). The reassignment-wrapper at codegen_stmt.c:1611-1631 used `is_heap_string_expr(rhs)` to decide whether to set the LHS heap flag, and that predicate only recognises function-call and string-interpolation RHS shapes — bare `AST_IDENTIFIER` RHS always returned 0. Consequence on the canonical "alias then reassign source" pattern:

  ```aether
  rest = string.substring(name, 6, 11)   // _heap_rest = 1
  word = rest                            // _heap_word = 0  (BUG)
  rest = ""                              // wrapper sees _heap_rest=1, frees buf
  println(word)                          // garbage — UAF
  ```

  Pre-fix, the alias `word` dropped its tracker on assignment while `rest` kept it; the source's next reassignment-wrapper then freed the buffer the alias still pointed at. This was a silent UAF that surfaced as garbage payload bytes (the AetherString header was intact, so `string.length(word)` still returned 5, but the bytes were recycled). The previous version's behaviour was the same shape with the opposite failure mode: pre-defer-free (≤ ~0.132) it was a leak (both pointers stayed valid but the source's allocation accumulated); the defer-free machinery added in 0.139 / 0.143 / 0.144 / 0.145 tightened reclamation for every classifier-recognised shape, leaving the bare-identifier-alias edge as a pure UAF.

  The canonical idiom that hits this is "split a string, accumulate words":

  ```aether
  while string.length(rest) > 0 {
      dash = string.index_of(rest, "-")
      word = string.substring(rest, 0, dash)        // alias-then-keep
      rest = string.substring(rest, dash + 1, ...)  // reassign source
      // word now reads freed memory
  }
  ```

  Fix: at the reassignment wrapper, when the RHS is a bare identifier referring to a heap-tracked local (and not self-assignment), emit ownership-TRANSFER instead of zeroing the LHS flag:

  ```c
  // Pre-fix:
  { _tmp_old = word; word = rest; if (_heap_word) free(_tmp_old); _heap_word = 0; }

  // Post-fix:
  { _tmp_old = word; word = rest; if (_heap_word) free(_tmp_old);
    _heap_word = _heap_rest; _heap_rest = 0; }
  ```

  The heap flag IS the ownership token — there is exactly one buffer; exactly one slot should hold the freeing duty. Moving the flag on alias keeps the buffer alive until the alias goes out of scope; the source's later reassignment frees nothing because its flag is now 0. No per-function alias map needed (a simpler resolution than the bug-filing's proposed Option A). Self-assignment guard: `a = a` falls through to the pre-existing emission shape — a separate concern (it was already incorrect; this fix doesn't worsen or improve it).

  Six-case regression test exercises: alias + reassign-to-empty, alias + reassign-to-literal, alias of string.concat result, chained alias-of-alias (`word = mid; mid = rest; rest = source`), the hyphen-split-loop pattern, and a literal-source no-regression guard.

  ### Upgrade notes

  This release closes a use-after-free that previously corrupted the alias's payload silently. Code that was working around it (manual `string.concat(rest, "")` after alias to take an explicit copy) is still correct under the new behaviour — the explicit copy is just no longer required.

  **What flips behaviourally:**

  1. Code that aliased a heap-string AND reassigned the source got freed-memory reads pre-fix. Post-fix the alias keeps the buffer alive until it goes out of scope. The reader will now get correct payload bytes where previously it got garbage.

  2. Memory-pressure footprint at the alias point shifts: pre-fix, the alias's apparent zero-tracker-flag meant the function-exit defer-free skipped it (leak avoidance via wrong direction). Post-fix the alias's transferred flag means the defer-free fires at function exit (correct reclamation). Net result: same allocation count, freed at the right time instead of leaked or double-touched.

  3. `aetherc --diagnose=ownership` now prints `_heap_<alias> = transferred-from <source>` for alias-assignment sites where the source was heap-tracked (was previously `_heap_<alias> = 0`). Tooling that parses the diagnose output by exact-equality on `= 0` / `= 1` should re-parse for the transfer shape if it cares to distinguish.

  No recommended pre-upgrade ladder: the change closes a UAF without introducing one. Sanitiser builds (ASan / Valgrind) over hot paths that touch the alias-then-reassign pattern will show fewer heap-use-after-free reports post-upgrade.

## [0.146.0]

### Added

- **`@heap` annotation on single-value extern returns** (`compiler/parser/parser.c`, `compiler/codegen/codegen_stmt.c`, `tests/integration/extern_single_value_heap/`). The parser already accepted per-position `@heap` / `@borrow` on tuple-returning externs (issue #420) via `Type.tuple_heap_flags`; the parallel channel did not exist for non-tuple `-> string` returns because `Type` has no "this whole thing is heap" slot outside `tuple_heap_flags`. Result: `extern http_request_body(req: ptr) -> string` and every other single-value extern that returns a malloc'd C buffer the caller is supposed to own was permanently stuck at `_heap_<lhs> = 0` at every call site — the reassignment-wrapper free never fired, the function-exit defer-free never ran, every call leaked the underlying buffer. avn's read path hits this on every `body = http.request_body(req)`. There was also no user-side workaround: the `string_concat(call(), "")` refactor that worked for cross-Aether-fn classification does NOT help here — `string_concat` copies its inputs into a fresh allocation but never frees the originals, so wrapping just adds a second allocation alongside the leaked first one. New syntax: `extern foo(...) -> string @heap` (opt-in, default unchanged is `@borrow`). `@heap` on `-> int`, `-> ptr`, or `-> void` is a parse error — only meaningful on `-> string`. Parser stores `"heap_return"` on the extern's annotation slot using the same combine-dedupe pattern as the per-param `@aether` / `@retain` handler (parser.c:2800-2813). Codegen extends `is_heap_string_expr` with a `find_extern_declaration_by_name` lookup that walks both `program->children` and import-statement-resolved module ASTs (matching the diagnose code's pattern at codegen_stmt.c:4142) — externs aren't cloned into the program AST by `module_merge_into_program`, so the lookup has to traverse explicitly. User wrappers around an annotated extern (`wrap_owned(seed: string) -> string { return owned_string(seed) }`) inherit HEAP via the existing `function_def_returns_heap_string` walk — no extra annotation needed at the wrapper level. Test exercises the classifier verdict via `aetherc --diagnose=ownership` plus a 10k-iter runtime probe with a malloc-counting shim that verifies the wrapper-emitted free runs without crashing on borrowed-pointer over-frees.

  ### Upgrade notes

  This release **adds** new syntax. The default behaviour for every existing `extern` declaration is unchanged: unannotated returns stay classified non-heap (conservative borrow), matching the pre-fix semantics. No existing callers are affected by the addition alone.

  However, **once an extern is annotated `@heap`**, every call site to that extern flips from "wrapper free skipped" to "wrapper frees the previous value at next reassignment". Functions that previously leaked the underlying buffer per call now reclaim it. This is the intended correctness fix, but it changes behaviour for any caller that:

  1. Stored the returned string somewhere the escape analyzer doesn't recognise (raw stuffing into an opaque arena, hand-rolled pointer arithmetic) — the reassignment-wrapper free will reclaim the buffer at the next assignment to the variable, and the stored alias becomes a dangling pointer.
  2. Aliased the returned pointer into a struct field via a setter and read it back through a getter — covered by the #0.144 struct-field escape fix; verify your project is on 0.144+ before annotating high-traffic externs.

  **Recommended pre-upgrade play:**

  1. **For downstream stdlib annotations (separate release):** when an `extern` gets `@heap` added, run `aetherc --diagnose=ownership` over your project. Every `_heap_<lhs> = 1` line that newly flips on is a call site whose previous-value-free now fires; audit whether any code path retained a borrowed copy of `<lhs>` past the next assignment.
  2. For any annotation you add yourself to a project-local extern, verify the C side genuinely returns a `malloc`'d (or equivalent libc-`free`-compatible) buffer — the codegen emits a plain `free((void*)_tmp_old)`, not header-aware `string_release`, so a `static`-buffer or `string_new_with_length`-style AetherString return would crash with a heap-corruption diagnostic on the second iteration.
  3. Re-run sanitiser builds (ASan / Valgrind) over hot paths that touch newly-annotated externs.

  No stdlib externs have been annotated in this release — that audit pass ships in a separate change once we've confirmed the C side of each high-traffic extern (`http_request_body`, `fs.read_to_string`, `json_stringify_raw`, the openssl/zlib wrappers …) is libc-`free`-compatible.

## [0.145.0]

### Fixed

- **Tuple-destructure heap classification now resolves cross-module callees + json wrappers refactored to uniform-heap return shapes** (`compiler/codegen/codegen_stmt.c`, `std/json/module.ae`, `tests/regression/test_json_get_string_loop_leak.ae`). Two coupled fixes that together close the residual heap-tracker gap surfaced in `further-bug-fix4.md`'s 2026-05-10 update — avn's 5000-commit bench retained ~666 MB just from `b64, _ = json.get_string(jcontent)` because that destructure's wrapper emitted `_heap_b64 = 0` and the function-exit defer-free skipped it.

  - **Cross-module callee dot-normalisation at the destructure-time analyzer lookup.** `function_def_returns_heap_at` runs against the callee's function definition fetched by `find_function_definition_by_name(gen->program, rhs->value)`. Source-level callees land in the AST in dotted form (`"json.get_string"`) but the merged user-fn lives in the program AST under the underscored namespace-prefixed name (`"json_get_string"`). Pre-fix the lookup missed every cross-module call, the per-position analyzer never ran, and `pos_is_heap` fell back to 0 even when the callee was uniformly heap-classifiable. Routed `rhs->value` through `codegen_normalise_callee` first — same dot-normalisation pattern `is_heap_string_expr` already uses on its hardcoded fast-path lookups. The fix also unlocks classifier-correctness for every other `module.fn`-style cross-module destructure in user code.

  - **Stdlib json wrappers refactored to uniform-heap return shapes.** `json.parse`, `json.parse_strict`, `json.stringify`, `json.get_string`, `json.object_entry` previously returned a bare literal `""` on one side of the if-else (failure path) and `string_concat(raw, "")` on the other (success path). `function_def_returns_heap_at`'s AND-fold then yielded non-heap at the affected position (conservative correctness — a mixed-shape position can't be eager-freed at compile time without a runtime header dispatch we don't have), so the destructure wrapper never fired in the success path's hot loop. Refactored each return to inline `string_concat(...)` at every string position — `string_concat("", "")` on the cheap failure side, `string_concat(raw, "")` on the success side — so the AND-fold yields heap uniformly. Tiny extra allocation on the failure side (a one-byte heap empty string) in exchange for the success-side leak going away. Also surfaced as a side-effect: where the success path used to write through an intermediate `owned = string_concat(...)` local before returning `owned, ""`, `is_heap_string_expr` couldn't follow the bare-identifier-at-return shape back to its assignment — so the rewrite also inlines the `string_concat` call directly at the return position. Same shape for every wrapper this touched.

  ### Upgrade notes

  This release tightens the per-position tuple-destructure heap classifier: cross-module user-fn callees now resolve correctly, and `json.parse` / `json.parse_strict` / `json.stringify` / `json.get_string` / `json.object_entry` now classify their string return positions uniformly as heap. The previous version's destructure wrapper at every `_, _ = json.<fn>(...)` call site emitted `_heap_<lhs> = 0` and the function-exit defer-free skipped the var, so the owned-string copy leaked across loop iterations and to process exit.

  If your project loops over `json.get_string` / `json.parse` / `json.parse_strict` / `json.stringify` / `json.object_entry` and stores the destructured string result somewhere outside the destructure scope (struct field, list, map, actor message field, closure capture) without the escape-analyzer recognising it — the previous compiler tolerated this as a slow leak; this release will now `free()` that string at function exit, dangling whatever pointer the recipient stored. This shape is a use-after-free.

  **Recommended pre-upgrade play:**

  1. Run `aetherc --diagnose=ownership` over your project to surface every `_heap_<lhs> = 1`-flipped variable that touches a `json.*` destructure.
  2. For each, check whether the value is handed to a `ptr`-parameter (already escape-marked), returned (already escape-marked), captured by a closure (already escape-marked), assigned to a struct field (escape-marked as of #0.144 — fix landed last release), or stored some other way the analyzer doesn't yet catch (e.g. raw stuffing into an opaque arena). The last case is the dangerous one; rewrite to take a copy.
  3. Re-run your sanitiser builds (ASan or Valgrind) on the destructure-heavy hot paths after rebuilding.

  Conservative migration: if any of your code does `_, payload = json.get_string(v); some_extern_that_keeps_payload(payload)` without an `@retain` annotation on the recipient's parameter, the recipient will now read freed memory. Annotate the recipient `@retain` (so the escape walker marks `payload` escaped) or take an explicit copy.

## [0.144.0]

### Fixed

- **Struct-field assignment now counted as an escape edge for heap-string vars** (`compiler/codegen/codegen_stmt.c`, `tests/regression/test_struct_field_string_escape.ae`). The escape-analysis pre-pass behind the function-exit defer-free (`mark_escaped_heap_string_vars`, the #420 follow-up) recognised four escape edges: function-call arg through a `ptr` / `@retain` param, return statement, closure capture, and the AST_VARIABLE_DECLARATION RHS shape. It had no case for AST_BINARY_EXPRESSION with `value="="` — which is the AST shape the parser uses for non-trivial assignment LHS (`s.field = ...`, `arr[i] = ...`). Bare local re-assignment goes through AST_VARIABLE_DECLARATION instead, so it was already covered. The gap meant a heap-tracked local assigned to a struct field inside a setter got reclaimed by the function-exit defer-free when the setter returned, leaving the struct holding a dangling pointer. Same-function write+read masked the bug because the temp lived the full activation record; cross-function setter/getter — the canonical opaque-handle / state-bearing module API shape — hit it every time. avn impact: every commit went out as `"edits":[{}]` because RaCommit.edits_packed was clobbered between `remote_commit_add_file` (writer) and `remote_commit_build_body` (reader); 5000-commit bench couldn't make any progress. Fix: extend `escape_walk` to detect AST_BINARY_EXPRESSION with `value="="` whose LHS is non-bare-identifier (AST_MEMBER_ACCESS for field writes, AST_ARRAY_ACCESS for element writes); mark a bare-identifier RHS as escaped if it names a heap-tracked var. New regression test exercises both shapes (21-byte heap-built string round-tripped through a setter, plus a byte-for-byte 16-byte verification).

  ### Upgrade notes

  This release widens what the escape analyzer treats as a "store outside the current activation record": `s.field = cp` and `arr[i] = cp` now keep `cp`'s buffer alive past function exit, instead of letting the function-exit defer-free reclaim it. The previous version reclaimed the buffer eagerly, which manifested as silent dangling-pointer reads from the struct field anywhere outside the writing function.

  If your project stores a heap-string into a struct field and the writing function expected the buffer to be freed at scope exit (very unlikely — that pattern was always broken under the old behaviour, just symptomatically silent until you read the field cross-function), the field will now retain ownership for the struct's lifetime instead. This shape leaks the buffer on the writer side IF the receiver never frees it explicitly — strictly better than the dangling-read it replaces.

  **Recommended pre-upgrade play:**

  1. Build under the new compiler and re-run any test that exercises a setter/getter pair across a function boundary; previously-corrupted struct fields will start returning correct content.
  2. If your project tracks heap-string lifetimes across struct ownership, audit any `string.release` calls on a struct field after the field's owner has been freed — these may now over-free if the owner's destructor already releases.

## [0.143.0]

### Fixed

- **`string.substring_n` on AetherString-shaped inputs returned the header struct's leading bytes instead of the payload slice** (`std/string/aether_string.c`, `tests/regression/test_substring_n_on_bytes_finish.ae`). Silent data corruption when the source string was built via `bytes.finish`, `string_concat_wrapped`, `string_new_with_length`, `fs.read_binary`, or any other path that produces a wrapped `AetherString*`. Pre-fix, `string_substring_n` cast its first arg straight to `(const char*)str` instead of routing through `str_data()`. Inputs without a header (literals, the bare-`string_concat` return) happened to work because the cast was a no-op there. The codegen's auto-unwrap pass at the call site is *skipped* for `string_*`-prefixed externs (`is_stdlib_string_aware_extern`) on the assumption that they'd dispatch internally — every other `string_*` extern (`string_length`, `string_char_at`, `string_concat_wrapped`, etc.) does, but `substring_n` was an outlier. Fix uses `str_data()` to honour the AetherString header uniformly. `str_len_bytes` parameter remains authoritative for slice bounds (don't strlen-truncate at embedded NULs — that's why the `_n` variant exists). Surfaced by avn's `repo_storage/module.ae:rep_encode_blob` chain (`bytes.new` → `bytes.copy_from_string` → `bytes.finish` → `string.substring_n` → `fs.write_binary`): every blob landed on disk as `57 ae 01 …` (header bytes) instead of the expected `52 …` ('R' + body) or `5A …` ('Z' + zlib), so the read-side envelope-tag check failed and every commit short-circuited to error. New regression test exercises the four canonical input shapes: bytes.finish source, string.concat source, full-range slice from bytes.finish, and the avn rep_encode_blob 2-byte-header-drop pattern.

### Removed

- **`contrib/aeocha/` spun out into [aether-lang-org/aeocha](https://github.com/aether-lang-org/aeocha)** (`contrib/aeocha/`, `Makefile`, `tests/integration/aeocha_aeb_ipc_reporting/`, `tests/integration/aeocha_expect_matchers/`, `tests/regression/test_closure_extern_ordering.ae`, `tests/regression/test_closure_shadow_tuple_destructure.ae`, `.buildkite/pipeline.yml`, `README.md`, `LLM.md`). The BDD-style test framework is now a standalone sibling repo. It graduates to a top-level `aeocha` namespace (dropping the `contrib.` prefix) and is consumed downstream as a standard Aether package. Removed: the framework itself; related integration and regression tests; the Buildkite self-test step; and references in the main repository's canary installer checks. README and `LLM.md` updated to reflect Aeocha's graduation to a sibling project.

## [0.142.0]

### Fixed

- **Imported modules at scale: selectively-imported names now propagate, and the 128-decl truncation cap is lifted** (`compiler/aether_module.c`, `tests/integration/import_selective_propagation/`, `tests/integration/import_large_module/`). Two coupled holes in the cross-`import` merge codepath that the 0.141.0 struct-merge fix surfaced once large per-file shims started being merged into single `<dir>/module.ae` modules.

  - **Selective-import propagation across the `import` boundary.** A module M that does `import X (a, b, c)` and uses `a(...)`, `b(...)` as bare names inside its function bodies type-checked fine standalone — M's symbol table aliased `a` to `X_a`. But once a consumer imported M, the cloned function bodies still said `a(...)` verbatim, and the consumer's typer had no `a` in its symbol table (the consumer never wrote `import X`), so it fired E0301 on every call site. New `apply_inherited_selective_imports` walks M's own `AST_IMPORT_STATEMENT`s at clone time and rewrites bare-name references in the cloned body to the prefixed form (`a` → `X_a`), so the consumer's transitive cross-module merge can resolve them through the existing prefixed-symbol path. Wired into all three clone sites: main merge loop, transitive helpers pull-in, and the cross-module BFS for transitive deps.

  - **Decl-table cap lifted from 128 to 4096.** Every clone-and-rename pass collects a snapshot of the source module's function and constant names so intra-module calls (`square(...)` referencing a sibling) get rewritten to the prefixed form (`mathy_square(...)`) before the body lands in the consumer. Pre-fix this snapshot was hardcoded to 128 entries at every collection site. Modules merged from large per-file shims (avn's `working_copy/module.ae`, ~150 functions in 4300 lines) silently truncated at the 129th decl — every call to a beyond-128 sibling stayed bare and the consumer's typer fired E0301 even though the prefixed symbol existed in the registry. Centralised as `AETHER_MODULE_MAX_DECLS = 4096` (with a stack-frame budget rationale in the macro's docstring) and applied to every collection / selection / aggregation site in the file. Headroom for ~32× the current largest known shim before we'd need dynamic allocation.

  Verified: standalone `ae check` of a 200-function helper module + cross-`import` consumer round-trip prints the expected arithmetic; `make ci` and `HARDEN=1 make ci` green; new regression tests `tests/integration/import_selective_propagation/` (innerlib + middlemod + consumer) and `tests/integration/import_large_module/` (200-function chain + recursion across the boundary) lock both shapes in.

### Changed

- **Install layout consolidated to a single `<prefix>/lib/aether/` subdir** (`install.sh`, `tools/ae.c`, `Makefile`, `docs/install-layout.md`, `docs/c-interop.md`, `docs/c-embedding.md`). Pre-fix the two installers disagreed: `install.sh /usr/local` placed the static archive at `/usr/local/lib/libaether.a` (flat, FHS-canonical), while `make install PREFIX=/usr/local` placed it at `/usr/local/lib/aether/libaether.a` (subdir, co-located with `libaether_<contrib>.a`). Downstream embedders (especially LLM-generated build scripts) routinely guessed the FHS-flat path and missed the file under one of the two installers. Subdir wins because it co-locates contrib archives and gives a single `-L<prefix>/lib/aether` line that resolves both core and contrib libs uniformly. `install.sh`'s `LIB_DIR` now points at `<prefix>/lib/aether`; `tools/ae.c` drops the flat-path probe fallbacks and looks only at the subdir; `Makefile`'s `test-release-archive` produces a subdir-shaped tarball matching what `release.yml` ships. Doc surface (`install-layout.md`, `c-interop.md`, `c-embedding.md`) now points consumers at `$(ae cflags)` rather than hand-crafted `-I` / `-L` / `-l` lines — the canonical recipe is a single `ae cflags` invocation that emits the correct flags for in-tree dev builds, `~/.aether` user installs, and `/usr/local` system installs uniformly.

- **`make install` and `make install-contrib` self-restore `build/` ownership after `sudo` invocations** (`Makefile`). `sudo make install` re-runs the `release ae stdlib` build deps as root, leaving every object/archive/binary in `build/` root-owned. The next plain `make` then failed with "Permission denied" until the user manually `chown`ed back. Both targets now end with `if [ -n "$$SUDO_USER" ] && [ -d build ]; then chown -R "$$SUDO_USER:..." build 2>/dev/null || true; fi` — silent no-op outside sudo, silent no-op on `chown` failure, transparent self-heal under sudo.

### Removed

- **`contrib/aether_ui/` spun out into [aether-lang-org/aether-ui](https://github.com/aether-lang-org/aether-ui)** (`contrib/aether_ui/`, `Makefile`, `docs/aether-ui-windows.md`, `docs/aether-ui-testing.md`, `docs/stdlib-vs-contrib.md`, `README.md`, `LLM.md`). The widget toolkit (GTK4 / AppKit / Win32 backends + AetherUIDriver HTTP test server) is now a sibling repo, consumed downstream the same way external users consume Aether (`import` against the installed `share/aether/` tree + `$(ae cflags)` for the link line). Removed: the directory itself; `contrib-aether-ui-check` and `benchmark-aether-ui` Makefile targets and their `make ci` step (now [9/9] not [10/10]); the UI-specific docs `docs/aether-ui-windows.md` and `docs/aether-ui-testing.md` (those move to the spin-out repo). README and `LLM.md` get a "Sibling Projects" / "Working with downstream users" section pointing at both `aether-ui` and [aeb](https://github.com/aether-lang-org/aeb) (the build system, also a downstream consumer of `share/aether/MANIFEST`). `docs/stdlib-vs-contrib.md` gets a "Spun out to sibling repos" subsection capturing the graduation path: `contrib/` → standalone repo when a module's release cadence and audience diverge from the core compiler's.

## [0.141.0]

### Fixed

- **Struct definitions in imported modules are now visible to that module's own merged function bodies** (`compiler/aether_module.c`, `tests/integration/import_struct/`). `module_merge_into_program` only cloned `AST_FUNCTION_DEFINITION` / `AST_BUILDER_FUNCTION` / `AST_CONST_DECLARATION` from imported modules into the consumer's program AST — never `AST_STRUCT_DEFINITION`. A module that exposed a `*ptr`-returning constructor whose body internally cast to its own struct (`raw as *Slot`) typechecked standalone but failed once a consumer did `import handle`, because the cloned function body's cast site resolved against an empty struct table and errored as `'Slot' is not a struct type`. Fix clones struct decls into the consumer's program AST under their bare name (no `<ns>_` prefix), with a `program_has_struct` dedup helper. Selective-import filter is bypassed for structs because a function body that casts to `*T` cannot type-check without `T` in scope. Both the main merge loop and the cross-module BFS pass (transitive deps) are extended. Unblocks the opaque-handle pattern (struct stays an internal implementation detail; only `ptr` crosses the API boundary). New integration test `tests/integration/import_struct/` exercises constructor + getter + setter round-trip across the import boundary.

## [0.139.0]

### Added

- **Function-exit defer-free for hoisted heap-string vars + `@retain` extern-param annotation** (`compiler/codegen/{codegen.{c,h},codegen_func.c,codegen_internal.h,codegen_stmt.c}`, `compiler/parser/parser.c`, `std/{collections,fs,os}/module.ae`, `tests/regression/test_string_leak_function_exit.ae`, `docs/memory-management.md`). Issue #420 v2 — the deferred half of the tuple-destructure fix, plus the safety infrastructure required to ship the stdlib annotations without UAFs. Three coupled pieces:

  - **`push_heap_string_exit_free_defers`** runs after the existing `mark_escaped_heap_string_vars` pre-pass. For every hoisted heap-string variable that is NOT escaped, push a synthetic `AST_EXPRESSION_STATEMENT` carrier annotated `"heap_string_exit_free:<name>"` onto the function-scope defer stack. The defer-emit loops in `codegen.c` recognise the annotation prefix and render `if (_heap_<name>) { free((void*)<name>); <name> = NULL; _heap_<name> = 0; }` at every function exit and explicit `return`. Closes the single-call leak shape — a heap-string variable assigned once, never reassigned, that previously leaked at function exit. Same shape applies to both the single-value path and the tuple-destructure path from #420 v1; one defer-free covers both.

  - **`@retain` per-parameter annotation on extern declarations.** Parser now accepts repeated `@<attr>` markers between `:` and the type, storing them comma-separated on `param->annotation` so multiple stack (`@aether @retain string` is legal). New `params_retain` parallel array on the extern registry (lazy-allocated; NULL when no param uses the annotation). New `is_retain_extern_param` lookup consulted by the escape walker before the existing TypeKind-based escape rule: if a heap-string arg is passed at a `@retain`-typed slot, the walker marks the var escaped so the function-exit free skips. Without it, default "string param is read-only" treatment would UAF every retainer (string-typed function that stores the pointer beyond the call).

  - **Latent `_builder_ret` type bug fix.** The defer-aware return path computed `_builder_ret`'s C type from the return *expression*'s `node_type` — NULL/UNKNOWN for plain `return ident;` shapes, falling back to `int`. Latent because no functions had defers AND a bare-identifier return AND a non-int return type until the new defer-pre-pass started pushing defers for previously-leaking heap-string locals. Fix prefers `gen->current_func_return_type` (the function's declared return type) over the expression's stamped type. Unblocks `contrib/aeocha/module.ae:_format_aeb_report` and three other functions that had latent heap-string leaks.

  Stdlib annotations landed in the same change:
    - `fs_realpath_raw(path) -> (string @heap, int, string)` — POSIX `realpath(NULL)` / Windows `GetFinalPathNameByHandleW` UTF-8 malloc
    - `os_run_capture_status_raw(...) -> (string @heap, int, string)` — captured stdout buffer (realloc-grown)
    - `os_run_pipe_drain_and_wait_raw(...) -> (string @heap, int, string)` — pipe payload (realloc-grown)
    - `string_list_add(list, s: @retain string)` — list stores the AetherString reference
    - `string_list_set(list, idx, s: @retain string)` — same as add
    - `map_put_raw(map, key: @retain string, value: ptr)` — map stores its key reference

  Each annotation was paired with an audit: every Aether-side caller checked for paired `string.release` calls (none found across stdlib, tests, examples, tools, and contrib) and the C-side return path verified to malloc/realloc the position-0 buffer. The audit is captured in the extern's doc comment so a future porter touching these knows the heap-ness was verified, not guessed.

  Verified: 215/215 C unit tests + 463/463 ae regression + integration tests green on a clean rebuild. New regression test `test_string_leak_function_exit.ae` adds 5 cases exercising the four corners (single-value vs tuple destructure × non-escaped vs `@retain`-escaped) plus a mixed heap+literal tuple destructure to confirm the per-position classifier from #420 v1 stays correct under the new defer-free pre-pass.

- **Tuple-destructure heap-string tracker** (`compiler/ast.{h,c}`, `compiler/codegen/codegen_stmt.c`, `compiler/parser/parser.c`, `tests/regression/test_string_leak_tuple_destructure.ae`, `docs/memory-management.md`). Closes the codegen gap from issue #420. Heap strings unpacked from a `(string, int, string)`-shaped return now flow through the same `if (_heap_<lhs>) free((void*)_tmp_old); _heap_<lhs> = N;` wrapper that the AST_VARIABLE_DECLARATION single-value path uses on every reassignment — so a tight loop that destructures a heap-tuple-returning function steady-states at the size of one allocation per LHS, instead of accumulating linearly by `<heap-bytes-per-call> × iterations`. Three pieces:
  - **Per-position structural escape analyzer** (`function_def_returns_heap_at`) for user-defined tuple-returning functions. Mirrors the existing single-value `function_def_returns_heap_string`: walks every `return e1, e2, e3` and AND-folds `is_heap_string_expr(e_position)` per position. Memoised on the AST node's annotation slot as `"heap_positions:1,0,1"` so subsequent codegen queries are O(positions). Cycle break stamps `"heap_pending"` exactly like the single-value analyzer; mutual recursion through `-> (string, int)` functions terminates with a conservative non-heap verdict.
  - **`@heap` / `@borrow` per-position annotations on tuple types** (parser side). New syntax — `extern foo(s: string) -> (string @heap, int, string @borrow)` — populates a parallel `int* tuple_heap_flags` array on `struct Type` (length matches `tuple_count`). Default for unannotated string positions is `@borrow` (= flag 0), preserving the silent pre-#420 behaviour for every existing tuple-returning extern. The `@aether` per-param annotation handler at parser.c:2712 is the precedent — same idiom, no new lexer token, peek `TOKEN_AT` then expect identifier `heap` / `borrow`.
  - **Codegen wrapper-emit at the `AST_TUPLE_DESTRUCTURE` site**. For each `<lhs_i> = _tup<n>._<i>;` component assignment, if the tuple position's type is `TYPE_STRING` and the LHS is a hoisted heap-tracker var, emit the wrapper. User-defined RHS fn → consult the per-position analyzer. Extern RHS or anything else → consult the parser's `tuple_heap_flags`. Non-string positions stay plain assignment with no spurious `_heap_<int_var>` emit. The escape gate from #405 (`is_escaped_string_var`) is consulted exactly as in the single-value path: an LHS that may have been stored externally takes a plain assignment (strictly leaks the previous value, strictly better than UAF). `_` discard slot at a heap position emits `if (_tup<n>._<i>) free((void*)_tup<n>._<i>);` so name-less heap values don't leak across the destructure.

  Verified: 215/215 C unit tests + 462/462 ae regression + integration tests green; existing tuple-destructure tests (`test_closure_shadow_tuple_destructure.ae`, `test_tuple_destructure_cross_module.ae`) unaffected because their non-string positions don't touch the new emit path. New regression test `test_string_leak_tuple_destructure.ae` covers four shapes: both-heap user fn loop, mixed `(string, int)` user fn, per-position-flips re-destructure, cross-block destructure reassignment.

### Upgrade notes

- The tuple-destructure wrapper is enabled unconditionally for any LHS that's already a hoisted heap-string-tracker var (i.e. any string-typed local in the function body). For tuple-returning **user functions** the per-position analyzer reads each return statement; existing user functions that return heap from one position and a literal from another are correctly classified. For tuple-returning **externs** the default is `@borrow` (no behaviour change unless you opt in); annotating an extern as `@heap` starts auto-freeing the corresponding LHS on reassignment, which is the desired loop-leak fix BUT will UAF if any caller is doing manual `string.release` on the returned pointer. Audit any manual `release` calls paired with a tuple-returning extern before adding `@heap` to its declaration.
- Stdlib-side annotation of known-heap externs (`fs_realpath_raw`, `os_run_capture_status_raw`, `os_run_pipe_drain_and_wait_raw`) is **deliberately deferred** to a follow-up PR. Each annotation is a behaviour change for every downstream caller; landing them safely needs a per-callee audit (find every Aether and C-side caller, verify none does manual `string.release` on the returned pointer, check the test surface). Doing all three at once expands scope past what this PR can verify; doing them piecemeal is the right shape. The parser + codegen + analyzer infrastructure is in place — annotations land in any subsequent PR with no further compiler change.
- This fix addresses the **loop reassignment** leak shape (same LHS destructured many times). Functions that destructure once per call and exit without reassignment still leak the heap value at function exit — same shape as the existing single-value heap-tracker path, not specific to tuple destructures. Adding automatic function-exit defer-free for hoisted heap-string vars is a separate enhancement that affects both code paths and is tracked as a follow-up.

## [0.138.0]

### Added

- **`aetherc --diagnose=ownership` — string-ownership audit without codegen** (`compiler/codegen/codegen_stmt.c`, `compiler/codegen/codegen.h`, `compiler/aetherc.c`). Walks the parsed/typechecked program and prints the same heap/non-heap verdicts the wrapper terminator at `codegen_stmt.c:1611-1631` will emit during codegen — without running codegen. Output is two passes: (1) every string-returning user function with its `function_def_returns_heap_string` verdict (HEAP if every return path is heap-classified, NOT HEAP if any return is literal/borrowed/unclassified — same memoised structural-escape recursion the wrapper uses); (2) every string-typed `AST_VARIABLE_DECLARATION` (Aether's "decl" covers both first-assignment and reassignment) with the `_heap_<lhs> = N` value the wrapper would emit after that assignment, plus a one-word RHS shape (`literal` / `borrow from another variable` / `string interpolation → HEAP` / `heap-returning fn → HEAP` / `fn call (not heap-classified)`). Final stanza names the UAF shape this surfaces: any line ending `_heap_<lhs> = 1` will have the wrapper free `<lhs>`'s previous value at the next reassignment, so if that previous value was handed to a function that stores the pointer raw (map.put values, list.add, actor message fields with `: ptr`), the next reassignment dangles the stored pointer. Designed for the leak-fix-unmasks-UAF debugging pattern: running this on a downstream codebase before upgrade flags every variable that just became heap-tracked, so the porter can audit whether the previous value has alias-using sites in the same codepath.

- **CHANGELOG `### Upgrade notes` convention for memory-fix releases** (`CONTRIBUTING.md`). New convention: PRs that touch `compiler/codegen/codegen_stmt.c` (the heap-string-tracker wrapper) or `runtime/aether_string.c` (the refcount allocator) include an `### Upgrade notes` block in their CHANGELOG entry naming what alias / ownership patterns the new behaviour can break in downstream code, plus the recommended pre-upgrade play (run `aetherc --diagnose=ownership` over the project, audit newly-heap-tracked variables for aliasing). Goal: a downstream that hits "rebuilt under new aetherc, server crashes" maps the failure to a known shape on the very first report instead of running a manual debugging ladder.

### Fixed

- **`is_heap_string_expr` ignored dotted callees, leaving 0.135.0's heap-string-tracker fix dead in real-world code** (`compiler/codegen/codegen_stmt.c`). Follow-up to the architectural fix shipped in 0.135.0 (#405). The hardcoded stdlib fast-path strcmp'd against the underscore form (`"string_concat"` etc.) but the AST stores source-level callees in their dotted form (`"string.concat"`), so the leaf check returned 0 for every dotted stdlib call and — transitively, via the structural-escape recursion — for every user-defined function whose body called one. The wrapper terminator therefore emitted `_heap_<name> = 0` after every reassignment, leaving the guarded free as `if (0) free(...)` and orphaning the previous heap allocation. Net behaviour was identical to pre-0.135.0; the architectural piece (function-entry tracker hoisting) had landed but the value the tracker carried was always wrong. Fix mirrors the dot→underscore normalisation already used at `compiler/codegen/codegen_expr.c:2425-2432` (the builder-funcs registry comparison) — six lines at the top of `is_heap_string_expr`'s function-call branch, normalising into a 256-byte stack buffer before both the stdlib allowlist strcmp and the user-fn `find_function_definition_by_name` lookup. The existing six-case regression test (`tests/regression/test_string_leak_user_function.ae`) already used the dotted source form and passed only because it asserts string correctness, not RSS bounds — the silent leak slipped through. Recommend a follow-up to (a) hoist the normalisation into a `codegen_normalise_callee` helper so the pattern can't grow a third uncoordinated copy, and (b) tighten the regression test to assert RSS stays O(1) across N=10000 iterations.

- **Heap-string-tracker wrapper now type-aware: skips `free(_tmp_old)` for variables that escape via `ptr` parameters** (`compiler/codegen/codegen_stmt.c`, `compiler/codegen/codegen_func.c`, `compiler/codegen/codegen.c`, `compiler/codegen/codegen.h`, `compiler/codegen/codegen_internal.h`). Once the dot-normalisation fix above made the wrapper actually fire, a latent UAF surfaced: code that built a heap-string into a local and handed it to a function that stores the pointer raw (`map.put` value, `list.add` element, actor message field with `: ptr`) would dangle the stored copy at the next reassignment of the local. New escape-analysis pre-pass (`mark_escaped_heap_string_vars`) walks the function body alongside the existing `hoist_heap_string_trackers`, marks heap-tracked vars whose value is passed as an argument matching a `ptr`-typed parameter (or unknown — safe-default — to a callee whose signature isn't visible), or captured by a closure, or returned from the function. The wrapper at codegen_stmt.c:1611 then emits a plain assignment instead of the `if (_heap_X) free(_tmp_old); _heap_X = N` shape for escaped vars. Conservative: alias-safe (no UAF) at the cost of leaking the previous value for the function's lifetime — strictly better than the pre-pass UAF. The "consumed transiently" exception preserves #405's leak fix for the canonical `s = my_concat(s, "x")` loop: when the LHS of the current assignment matches the bare-identifier on the RHS, the appearance is exempted (the call only reads the old value to build the new one; the recipient doesn't store anything outliving the call). Type-based precision keeps innocuous read-only calls — `string.length(s)`, `string.equals(a, b)`, `print(s)` — from over-marking the var as escaped: a `string`-typed parameter signals read-and-consume, not store. Param kinds resolve via the existing extern registry (`lookup_extern_param_kind`) with dot→underscore normalisation; user-defined fns resolve via `find_function_definition_by_name`. `tests/regression/test_collections_hashmap_edge.ae` is the standing regression — Tests 1 and 10 alias a heap-string into a map across loop iterations and now pass under the original code (no test-side workaround), and `tests/regression/test_string_leak_user_function.ae` Case 1's 1000-iter `s = my_concat(s, "x")` still has its wrapper emitted (verified by inspecting generated C for the `if (_heap_s) free` pattern), keeping #405's O(1)-memory fix intact.

- **Heap-string-tracker hoist completed: C variable now hoisted to function scope alongside the tracker** (`compiler/codegen/codegen_stmt.c`). The original 0.135.0 fix (#405) hoisted only the `int _heap_<name>` tracker to function-entry scope. The C-level `const char* <name>;` declaration was left at its original first-use point, which `hoist_loop_vars` / `hoist_if_branch_vars` then promoted to the loop-enclosing or if-referenced-outside C scope but never to function scope. When the same Aether name was first-assigned in two sibling C-blocks (e.g. `if (...) { ... name = ... }` followed by another `if (...) { ... name = ... }`), the codegen emitted two separate uninitialised C variables sharing one function-scoped tracker. The wrapper at the second block's first assignment read `_tmp_old = name` from the freshly-declared (uninitialised) stack slot, evaluated `if (_heap_name)` against the tracker (= 1 from the first block's last iteration), and called `free()` on stack garbage — glibc abort. Reproduced against a real downstream project's commit-bench (the `rebuild_dir` shape: an `if { while { name = ... }}` followed by another `if { ... name = ... }`). Fix extends `hoist_heap_string_trackers` to additionally emit `const char* <name> = NULL;` at function entry and call `mark_var_declared` so the per-stmt codegen routes ALL assignments — including the syntactic "first" assignment — through the reassignment path's wrapper. The wrapper then reads `_tmp_old` from the function-scoped slot (NULL initially, last-iteration value across sibling blocks) — never from a freshly-declared per-block stack slot. Architectural intent now self-consistent: tracker AND the variable it tracks both at function scope, lock-step. Skipped names (tracker still hoisted; only the C-var hoist is skipped): function parameters and other already-declared names; actor state vars (accessed via `self->name`); env captures (accessed via `_env->name` in closure body); promoted captures (declared as `int* name = malloc(...)` by the closure-promotion path). New `tests/regression/test_string_sibling_blocks.ae` is the targeted regression — pre-fix this aborts with `free(): invalid pointer` when the seed argument's length > 0, post-fix runs to completion and passes correctness + 200x stress.

### Upgrade notes

This release tightens string-ownership behaviour: variables reassigned from heap-returning calls (`string.concat`, `string.substring`, `string.to_upper`, `string.to_lower`, `string.trim`, and any user-defined `-> string` function whose every return path yields one of those) now correctly free their previous value at the next reassignment. The previous version of the compiler had the architectural fix (#405, in 0.135.0) but a one-character mismatch in the leaf check made the heap-tracker permanently stuck at 0 — so the free was guarded by `if (0)` and the previous value leaked. With this release the free actually fires.

If your project aliases a heap-string across an ownership-transfer boundary — e.g. stores it in a long-lived structure (map value, list element, actor message field with `: ptr`, struct field, global) and *also* keeps a local pointer to it that you later reassign — the previous compiler tolerated this as a slow leak; this release will correctly free the local's previous value, dangling the alias. This shape is a use-after-free.

**Recommended pre-upgrade play:**

1. Build your project under `aetherc --diagnose=ownership <file.ae>` on the new compiler. The output enumerates every variable that just became heap-tracked (lines ending `_heap_<lhs> = 1`).
2. For each such variable, check whether its previous value is passed to any function that stores the pointer raw (`map.put` values, `list.add`, actor `send` message fields with `: ptr`) before the next reassignment in the same codepath. If yes, fix the alias before upgrading — either pass the value inline at the storage site (`map.put(m, k, string.concat(...))` rather than `v = string.concat(...); map.put(m, k, v)`), or `string.copy` it at the storage boundary.
3. Run integration tests under a sanitiser (`gcc -fsanitize=address` over the generated C; an `aetherc --sanitize=address` driver flag is a follow-up).

## [0.136.0]

### Removed

- **Self-hosted source snapshot release** (`.github/workflows/release.yml`). Dropped the `Update snapshot (main HEAD)` job that published `aether-source.tar.gz` / `aether-source.zip` to a `snapshot` GitHub Release on every merge. The workflow had been failing in CI — GitHub Releases became immutable, and the delete-then-recreate workaround is brittle. Downstream users tracking HEAD should use GitHub's built-in archive URL: `https://github.com/aether-lang-org/aether/archive/refs/heads/main.tar.gz` (always-fresh, no maintenance, extracts to `aether-main/`).

## [0.135.0]

### Added

- **Kind-discriminator + safe deep-free for `AetherValue*`** (`std/collections/aether_collections.c`, `runtime/aether_config.{c,h}`, `runtime/aether_value_kind.h`, `tests/integration/emit_lib_kind_safe/`, `docs/emit-lib.md`). Closes another v1-out-of-scope item. Aether's collections are deliberately untyped at the storage level — historically that meant schema-mismatch bugs (script stores an int where a map was expected, or a list where a map was expected) degraded into host-side segfaults. Every `HashMap` and `ArrayList` now carries a `uint32_t _kind_magic` leading field set at construction (`AETHER_KIND_LIST_MAGIC = 0xAE57E715`, `AETHER_KIND_MAP_MAGIC = 0xAE571A99` — distinct from `AETHER_STRING_MAGIC = 0xAE57C0DE`) and cleared on free. New public ABI in `runtime/aether_config.h`: `AetherKind aether_value_kind(AetherValue* v)`, `int32_t aether_value_is_map(AetherValue* v)`, `int32_t aether_value_is_list(AetherValue* v)` — safe to call on any pointer the host holds, including scalars intptr-cast to `AetherValue*`. Two-layer safety: a low-address guard (`< 0x10000` filtered before any deref — the zero page + first 64 KiB are unmapped on every supported OS) handles common intptr-cast scalars, and a 32-bit magic check rejects higher-address values that survive the guard with false-match probability 2/2³² ≈ 5 × 10⁻¹⁰. New `aether_config_free_deep(root)` walks nested containers in post-order using the predicate so a tree of map+list+scalars cleans up correctly without crashing on the scalars; existing `aether_config_free` now auto-detects map vs list root via the same predicate so it works on either shape uniformly. Defense-in-depth: `list_free` / `map_free` clear the magic before releasing the struct, so a use-after-free probe via `aether_value_kind` reports `UNKNOWN` rather than the freed-but-still-readable original. Integration test compiles a 4-slot tree (`map { name: string, age: int, addr: map, tags: list }`), probes every slot's kind, runs adversarial low-address probes (`(AetherValue*)42`, `0x1`, `0xFF`, `0xFFFF`), exercises a schema-mismatched `_get_map` on the int slot (returns intptr-cast 42; kind probe must report `UNKNOWN` not crash), runs `aether_config_free_deep`, and asserts the post-free probe reports `UNKNOWN` — every assertion passes without segfault, which is the whole safety contract.

- **`ae build --emit=both` produces both artifacts in one invocation** (`tools/ae.c`, `tests/integration/emit_lib_dual_build/`, `docs/emit-lib.md`). Closes one of the documented "out of scope for v1" items in `docs/emit-lib.md`. Previously rejected with "not yet implemented" — now dispatches `cmd_build` twice internally, once as `--emit=exe` and once as `--emit=lib`, with the lib pass's `-o` rewritten to append the platform lib extension (`.dylib` / `.so` / `.dll`) so the two outputs land at distinct paths. With no `-o` both passes use their existing defaults (exe = `<src-base>`, lib = `lib<src-base>.<ext>`) which already differ; with `-o foo` you get `foo` (exe) and `foo.dylib` / `foo.so` (lib) side by side. The `.c` file content for exe vs lib genuinely differs (whether `main()` is emitted), so a single gcc call could not produce both shapes — the two-pass dispatch is structural, not a workaround. Two-fail-fast: if the exe pass errors the lib pass is skipped and the exe's exit code is returned. Existing `tests/integration/emit_lib_dual_build/` flipped from "asserts --emit=both is rejected" to "asserts --emit=both produces both runnable exe + lib-with-aether_greet-symbol" — 5/5 cases green.

- **Contract const-fold elision** (`compiler/codegen/codegen_stmt.c`, `tests/regression/test_contract_const_fold.ae`, `docs/language-reference.md`). Companion to #348. When a `requires` / `ensures` predicate is provably constant-true at compile time (`requires true`, `ensures 1 > 0`, `requires 1 + 1 == 2`, `ensures !false`, `requires true && (3 > 2)`, …), the codegen now drops the runtime check entirely and emits a `/* precondition elided (always-true): <text> */` comment in its place. Generated C is byte-for-byte identical to a function written without the clause — the user keeps the documentation; the binary takes zero overhead. The folder (`try_fold_predicate`) handles bool/integer/float literals, comparison ops (`< <= > >= == !=`), logical ops (`&& ||`), arithmetic (`+ - * / %`), unary `!`, unary `-`. Anything with an identifier reference, function call, or member access (i.e., anything the optimizer can't prove pure-and-known) keeps the runtime check. Conservative on short-circuit: `x || true` does NOT fold to `true` because the runtime evaluation of `x` may carry a side effect. Mixed clauses on the same function are independently folded — one elided + one runtime-checked is supported (the regression test guards this case explicitly).

- **`--emit=lib` symbol-catalog metadata + `ae lib-info` reflection** (`runtime/aether_lib_meta.h`, `compiler/codegen/codegen.c`, `tools/ae.c`, `tests/integration/lib_meta/`, `docs/emit-lib.md`). Closes #403 (MVP). Every `--emit=lib` artifact now exports a single reflection entry point — `const AetherLibMeta* aether_lib_meta(void)` — returning a layout-stable `static const` struct that describes every exported function: Aether name, C symbol, public signature, source file, source line. No JSON, no parsing, no dynamic allocation — the catalog literally lives in `.rodata`, walkable by Python ctypes / Java Panama / Ruby Fiddle / Go cgo / hand-rolled `dlsym` directly. Schema is `"1.0"` with explicit append-only-fields-at-the-tail policy; the `closure_count` / `closures` slots are reserved for v2's closure-context records (the harder half of #403 — capturing Aether closures across the FFI boundary). The codegen emitter (`emit_lib_metadata` in `compiler/codegen/codegen.c`) walks the program's exported functions, renders the public signature via the same `int32_t` / `int64_t` / `float` / `const char*` / `AetherValue*` table the alias surface uses (so the metadata signature matches what the host actually calls), gates `c_symbol` on the `@c_callback` attribute (callbacks expose the bare Aether name as the C symbol; plain exports get the `aether_<name>` prefix), and emits the catalog as a private `_AetherLibMeta` struct so multiple-TU builds don't trip duplicate-tag warnings. `ae lib-info <path-to-library>` is a turnkey CLI reader: `dlopen` → `dlsym("aether_lib_meta")` → format-print the catalog to stdout — the same flow any embedder uses, run interactively. Output suppresses the `c_symbol:` line when it equals `aether_<name>` (printing it would just be noise). Windows `lib-info` is a follow-up (POSIX `dlopen` only in v1); the metadata struct is still emitted into the DLL's `.rodata` and reachable via `LoadLibrary` + `GetProcAddress("aether_lib_meta")` from any host. 7-assertion integration test compiles a 3-export `script.ae` (`@c_callback aether_script_handle` + `double_int(int) -> int` + `greet(string) -> string`) under `--emit=lib`, links the `.c` into a `.so`/`.dylib`, runs `ae lib-info`, and asserts schema "1.0" + source name + function count + three exact signature lines + `c_symbol:` gating + source-file references all round-trip cleanly.

- **`std.fs` completeness bundle** (`std/fs/aether_fs.{c,h}`, `std/fs/module.ae`, `tests/regression/test_fs_{copy,move,realpath,chmod,kind_constants}.ae`, `docs/stdlib-reference.md`, `docs/stdlib-module-pattern.md`, `docs/next-steps.md`). Closes #391. Adds `fs.copy`, `fs.move`, `fs.realpath`, `fs.chmod` — the four primitives that shipped Aether users to `os.system "cp ..."` shell-quoting hell. `fs.mkdir_p` and `fs.symlink` already existed (the issue text was outdated). Performance is per-platform best-primitive: Linux `copy_file_range(2)` (reflinks on btrfs/XFS) → `sendfile(2)` → 8 MiB read/write fallback; macOS `fcopyfile(COPYFILE_DATA)` (APFS clone on same-volume); Windows `CopyFileExW` (kernel block copy with UTF-8 → UTF-16 path conversion via `MultiByteToWideChar(CP_UTF8)`). `fs.move` is `rename(2)` with transparent EXDEV → copy + unlink fallback (atomic on same-fs, correct cross-fs); Windows `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED`. `fs.realpath` is POSIX `realpath(3)`; Windows `CreateFileW(FILE_FLAG_BACKUP_SEMANTICS)` + `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED)` with the `\\?\` prefix stripped. `fs.chmod` is POSIX `chmod(2)`; Windows emulates the user-write bit via `SetFileAttributesW(FILE_ATTRIBUTE_READONLY)`, matching Python's `os.chmod` documented behaviour.

- **Structured-error pilot: `(value, kind, message)` tuple shape** (`std/fs/module.ae`, `std/fs/aether_fs.{c,h}`, `docs/stdlib-module-pattern.md`, `tests/regression/test_fs_kind_constants.ae`). Closes #392 (pilot scope). The four new fs primitives above adopt the (value, kind, message) shape — callers switch on `fs.KIND_*` (one of 13 stable integer constants exported from `std.fs`) instead of parsing English error strings. Kinds returned per-call (not VCR-style globals — std.fs is called concurrently from actors and a shared mutable last-kind slot would race; the tuple costs one extra register on every ABI we target). A single `aether_fs_errno_to_kind(int errno)` helper is the only errno→kind translation site, ensuring the four primitives all classify failures the same way. The shape is opt-in and additive — existing wrappers keep their `(value, err)` returns. Documented in `docs/stdlib-module-pattern.md` as the next-tier shape for stdlib modules with kind diversity.

- **Opt-in build hardening (`HARDEN=1`)** (`Makefile`, `docs/build-system.md`, `CONTRIBUTING.md`). Closes #396. `HARDEN=1 make ...` adds `-fstack-protector-all` (stack canaries on every function — not just gcc-strong heuristic candidates), `-D_FORTIFY_SOURCE=2` (runtime checks on `read`/`write`/`memcpy`/`strncpy`/`printf`-family wrappers; requires `-O1+`, satisfied by the default `-O2`), and `-Wformat -Wformat-security` (printf-format-injection diagnostics). Disabled by default in release builds — runtime overhead is non-zero (~3-5% on micro-benchmarks) and macOS Clang has historically been finicky with `_FORTIFY_SOURCE` on a few setups — but pinned in CI via a Linux/Hardened (gcc) matrix entry so a regression that introduces an unchecked `memcpy`-over-fixed-buffer trips a red check before merge. CONTRIBUTING.md "Before Submitting" recommends a local `HARDEN=1 make ci` for any PR that touches C in `compiler/`, `runtime/`, or `std/`.

- **RISC-V 64-bit cross-CI (`ci-riscv64`)** (`Makefile`, `.github/workflows/ci.yml`, `docs/build-system.md`). Closes #397 (gcc + clang on Linux already shipped as separate matrix entries; this delivers the architectural-portability half of the issue). New `make ci-riscv64` target cross-compiles compiler/ae/stdlib via `riscv64-linux-gnu-gcc` (host x86_64 binary, riscv64 target), verifies the produced ELFs are riscv64 (`file build/aetherc | grep RISC-V`), then smoke-runs `aetherc --version` and `ae --version` under `qemu-riscv64-static` to confirm the binaries actually execute. Optional libs (OpenSSL, zlib, nghttp2, GTK4) are disabled for the riscv64 build because the host runner's pkg-config returns x86_64 lib paths; the std.* feature-detection wrappers fall into their "unavailable" stubs cleanly — same pattern WASM and embedded already use. Catches portability bugs that an x86_64-only matrix can't surface (pointer-width / struct-padding / atomic-instruction-availability differences between architectures). New top-level CI workflow job mirrors the existing `ci-embedded` (ARM Cortex-M4) shape.

- **Eiffel-style runtime-checked contracts: `requires` / `ensures`** (`compiler/parser/{tokens.h,lexer.c,parser.c}`, `compiler/ast.h`, `compiler/codegen/{codegen_func.c,codegen_stmt.c,codegen.h,codegen.c,codegen_internal.h}`, `compiler/aetherc.c`, `tests/regression/test_requires_ensures.ae`, `examples/basics/contracts.ae`, `docs/language-reference.md`). Closes #348. Function declarations may now carry `requires <expr>` and `ensures <expr>` clauses between the typed return arrow and the body `{`. Each clause is a single boolean expression; multiple clauses (freely interleaved) are checked independently so the panic message names the specific failed predicate. Two new tokens (`TOKEN_REQUIRES`, `TOKEN_ENSURES`), two new AST node types (`AST_REQUIRES_CLAUSE`, `AST_ENSURES_CLAUSE`), parser hook between typed-return parsing and body parsing. Codegen lowers `requires <expr>` to `if (!(<expr>)) aether_panic("precondition violation: <text> in <fn>");` at function entry (after parameters declared, before any user code); `ensures <expr>` lowers to a fresh C scope before each `return`: `{ <T> result = <return-expr>; if (!(<expr>)) aether_panic("postcondition violation: <text> in <fn>"); return result; }`. The synthetic `result` local is the return value about to be returned and is scoped to the wrapper block so it shadows any outer `result` cleanly. A small reverse-printer (`fprint_expr_text`) round-trips the predicate AST back to source-like text for the panic message — covers identifiers, literals, binary/unary ops, member access, function calls. New `--no-contracts` aetherc flag (mirrors C's `-DNDEBUG` for assert) skips check emission entirely; `gen->no_contracts` is the codegen-side gate. v1 limitations (called out in docs/language-reference.md): postconditions check only single-value explicit returns; multi-value returns and fall-off-the-end of void functions are follow-up work, as is compile-time const-fold rejection of guaranteed-false predicates and `--emit=lib` `aether_describe()` integration. 8-case regression test plus a runnable example demonstrating happy paths and the panic surface.

- **`std.json` structured-error pilot expansion** (`std/json/{aether_json.c,aether_json.h,module.ae}`, `tests/regression/test_json_structured_errors.ae`, `docs/stdlib-reference.md`). Closes #392 (pilot scope expanded — first non-`std.fs` adopter). Adds `parse_strict(json_str) -> (ptr, int, string)` returning the canonical `(value, kind, message)` triple from the std.fs pilot; `KIND_OK` / `KIND_PARSE_ERROR` / `KIND_OUT_OF_MEMORY` / `KIND_INVALID_INPUT` exported as `const int` from `std.json`. KIND values match `std.fs`'s where they overlap so callers can switch on either surface in the same logic. Programmatic accessors `last_error_kind` / `last_error_line` / `last_error_col` expose the failure context without parsing the human message. C-side: thread-local `g_json_err_kind` / `_line` / `_col` set alongside the existing `g_json_err_buf`; OOM detection via single-site `strstr(msg, "out of memory")` in `err_set` keeps the dozens of existing call sites unchanged. Existing `parse(json_str) -> (ptr, string)` API unchanged — pilot is opt-in and additive per #392's non-breaking contract.

- **String-leak architectural fix: heap-string-tracker hoisted to function-entry scope** (`compiler/codegen/codegen_stmt.c`, `compiler/codegen/codegen_func.c`, `compiler/codegen/codegen.c`, `compiler/codegen/codegen_internal.h`, `tests/regression/test_string_leak_user_function.ae`, `docs/memory-management.md`, `docs/stdlib-reference.md`). Closes #405. Removes the C-scope visibility limitation that blocked the bug_repo.md repro: `_heap_<name>` companion trackers for every string variable are now emitted at function-entry scope by a new pre-pass (`hoist_heap_string_trackers`) that walks the body recursively and registers every name needing a tracker. The first-decl codegen is hoist-aware — emits an assignment-only when the tracker has been hoisted (`_heap_x = N;` instead of `int _heap_x = N;`), avoiding duplicate-declaration C errors. The reassignment path is now a single unified wrapper that handles all four heap/literal transitions (heap→heap, heap→literal, literal→heap, literal→literal) so the tracker stays in lock-step with the actual pointer's heap-ness — fixes a latent bug where heap→literal reassignment left `_heap_<name>` stale and a later free could attempt to release a literal. With the architectural blocker gone, `is_heap_string_expr` is extended to recognise user-defined `-> string` functions via structural escape analysis: a function is treated as heap-returning iff every return statement in its body yields a heap-string-expression (recursive walk, memoised on the AST node's annotation slot, with cycle detection that conservatively returns "not heap"). The bug_repo.md `s = my_concat(s, "x")` loop now collapses from O(N²) to O(1) memory; functions returning string literals (e.g. `file.delete` returning `""` or `"cannot delete file"`) are NOT recognised and their results aren't freed — closes the literal-vs-heap risk that the naive `node_type` check would have introduced. Six-case regression test covers the canonical loop, cross-block reassignment, heap→literal transition, mixed transitions through a helper, nested-block reassignment, and chained user-fn reassignment. The `make ci` test sweep stays green: 215/215 C unit tests + 84/84 regression tests + every integration test we exercise.

- **In-process script gateway (`std.http.script_gateway`)** (`std/http/script_gateway/`, `tests/integration/http_script_gateway/`, `Makefile`, `docs/stdlib-reference.md`). Closes #384. Mounts a pre-compiled `aetherc --emit=lib --with=net script.ae -o script.so` shared library as the HTTP request handler for a path prefix — gives `.ae` scripts the dispatch ergonomics of CGI without paying the per-request fork/exec cost. Empirically ~50× faster than the equivalent subprocess-spawn approach. The script's exported `aether_script_handle(req, res, ud)` symbol is resolved once at mount time via `dlsym(3)` (script must mark it `@c_callback` so aetherc emits the unmangled name) and called directly per request from the connection thread. dlopen flags: `RTLD_NOW` (bind every symbol at load — fail fast on missing runtime references rather than at first request) + `RTLD_LOCAL` (don't promote the script's symbols to the host's global namespace). Mount registers a middleware on the host server; the per-mount `ScriptMount` struct (path_prefix + dlopen handle + entrypoint pointer) flows as the middleware's `user_data` slot. dlopen handles are intentionally long-lived (process-lifetime) — hot-reload is a separate feature outside #384's scope. Sandbox: `mount()` calls `aether_sandbox_check("fs_read", so_path)` before dlopen, so a sandboxed host cannot bring in arbitrary native code via untrusted `.so` paths. Windows host: returns `KIND_UNAVAILABLE` with a clear message — DLL hosting (`LoadLibrary`/`GetProcAddress` with the dllimport/dllexport ABI quirks) is a follow-up commit; the POSIX path covers Linux + macOS + FreeBSD. Returns the same structured-error tuple `(value, kind, message)` as the std.fs pilot (#392); KIND values are a subset that match std.fs's so callers can mix the two surfaces. 4-case integration test: `/gateway/*` dispatches to the .so handler, `/static` runs through to the registered route handler (proves the gateway middleware does not over-match), missing .so path → KIND_NOT_FOUND, .so without `aether_script_handle` export → KIND_INVALID.

## [0.131.0]

### Added

- **DSL block receiver scoping** (`compiler/analysis/typechecker.{c,h}`, `compiler/parser/parser.c`, `tests/integration/dsl_receiver_scoping*/`). Closes #333. Inside a trailing closure bound to a member-access call (`receiver.method(args) { body }`), unqualified function references in the body now fall back through `<receiver>_<name>` after a local-scope miss before erroring as undefined. Removes the `import X (a, b, c)` boilerplate that grows with every new SDK setter. Pure additive: existing selective imports keep working, qualified calls inside the block keep resolving via the unchanged `lookup_qualified_symbol` path, and explicit local definitions still shadow the receiver fallback. `SymbolTable.dsl_receiver` carries the receiver namespace per-scope (NULL on non-DSL scopes; not inherited); `typecheck_function_call` stamps the receiver onto the `AST_CLOSURE` annotation before descending; the closure typecheck reads the annotation and seeds the closure-scope's `dsl_receiver`. When a bare-name call resolves through the fallback, `call->value` is rewritten to `<receiver>.<name>` so codegen (which converts `.` to `_`) emits the resolved C symbol. Three multi-file integration fixtures cover namespace receivers, nested DSL blocks (inner-block receiver wins for inner-namespace names; outer's receiver applies via the natural scope-walk), and the existing-qualified passthrough.

- **Zero-copy `sendfile(2)` fast path for `http_serve_file`** (`std/net/aether_http_server.{c,h}`, `std/http/module.ae`, `tests/integration/http_sendfile/`). Closes #383. Replaces the read-whole-file-into-heap pattern with a `sendfile(2)` syscall when the connection is HTTP/1.1 + cleartext + Linux/macOS + no Range header. `http_serve_file` `open(2)`s the file, `fstat(2)`s it (open-then-fstat closes the stat-vs-content race — Content-Length and the byte count sendfile reads come from the same fd), and stashes (fd, size) on `HttpServerResponse`; the connection writer in `handle_one_request` decides at send time whether to take the fast path. New helpers near `conn_send`: `conn_sendfile_drain` (Linux: `sendfile(out, in, &off, count)` loop with EAGAIN retry; macOS: `sendfile(in, out, off, &len, NULL, 0)` with the inverse argument order); `conn_cork` (TCP_CORK on Linux, TCP_NOPUSH on BSD/macOS, no-op on Windows — coalesces headers + body into one TCP segment); `sendfile_eligible` (gates on `!ssl && !is_h2 && no Range`). Ineligible cases (TLS / HTTP/2 / Range / Windows / sendfile failure) read the fd into `res->body` and fall through to the buffered serialize+send path so client-facing behaviour is identical to before. The same `send_response_with_optional_sendfile` helper is called from BOTH the regular route-handler send site AND the middleware-short-circuit path, so middleware-staged static-file responses (e.g. `middleware.use_static_files`) also take the fast path. Keep-alive correctness: bytes-written-vs-Content-Length mismatch forces the connection closed. `http.serve_file` / `http.serve_static` Go-style wrappers added on top of the existing externs. VCR static-content mounts get the optimization transparently. 4-case integration test: 1 MiB body byte-identical, two-request keep-alive (proves Content-Length matches sendfile output), Range request fallback (200 + full body via buffered path; documented), 404 doesn't leak FD (server still serves further requests).

- **Per-call resource caps for `--emit=lib`** (`runtime/aether_resource_caps.{c,h}`, `runtime/libaether_caps.c`, `include/libaether.h`, `compiler/codegen/codegen_stmt.c`, `compiler/codegen/codegen.c`, `std/string/aether_string.c`, `std/collections/aether_collections.c`, `std/collections/aether_hashmap.c`, `runtime/memory/aether_arena.c`, `runtime/memory/aether_pool.c`, `tests/runtime/test_resource_caps.c`, `tests/runtime/test_libaether_caps.c`, `tests/integration/emit_lib_deadline/`). Closes #343. Adds the runtime memory ceiling and per-thread wall-clock deadline that `--emit=lib` plugin hosts use to bound untrusted Aether guests. Public C surface in `include/libaether.h`: `aether_set_memory_cap(uint64_t bytes)` (process-wide cap; 0 disables), `aether_set_call_deadline(int64_t ms)` (per-thread monotonic deadline; 0 disables), `aether_deadline_tripped()` (codegen-emitted tripwire target). Memory accounting: an `_Atomic uint64_t` tracks current usage (NOT high-water-mark) — long-running guests that allocate-and-free in a loop don't trip the cap on cumulative churn. Cap-aware allocators (`aether_caps_malloc` / `_calloc` / `_realloc` / `_free`) are libc-compatible — no hidden header — so a libc free() on a returned pointer is heap-safe (cap accounting is lost in that case but heap integrity is preserved). 5 stdlib + runtime allocator files instrumented: `string_new_with_length` / `string_release` (struct + capacity); `list_new` / `list_free` / `list_add_raw` (struct + capacity * sizeof(void*)); `map_new` / `map_free` / `map_put_raw` / `map_remove` / `map_clear` / `hashmap_resize` (struct + buckets array + per-entry); `hashmap_create` / `hashmap_free` / `hashmap_resize` (struct + entries array); `arena_create` / `arena_destroy` (struct + memory block); `pool_create` / `pool_destroy` / `standard_pools_create` / `standard_pools_destroy` (struct + backing memory). Codegen emits `if (aether_caps_deadline_tripped()) { __aether_abort_call(); break; }` at every `for` / `while` loop head when `gen->emit_lib` is set — mirrors the existing `gen->preempt_loops` precedent at `codegen_stmt.c:1827`. The `break` exits the immediately-enclosing loop after marking the sticky tripped flag; outer loops re-check the flag at their next iteration and break too — N tripwire-bounded breaks unwind any depth of nesting. Zero overhead under `--emit=exe`: both per-loop emission and prelude extern decls are gated on `gen->emit_lib`. End-to-end test compiles a 1-billion-iteration loop fixture, links it with the runtime cap module + a C harness that arms a 1 ms deadline, and asserts the call returns within 1 second. `--emit=exe` zero-overhead is verified by `grep`-counting tripwire references in the emitted C (must be 0).

- **`@derive(eq)` synthesizer for structs (v1)** (`compiler/parser/parser.c`, `compiler/analysis/derive.{c,h}`, `compiler/aetherc.c`, `tests/integration/derive_eq/`). Issue #338 v1. The `@derive(eq)` annotation on a struct synthesizes a `int T_eq(T a, T b)` helper that returns a `&&` chain over per-field `==`. v1 supports primitive numeric (int / long / float / byte / bool) and string fields — the existing codegen lowers `string == string` to `strcmp(...) == 0` automatically, so the synthesizer doesn't need a special path. Empty structs return `1` (always equal). Pre-typecheck pass (`derive_synthesize_pass`) walks `program->children`, finds annotated structs, builds AST_FUNCTION_DEFINITION nodes, and inserts them as siblings — normal typecheck + codegen runs them through unchanged. Out of v1 scope (each emits a precise compile-time diagnostic so users hit a deliberate signal, not silent omission): `format` / `clone` / `hash` derives ("not yet supported in v1" pointing at the annotation; the eq pattern is the expansion seam for follow-up commits); nested struct fields (would need topological sort over inter-struct dependencies + recursion to `<F>_eq`); array / list / map / ptr fields. 3 positive struct cases (Point with two int fields, User with int + string + bool, Empty zero-field) plus 2 negative cases (`@derive(format)` surfaces v1 limitation; `@derive(eq)` on a `ptr`-field struct surfaces unsupported-type diagnostic).

## [0.130.0]

### Added

- **`std.http.server.vcr` HTTP-sourced tape loading (Step 16)** — `vcr.load_url(tape_url, port)` fetches Servirtium markdown tapes from HTTP servers, enabling centralized tape libraries. Extracted `parse_tape_text(label, contents)` helper so filesystem and HTTP paths share identical parsing logic. Both endpoints return the same server pointer shape and integrate seamlessly with the existing tape/replay flow.

### Fixed

- **`std.http.server.vcr` snapshot release mutability** — GitHub Actions `release.yml` now creates snapshot releases as `draft: true` instead of `prerelease: true`, keeping releases mutable for asset uploads on every merge. Fixes 404 on `curl https://github.com/aether-lang-org/aether/releases/download/snapshot/aether-source.tar.gz`.

### Testing

- **VCR transparent MITM recorder validation** — `test_vcr_transparent_recorder.ae` validates record-mode forwarding against a refused upstream, asserting that record-mode handler forwards requests, echoes real responses, and correctly populates diagnostic surfaces (`last_error`, `last_kind`, `last_index`).
- **VCR static-content cache semantics** — `test_vcr_static_content_cache.ae` extends Step 11 (static-content bypass) by validating cache-control headers (ETag, Last-Modified, Cache-Control) on static files. Current state: files are served but lack cache headers; test documents the gap and provides baseline for future conditional-request handling (304 Not Modified).

## [0.129.0]

### Added

- **`std.http.server.vcr` record-mode hardening** (`std/http/server/vcr/`, `tests/integration/test_vcr_*.ae`). Adds live forwarding record mode, all-method VCR coverage, record/playback diagnostics, Servirtium-style drift failure via `flush_and_fail_if_changed`, gzip normalize/restore for `Content-Encoding: gzip`, and updated VCR documentation/test coverage maps. Supporting HTTP fixes make client response bodies and server `response_set_body_n` binary-safe for gzip payloads.

## [0.128.0]

### Added

- **`std.http.proxy` — nginx-class reverse proxy** (`std/http/proxy/`, `Makefile`, `examples/stdlib/http-reverse-proxy.ae`, `examples/stdlib/http-reverse-proxy-pool.ae`, `tests/integration/http_reverse_proxy/`, `tests/integration/http_reverse_proxy_pool/`, `docs/http-reverse-proxy.md`). Closes #381. Adds the outbound side of the HTTP server: forward inbound requests to a pool of upstream HTTP servers via `std.http.client`, with five load-balancing algorithms (round-robin / least-conn / ip-hash / smooth weighted RR — the algorithm nginx uses / cookie-hash sticky), active health checks (one pthread per pool, configurable interval / threshold; lock-free hot-path read via atomics), in-memory LRU response cache with RFC 7234 cacheability gates and Vary-aware keying (entries store the upstream's `Vary` spec so lookups recompute the variant key against each entry's stored Vary headers — same bucket, O(1)/variant matching), per-upstream three-state circuit breaker (closed/open/half-open with CAS-based transitions), idempotent retry on 5xx + transport with exponential backoff + full jitter and per-attempt re-pick (matches nginx `proxy_next_upstream` semantics — POST/PATCH never retried), per-upstream token-bucket rate limit, active drain (skip new requests, in-flight finish), W3C Trace-Context propagation (passthrough or generated traceparent), Prometheus 0.0.4 metrics surface (per-upstream counters by status class + transport_errors + timeouts + retries + latency_ms_sum/count + gauges for inflight/healthy/breaker_state/draining; pool-level cache_hits/misses/revalidations/503_no_upstream), and Hop-by-Hop header handling per RFC 7230 §6.1 (strip on both directions; add X-Forwarded-{For, Proto, Host} + Via). Reuses the existing `std.http.client` for the round-trip (TLS, header lookup, timeouts, error classification, length-aware body via new `http_response_body_length`). Two install paths: `proxy.mount_simple(server, path_prefix, upstream_url, request_timeout_sec)` for single-upstream, and `proxy.mount(server, path_prefix, pool, opts)` for the production pool shape. Pool-level helpers: `upstream_pool_new`, `upstream_add` / `_remove` / `_drain` / `_undrain`, `health_checks_enable`, `breaker_configure`, `cache_new`, `pool_set_cookie_name`, `rate_limit_set`, `pool_metrics_text`. Per-mount `opts_*` for path-prefix stripping, Host: rewrite, X-Forwarded-* toggles, body cap, retry policy, trace inject. Refuses `Upgrade:` requests with 502 + `X-Aether-Proxy-Error: upgrade_unsupported` (WebSocket / h2 upstream is v2 follow-up; documented). Bodies buffered up to `opts.max_body_bytes` (default 8 MiB) with binary-safe handling (response body length tracked separately so embedded NULs don't truncate); streaming pass-through is the next major feature. Locking via `runtime/utils/aether_thread.h` pthread shim (Windows-MinGW compatible); per-upstream fields are atomics so the LB picker is lock-free in the hot path. The proxy plugs into the existing middleware chain via `http_server_use_middleware`, short-circuits with `return 0` after populating the response. **Tests**: 10-case base integration test (basic round-trip, hop-by-hop strip, X-Forwarded-* injection, Host rewrite, 1 KiB POST body round-trip via portable POSIX `wc -c` + `od -An -tx1` byte compare, Upgrade refusal, timeout → 504) plus 16-case pool integration test covering RR / weighted-RR / ip-hash / cookie-hash distribution, drain isolation, health-check kill + recovery, breaker open + half-open recovery, cache hit / TTL expiry / Vary, idempotent retry with re-pick, per-upstream rate limit (token bucket exhaustion → 503), Trace-Context passthrough, and Prometheus metrics endpoint surface. New module under `std/http/proxy/` (8 `.c` files split per concern: pool + lb + breaker + health + cache + opts + metrics + middleware glue).

### Changed

- **HTTP server documentation refreshed for current state** (`docs/http-server.md`, `docs/stdlib-reference.md`, `docs/stdlib-api.md`, `README.md`). Intro called HTTP/2 + WebSocket "follow-up PRs" but both shipped in v0.96.0+; rewritten to enumerate the current shipped surface (h2 + h2c via libnghttp2, ALPN, GOAWAY graceful shutdown, server-level pthread pool for per-stream concurrent dispatch, WebSocket RFC 6455, SSE, etc.). Trade-offs section called per-stream concurrent dispatch "not yet implemented" — corrected to describe the actual mechanism. Middleware compatibility table now lists `use_bearer_auth` and `use_session_auth`. Reverse-proxy section added with link to the new `docs/http-reverse-proxy.md` reference. README's "Production-grade HTTP server" bullet now mentions the reverse proxy alongside the other capabilities.

- **Person names removed from code, comments, and changelog** (8 files across docs, runtime, compiler, regression tests, conformance README). Per the project rule (no person names in code/comments/PR descriptions/commit messages); the only retained reference is the MIT-license attribution in `contrib/tinyweb/module.ae` line 3, which the upstream's MIT license requires preserved verbatim. Replacement style: describe behaviour or use case ("a real-world site-poller", "a downstream port", "an external porter") instead of attributing to a specific person.

- **Tree-level READMEs added** (`tests/integration/README.md`, `tests/syntax/README.md`, `tests/regression/README.md`, `examples/README.md`). Brief 20-50 line overviews explaining what each tree contains, where to add a new test or example, and the conventions to follow. Helps new contributors find the right home for a fixture without browsing 100+ subdirectories.

- **23 integration test files have a justification comment above their `extern http_server_start_raw` declaration**. The wrapper `http.server_start` exists but is awkward to use from inside an actor's `receive` handler (the wrapper's `string` return triggers an unused-result warning when discarded). The new comment makes the legitimate use of the raw form clear so future audits don't flag it as an anti-pattern.

### Fixed

- **`tests/integration/test_web_framework.ae` deleted**. The placeholder claimed "HTTP routing/middleware/WebSocket are not yet implemented" but every feature it listed has shipped, with full coverage in `tests/integration/http_server_*/`. The placeholder added confusion without unique test coverage.
- **`std.http.server.vcr` — base64 body encoding support and filesystem equivalence test** (`std/http/server/vcr/aether_vcr.c`, `tests/integration/svn_checkout_fs_equivalence/`, `tests/integration/svn_checkout_via_vcr/`). VCR tape playback now decodes base64-encoded response bodies marked with "base64 below" content-type hints, enabling byte-for-byte transmission of binary WebDAV responses from Servirtium markdown tapes. New `http_response_set_body_n()` C extern (length-aware body setter) supports binary-safe response bodies. Three helper functions in the C runtime: `ascii_ci_contains()` for case-insensitive header scanning, `b64_value()` and `b64_is_space()` for base64 alphabet and whitespace handling, and `decode_base64_body()` for the decoding pipeline. Public function `vcr_decode_base64_body_raw()` is exported for tests that need standalone decode. Integration test `svn_checkout_via_vcr` validates all 17 tape interactions (SVN WebDAV requests/responses with headers + binary-encoded bodies) byte-for-byte against the canonical Servirtium-Java tape from 2019. Integration test `svn_checkout_fs_equivalence` compares real-`svn` working trees from live svn.apache.org checkout against VCR-replay checkout, verifying that the tape playback produces identical files (excluding `.svn` metadata). Live test is gated behind `AETHER_RUN_LIVE_SVN_CHECKOUT_EQUIV=1` to avoid runtime dependency on network + SVN availability; both tests are POSIX-only (Windows gets a SKIP).

## [0.126.0]

### Fixed

- **aetherc codegen: closure-local tuple destructure of a captured outer-scope name** (`compiler/codegen/codegen_expr.c`, `compiler/codegen/codegen_stmt.c`, `tests/regression/test_closure_shadow_tuple_destructure.ae`). When a closure body destructured a tuple into names that shadowed outer-scope variables also captured by the closure (e.g. `out, status, _ = sh(cmd)` inside an `it()` callback whose outer `main()` had its own `out, status, err = sh(...)`), codegen emitted `name = _tup._N` against the captured-by-reference slot — a `T**` from the closure env. Result: a `[-Wincompatible-pointer-types]` C warning and a startup segfault before any user output as soon as the closure body next dereferenced the captured slot. Two fix sites: `is_assigned_to` (in `codegen_expr.c`) now treats `AST_TUPLE_DESTRUCTURE` targets as writes for promotion analysis, so the outer name gets heap-promoted; the `AST_TUPLE_DESTRUCTURE` emit (in `codegen_stmt.c`) now mirrors the `AST_VARIABLE_DECLARATION` Route-1 path — declares `T* x = malloc(...); *x = _tup._N` at the outer first-use and `*x = _tup._N` at the closure-body reassignment, with a defer-free queued at scope exit. Reported by the svn-aether porter as closure-shadow-tuple-destructure (Round 238/239) — the diagnostic was non-obvious (silent segfault, no "shadowing" hint) and the natural test-driver pattern (top-of-main shell prep + per-`it()` shell assertions, all using the same `out, status, err` triple) hit it immediately. Workaround that's no longer needed: rename the outer destructure's targets to `prep_out_`/`prep_st_`/`prep_err_`.

## [0.125.0]

### Added

- **`contrib.aeocha` emits a structured per-`it()` report through the parent IPC channel** (`contrib/aeocha/module.ae`, `contrib/aeocha/README.md`, `tests/integration/aeocha_aeb_ipc_reporting/`). When `run_summary()` runs under a parent that opened a back-channel via `os.run_pipe` / `os.run_pipe_drain_and_wait` (the shape aeb's `driver_test` uses), it additionally writes a `version=1` structured report through `std.ipc.parent_channel()` before exiting — header KV block (`version`, `total`, `passed`, `failed`, `errored`, `duration_ms`) + `---` separator + tab-packed per-`it()` rows (`STATUS\tindex\t"name"\tduration_ms[\tfailure_message]`). Gated on `parent_channel() >= 0`, so `ae run foo.ae` from a terminal sees no behavior change. Failure messages capped at 256 chars per row; if the full report would exceed ~60 KB (Linux's default 64 KB pipe buffer), the emitter progressively drops messages, then durations, then falls back to header-only — the parent always sees a valid v1 report. Per-`it()` telemetry is threaded through the existing framework map (`its` list, `current_it` slot, `t_start_ns_str` start time) so no module-level state is needed; pointer- and 64-bit-valued slots bypass `ref()` cells because the codegen `ref_get` casts to `(int)` and would truncate. POSIX-only in v1 — Windows returns `-1` from `parent_channel` and the report path is silently a no-op. New integration test `aeocha_aeb_ipc_reporting` compiles a child driver with one PASS + two FAIL `it()`s, spawns it via `run_pipe_drain_and_wait`, parses the report, and asserts the header KV, separator, and three rows. Drives the chokepoint aeb needs to surface failing-test diagnostics from a subordinate `ae`-spawned test program without scraping ANSI-coloured stdout.

## [0.124.0]

### Added

- **`std.ipc` — child-to-parent back-channel IPC** (`std/ipc/aether_ipc.c`, `std/ipc/module.ae`, `std/os/aether_os.c`, `std/os/module.ae`, `tests/integration/std_ipc_roundtrip/`, `tests/integration/std_ipc_bash_chain/`). New stdlib module providing the missing primitive aeb's `aether.driver_test` (and similar shapes elsewhere) had been working around with marker files at known FS paths: a child process writes a structured payload to the parent over an inherited pipe at fd 3 (the value goes into the env var `AETHER_IPC_FD` so the contract is the env var, not the literal 3). Three parent-side spawn variants in `std.os`: `os.run_pipe(prog, argv, env) -> (read_fd, child_pid, err)` for streaming/incremental consumers, `os.wait_pid(pid) -> (exit_code, err)` companion for reaping, and `os.run_pipe_drain_and_wait(prog, argv, env) -> (channel_payload, exit_code, err)` convenience that handles spawn + drain + wait + reap in one call without pipe-buffer deadlock risk. Three child-side primitives in `std.ipc`: `parent_channel() -> int` returning the fd or -1, `write(fd, bytes) -> string` and `write_close(fd, bytes) -> string` for the payload-then-close pattern. Common shape ends up at 3 lines per side (child: `ch = ipc.parent_channel(); ipc.write_close(ch, "passed=44\nfailed=3\n")`; parent: `data, exit_code, _ = os.run_pipe_drain_and_wait(child_bin, argv, null)`). POSIX-only in v1 — Windows returns "unsupported on Windows" from the parent-side spawns and -1 from `parent_channel`; Windows consumers fall back to file-marker patterns. Designed for in-process Aether ↔ Aether spawn shapes (cross-language is explicitly out of scope; cross-machine is its own ask). Three rounds of design discussion captured in `~/scm/aetherBuild/asks/lightweight-ipc-dx{,-aether-reply,-aeb-final}.md`. Two regression tests: `std_ipc_roundtrip` exercises all three parent-side spawn variants against a compiled child; `std_ipc_bash_chain` pins the contract that fd 3 + AETHER_IPC_FD survive a `bash -c '<wrapper>; <driver>'` intermediary (aeb's actual call shape — reported as nuance #1 in the design discussion).

## [0.123.0]

### Fixed

- **`make install-contrib` now ships `contrib.aeocha`** (`Makefile`). The `install-contrib` target was deleting `$(PREFIX)/share/aether/contrib/aeocha` post-copy alongside `aether_ui`, `tinyweb`, `host/{java,go,tinygo}` — but the rationale for that exclusion list ("modules whose build shape doesn't fit v1's .a-archive convention") doesn't apply to aeocha, which is pure Aether (`module.ae` only — no `.c` bridge, no `.a` archive). Result: porters who installed the toolchain via `make install && make install-contrib` couldn't `import contrib.aeocha` against the prefix because the module.ae wasn't there. Fix removes the spurious `rm -rf` line; aeocha now installs as `share/aether/contrib/aeocha/{module.ae,README.md,TODO.md}` (test/example noise still trimmed by the existing `find` rules). No `.a` lands in `lib/aether/` for aeocha — correct, because there's nothing to link against. Existing contrib modules unchanged.

- **aetherc codegen: hoist extern declarations before closure emission** (`compiler/codegen/codegen.c`, `tests/regression/test_closure_extern_ordering.ae`). Closure bodies that called imported externs (e.g. a `callback { argv = list_new() }` block where `list_new` is `extern`'d in `contrib.aeocha`) were emitted into the generated C *before* the imported-module extern prototypes — so the closure call-sites fell through C90's implicit-int rule and gcc/clang rejected the file with `error: conflicting types for 'list_new'; have 'void *()'` once the late prototype landed. The same trap applied to locally-declared externs that happened to be referenced from a closure body. Fix moves all extern emission (both locally declared `extern` siblings and externs reachable through `import M`) into a new pass that runs alongside the existing user-function forward-declaration pass, *before* `discover_closures` / `emit_closure_definitions`. The redundant emission inside the `AST_IMPORT_STATEMENT` and `AST_EXTERN_FUNCTION` handlers has been removed (they used to double-emit the same prototypes, harmless but noisy). Reported with full repro by the svn-aether porter ("Round 234"); the workaround porters were using (wrap each extern call in a local `argv_new() -> { return list_new() }` helper so the call-site sees an Aether forward decl) is no longer needed.

### Changed

- **`contrib.aeocha.expect_stdout_line_field` now tokenises on runs of whitespace (awk semantics) instead of single-space splits** (`contrib/aeocha/module.ae`, `contrib/aeocha/README.md`, `tests/integration/aeocha_expect_matchers/probe.ae`). The matcher's whole reason to exist is to absorb the bash idiom `awk '/^Revision:/{print $2}'`, but with single-space-only splitting a column-aligned line like `Revision:    3` produced `["Revision:", "", "", "", "3"]` — so callers had to count spaces and pick the right empty-string-laden index, or fall back to `expect_stdout_contains` on a fragile " 3\n" substring. New behaviour walks the line character-by-character collecting non-whitespace spans (space=32, tab=9), drops empty tokens, and indexes 0-based — `expect_stdout_line_field(out, "Revision:", 1, "3", ...)` now works against the porter's real `svn info` output regardless of column padding. Single-space-significant output (rare) loses the matcher; callers in that corner can fall back to `assert_str_eq` with a hand-derived value. Reported by svn-aether porter Round 234 follow-up.

### Added

- **`contrib.aeocha.expect_stdout_line_after`** (`contrib/aeocha/module.ae`). Companion to `expect_stdout_line_field` for the "value after a `Key:` line" idiom — common in svn/git-style key-value output where the value contains embedded spaces (e.g. `Log: first commit message`). Finds the first line starting with `prefix`, takes everything after the prefix, trims leading and trailing whitespace, and compares to `want`. Absorbs `sed -n 's/^Log: *//p'` style captures that single-token field-indexing can't handle.

- **`http.response_add_header(res, name, value)`** + **`http.response_clear_headers(res)`** (`std/net/aether_http_server.c`, `std/net/aether_http_server.h`, `std/http/module.ae`). Two new server-response helpers driven by VCR's record/replay needs. `add_header` always appends, even if a header with the same name already exists — necessary for SVN/WebDAV's repeated-key responses (13+ `DAV:` lines per response, each with a distinct value). The existing `set_header` keeps its replace-on-duplicate semantics for callers that want it (e.g. CORS middleware setting `Access-Control-Allow-Origin`). `clear_headers` drops every header on the response including the `Content-Type: text/html` and `Server: Aether/1.0` defaults that `http_response_create` injects, used by VCR's dispatcher to reset to a blank slate before emitting tape-recorded headers verbatim. Without these, replay couldn't faithfully serve the recorded wire bytes — both the duplicate-key collapse and the always-Aether `Server` header were visible defects when comparing replay output against the recording.

- **`vcr_get_resp_headers(i)`** (`std/http/server/vcr/aether_vcr.c`, `std/http/server/vcr/module.ae`). Read accessor mirroring the existing `vcr_get_req_headers` / `vcr_get_body` getters — exposes the response-headers block stored per tape interaction so test drivers can compare a replayed response against the recorded one. Setter (`vcr_aether_set_resp_headers`) shipped in PR #361; the getter completes the round-trip surface.

- **VCR replay validation against the canonical SVN tape — Aether-driver phase 1** (`tests/integration/svn_checkout_via_vcr/probe.ae`, `tests/integration/svn_checkout_via_vcr/test_svn_checkout_via_vcr.sh`). End-to-end test that loads `tests/integration/ExampleSubversionCheckoutRecording.md` (17 WebDAV interactions captured against svn.apache.org in 2019), starts an in-process VCR-backed HTTP server, drives all 17 requests through `std.http.client`, and asserts byte-for-byte equality on response status, body, and full response-headers including the 13+ duplicate `DAV:` lines per response. Modulo two wire-layer headers (`Content-Length` recomputed by `set_body`, `Connection` synthesized by HTTP/1.1 keep-alive state) which are documented as legitimate replay-time divergences, the entire SVN protocol round-trips through VCR. Sets up the framework for the future "transparent MITM forwarder" recorder (see `docs/next-steps.md`) — same Aether-side `std.http.client` driver shape, just with an upstream forward step in the middle.

- **VCR strict-match diagnostic surface + permutation tests** (`std/http/server/vcr/aether_vcr.c`, `std/http/server/vcr/module.ae`, `tests/integration/svn_vcr_strict_match/`). Tests need to navigate to root cause of a tape mismatch without parsing free-text 599 response bodies. New per-dispatch outcome slots — `vcr.last_kind() -> int` (one of `KIND_OK`, `KIND_PATH_OR_METHOD_DIFF`, `KIND_HEADER_MISSING`, `KIND_HEADER_VALUE_DIFF`, `KIND_HEADER_UNEXPECTED`, `KIND_TAPE_EXHAUSTED`, `KIND_BODY_DIFF`) and `vcr.last_index()` (the interaction the dispatcher was deciding) — sit alongside the existing `vcr.last_error()` string. `vcr.set_strict_headers(1)` opts a tape into request-header strict matching even when its recorded request blocks are blank, useful for negative-path tests. `vcr.reset_cursor()` resets dispatch position to interaction 0 without freeing the tape, letting tests run multiple independent cases against the same loaded tape. New permutation test exercises four kinds (control / missing / value-diff / unexpected) and asserts each produces the right `KIND_*` and a populated `last_error`. Models the Java Servirtium `ServiceMonitor`'s per-interaction notification pattern as a passive read surface — no callback registration, no allocation, no actor messaging (VCR is single-threaded by construction; tests drive serially and read after each request). The `normalize_live_headers` C helper now also drops `Connection` and `Content-Length` from the comparison input — wire-layer transport headers that aren't part of the recorded protocol.

### Changed

- **VCR's dispatcher now emits response headers verbatim from the tape, in recorded order, with no Aether defaults overlaid** (`std/http/server/vcr/aether_vcr.c`). Pre-fix, the dispatcher emitted `Content-Type: text/html` and `Server: Aether/1.0` (defaults from `http_response_create`) plus a single `Content-Type` from `e->content_type`, and the resp_headers walk used `set_header` (replace-on-duplicate). Result against an SVN-style tape: only the *last* of 13+ `DAV:` headers survived, the `Server:` header always said `Aether/1.0` instead of what the original server sent, and Content-Type ordering was forced to position 0 instead of wherever the recording had it. Fix: dispatcher now calls `http_response_clear_headers` immediately after setting the status, then walks the tape's resp_headers block calling `http_response_add_header` per line — no replace, no skip, no overlay. `e->content_type` is emitted only as a fallback for tapes whose resp_headers block doesn't include a Content-Type line (legacy / pre-PR-361 tapes). All 6 existing VCR regression tests still pass.

## [0.118.0]

### Added

- **`contrib.aeocha` — integration-shape `expect_*` matchers for process and HTTP assertions** (`contrib/aeocha/module.ae`, `contrib/aeocha/README.md`, `tests/integration/aeocha_expect_matchers/`). Ten matchers built on top of the existing `assert_*` primitives, designed to absorb bash-shaped "spawn / capture / awk-and-compare" idioms into single Aether calls when porting integration tests off shell scripts. Process surface (consumes the `(stdout, exit, err)` triple from `os.run_capture`): `expect_exit`, `expect_no_spawn_error`, `expect_stdout_contains`, `expect_stdout_line_field` (replaces `awk '/^Revision:/{print $2}'`-style assertions), `expect_stdout_line_count`, `expect_stdout_matches` (glob patterns via `std.string.glob_match`). HTTP surface (consumes a `resp: ptr` from `http.get` / `client.send_request`, works on both v1 and v2 responses): `expect_http_status`, `expect_http_no_error`, `expect_http_body_contains`, `expect_http_header`, `expect_http_body_json_field` (compact-JSON substring match for top-level string keys). Two matchers from the porter's spec are deferred: `expect_stderr_*` (blocked on `os.run_capture` learning to capture child stderr — it currently only captures stdout), and the porter's spec also asked for richer regex/JSON-path patterns that are out of scope until matching stdlib support exists. Companion fix in `std.http.server.vcr` below makes the HTTP matchers usable through VCR-replayed responses.

### Fixed

- **`std.http.server.vcr` now propagates the tape's `### Response headers recorded for playback:` block at replay time** (`std/http/server/vcr/aether_vcr.c`, `std/http/server/vcr/module.ae`). Previously the tape parser saw the response-headers block and threw it away — the `Interaction` struct had no field to store it, and the dispatcher emitted only Content-Type from the response-body header line. Tapes that declared headers like `X-Node-Hash: abc123` got compiled-but-silent-drop behaviour. Fix adds a `resp_headers` field to `Interaction`, a `vcr_aether_set_resp_headers(idx, headers)` setter the Aether parser calls per interaction, and a per-line walk in `vcr_dispatch` that emits each header via `http_response_set_header`. CRLF tolerance, Content-Type dedup (already set from `e->content_type`), 256/4096-byte name/value caps with drop-rather-than-truncate. All 6 existing VCR regression tests still pass; the new behaviour is additive (tapes without a response-headers block hit the legacy single-header path unchanged). Surfaced while writing the `expect_http_header` matcher in `contrib.aeocha`; the matcher is the first downstream consumer of replayed response headers.

### Changed

- **Documented `std.os` process-spawn surface — fills a tier-coverage gap** (`LLM.md`, `docs/stdlib-api.md`, `docs/next-steps.md`). The `os.run` / `os.run_capture` / `os_execv` family shipped in PR #148 + #289 but documentation was thin: `docs/stdlib-api.md` had only a "see PR #148" pointer, `docs/next-steps.md` still listed `os.run` as a P1 to-do, and `LLM.md` had no idiom-level entry — meaning a future LLM asked "how do I spawn a child binary in Aether?" would grep the stdlib instead of getting the answer immediately. Three updates: (1) `LLM.md` gains a one-line idiom under "Idioms that keep biting" calling out `os.run_capture(prog, argv, env) -> (stdout, exit_code, stderr)` as the canonical answer with no need to propose a `std.process` module; (2) `docs/stdlib-api.md` gets a new "Process spawn (argv-based, no shell)" subsection with full signatures, return shapes, a worked git-rev-parse example, capability/Windows portability notes, and a callout that the API is currently synchronous (no streaming stdin / incremental stdout); (3) `docs/next-steps.md` marks the P1 (`os.run`) and P3 (`argv0` + `execv`) sections as **SHIPPED** with banners pointing at the stdlib reference, retaining the original rationale as historical context. No code changes.

## [0.117.0]

### Fixed

- **`make release` now bakes the correct `AETHER_VERSION` into the release binary** (`Makefile`). The `release` target's hand-rolled `gcc` invocation was missing `-DAETHER_VERSION=\"$(VERSION)\"`, so `build/aetherc-release` (which `make install` ships as `$(PREFIX)/bin/aetherc`) reported the dev sentinel `v0.0.0-dev` regardless of the git tag. Surfaced after installing 0.116.0 — `aetherc --version` showed `v0.0.0-dev` while `ae version` correctly showed `0.116.0`. Fix: add the flag to the release target's gcc line, matching what the main `$(CFLAGS)` (line 133) already does for the dev build path. The dev `build/aetherc` was unaffected.

## [0.116.0]

### Added

- **`name: @aether string` per-param extern annotation** (`compiler/parser/parser.c`, `compiler/codegen/{codegen.h,codegen_internal.h,codegen.c,codegen_func.c,codegen_expr.c}`, `docs/c-interop.md`, `tests/integration/aether_extern_param_annotation/`). Marks an extern param as receiving an AetherString pointer (header preserved) rather than an unwrapped `const char*`. The annotation is per-param so a single extern can mix Aether-emitted-string params with naive-C-pointer params: `extern do_thing(aether_msg: @aether string, c_path: string) -> int` emits `do_thing(msg, aether_string_data(path))` at the call site. See `docs/c-interop.md` for when to reach for this.

### Fixed

- **Binary-content truncation across Aether-to-Aether extern boundaries** (#351). Under the v0.98.0 → v0.115.0 series, a `string` value carrying embedded NULs got strlen-truncated when it crossed an `extern` declaration whose receiver was Aether-emitted (e.g. another `--emit=lib` module's exports). Bisect-confirmed regression introduced by 718d13d (v0.97.0 → v0.98.0): that commit added a blanket `aether_string_data(s)` unwrap to fix #297 (naive C externs reading AetherString headers as data), but the unwrap conflated naive-C receivers with Aether-emitted ones. Aether-emitted receivers' `string.length` / `string.char_at` dispatch on the magic via `str_len`; stripping the header forced them into the `strlen()` fallback. Fix is opt-in via the new `@aether` per-param annotation above — bare `extern foo(s: string)` declarations preserve the v0.98.0 auto-unwrap so naive C externs (sqlite3, openssl, etc.) keep working. Acceptance: `binary-string-extern-boundary-repro.sh`-style program with `extern consume_binary(s: @aether string)` reports `length = 7` and `byte[5] = 14` from inside `consume_binary`.

## [0.115.0]

### Added

- **Test suite for `runtime/actors/aether_send_buffer.c`** (`tests/runtime/test_send_buffer.c` (new)). Coverage-data-driven test addition. `make ci-coverage` reported the file at 0% (0/34 lines): the public surface (`send_buffer_flush`, `send_buffered`, `send_buffer_force_flush`, `send_buffer_pending`) is referenced only from `runtime/examples/*.c` bench programs that the test suite never builds, and the codegen never emits the calls either — so regressions in batched-send would have shipped silently. Twelve tests covering: empty-buffer flush, NULL-target flush, single-message SPSC fast path, multi-message batch flush, accumulation up to capacity, target-change-triggers-flush, capacity-triggers-flush, force-flush partial drain, force-flush empty no-op, SPSC-overflow no-double-send invariant, mailbox-fully-accepts after SPSC reject, pending-count reporter. Coverage on this file moved **0% → 70.6%** (24/34). The remaining 10 lines are the cross-core slow path (lines 56-73, requires real `schedulers[]` Scheduler state — already covered by the separate `test_scheduler.c` suite). Suite-level: runtime (.c) bucket moved **75.7% → 78.2%**. **Coverage as a debugging tool:** while writing the SPSC-overflow test I had hypothesised a double-send bug in `send_buffer_flush` — that when SPSC partial-takes N of the batch and falls through, the mailbox would be sent buffer[0..count-1] and re-send messages 0..N-1. Wrote a unique-payloads-per-message test to expose it. Test *passed*. Re-reading `spsc_enqueue_batch` showed it's all-or-nothing (returns `count` on full success, `0` if `space < count`), never partial — so the bug I hypothesised cannot exist. Test now serves as a regression-locker against any future refactor that switches SPSC to partial-batch semantics. Confirmed: writing the test before the fix is what caught the wrong diagnosis, not the other way around.

- **`make ci-coverage-html` — one-command HTML coverage report via `gcovr`, with auto-bootstrap into a project-local venv if `gcovr` isn't on PATH** (`Makefile`, `tests/scripts/coverage_html.sh` (new)). After `make ci-coverage` populates `.gcda` data, this target runs `gcovr` over the merged stdlib + runtime + compiler + .ae-tests data set and writes `build/coverage/index.html` (browsable per-file detail) plus `build/coverage/coverage.json` (5.7 MB structured dump for CI / dashboard pickup). Three install paths in priority order: (1) `gcovr` already on PATH (auto-detected), (2) project-local venv at `build/cov-venv/` auto-created on first run via `python3 -m venv`, (3) clear diagnostic with apt/dnf/brew instructions if neither is available. The venv path keeps the install self-contained — no `pip install --break-system-packages`, no `sudo`, no system pollution. Honours `--merge-mode-functions=merge-use-line-min` since every per-test program defines its own `main` (gcovr's default strict mode rejects that as a merge conflict). Sample output: lines 14.3% (3374/23609), functions 27.4% (425/1550), branches 7.0% (1438/20487) — gcovr counts more aggressively than the bare-gcov bucket summary in `coverage_report.sh` (it includes header inlines, function entry/exit branches, etc.); both are valid views of the same data.

- **`ae build --coverage` — inject `gcc --coverage` into the user-program build, write `.gcda` files at runtime, produce `.ae.gcov` reports** (`tools/ae.c`). Pairs with `make ci-coverage` to extend coverage data from the C-level test runner (which only hits stdlib / runtime / compiler-as-library paths) to the 397 `.ae` regression tests (which exercise the parser+typechecker+codegen via every test compilation, plus `module.ae` files in the stdlib). Forces `-O0 -g` to keep gcov line attribution accurate — at `-O2` gcc inlines and merges blocks, scrambling the .gcov line numbers. Added `g_coverage` global, parsed from `--coverage` in `cmd_build`, plumbed through `build_gcc_cmd` via a new `opt_flags(optimize)` helper. **`make ci-coverage` now also runs the .ae regression suite** under `AE_BUILD_FLAGS=--coverage`, so `.ae.gcov` reports populate alongside the C-level data. New numbers on this branch: stdlib (.c) 31.0%, stdlib (.ae) 61.7%, runtime (.c) 75.7%, compiler (.c) 10.5%, tests (.ae) 81.1% — the stdlib-C 31% and compiler 10.5% are the actionable data points for "what's not tested."

- **`make ci-coverage` — line + branch coverage of stdlib / runtime / compiler, attributed to .ae source via gcov + Phase 1's `#line` directives** (`Makefile`, `tests/scripts/coverage_report.sh` (new), `tests/integration/ci_coverage_smoke/`). Builds a coverage-instrumented variant of `aetherc` + `libaether.a` under `build/cov-obj/` (mirrors the existing `build/asan-obj/` pattern), runs the C-level test_runner, then walks the `.gcda` files with `gcov -p -b -c` to produce per-source `.c.gcov` and (where the source originated as an `.ae` lowered through aetherc) `.ae.gcov` reports under `build/coverage/`. Summary printed by source-tree bucket: stdlib / runtime / compiler. Optional richer reports if `gcovr` (HTML) or `lcov + genhtml` (browsable HTML) are installed — probe-and-degrade. Sample output on this branch's first run: stdlib 31.0% (1193/3849 lines), runtime 37.0% (541/1461), compiler 4.0% (259/6446) — the compiler number is low because the C unit tests barely exercise the parser/typechecker/codegen; that's the data point you need to decide which compiler paths most need direct unit-test coverage. Companion `make ci-coverage-clean` clears `.gcda` between runs while keeping the `.gcno` instrumentation graphs for reuse. Not part of `make ci` — coverage rebuild is too heavy for every PR; run on demand. **Phase 2 of the coverage work** — Phase 1 was the `#line` directives that make this whole pipeline possible (no custom .ae↔.c sourcemap layer; gcov reads the directives natively).

- **`#line` source-map directives in generated C — gcc errors / gdb breakpoints / gcov reports point at `.ae` source** (`compiler/ast.{c,h}`, `compiler/aetherc.c`, `compiler/aether_module.c`, `compiler/codegen/{codegen.c,codegen.h,codegen_internal.h,codegen_stmt.c,codegen_func.c}`, `tests/integration/source_map_line_directives/`). Until now, `aetherc` lowered `.ae` → `.c` with no source-position metadata, so `gcc` warnings and segfaults inside the generated code reported against post-merge C-line space (which jumbles user code with stdlib glue and module-imported helpers in arbitrary order). The fix threads source-file paths through the AST: a new `source_file` field on `ASTNode` is populated by `ast_stamp_source_file()` after each parse — once for the top-level program, once per imported module — so cloned-by-`module_merge_into_program` nodes preserve their original file. Codegen's new `codegen_maybe_emit_line()` writes `#line N "src.ae"` directives (dedup'd: back-to-back nodes on the same line emit one directive) at every `generate_statement` and `generate_function_definition` entry. Path is escaped (`\\` for backslashes, `\"` for quotes) so paths with those chars round-trip through the C preprocessor. Phase 1 of the coverage-instrumentation work — Phase 2 will add `make ci-coverage` (gcov + lcov/gcovr) on top, since `gcov` reads `#line` directives natively and produces `.ae.gcov` reports automatically once this lands.

## [0.111.0]

### Added

- **`make contrib` + `make install-contrib` — per-machine prebuilt static libs for contrib modules** (`Makefile`, `tests/scripts/contrib_build.sh` (new), `contrib/sqlite/README.md`). Until now, `make install` shipped only the core toolchain (`build/aetherc`, `build/ae`, `libaether.a`, `runtime/`, `std/`); `contrib/` was source-only, leaving downstream projects to point `extra_sources` at `contrib/sqlite/aether_sqlite.c` inside a source checkout. The new pair fixes that. `make contrib` probes per-module system deps (sqlite3, python3, lua, perl, ruby, duktape, tcl) and compiles each available bridge to `build/contrib/libaether_<module>.a`; absent deps are skipped, never failed. `make install-contrib` then installs the built `.a`s to `$(PREFIX)/lib/aether/` plus each module's `module.ae` + headers to `$(PREFIX)/share/aether/contrib/<module>/`, trimming tests/benchmarks/example noise. Downstream `aether.toml` simplifies to `link_flags = "-laether_sqlite -lsqlite3"` — symmetric with `-laether` for the core. v1 covers `sqlite` + the six in-process host bridges (python/lua/perl/ruby/js/tcl); `host/{java,go,tinygo}`, `aether_ui`, `tinyweb`, `aeocha`, `climate_http_tests` deferred (different build shapes, different decisions still pending).

- **`http.request_body_length(req) -> int`** (`std/net/{aether_http_server.h,aether_http_server.c}`, `std/http/module.ae`, `std/net/module.ae`, `docs/stdlib-reference.md`, `tests/syntax/test_http_request_body_length.ae`). Sibling of `http.request_body` — exposes `HttpRequest::body_length` so callers can read NUL-bearing payloads (svn PUT, image uploads, gzip-compressed bodies) without `string.length(http.request_body(req))` strlen-truncating at the first embedded zero. The field already existed on the C struct (`size_t body_length`); this just plumbs it out via the standard accessor pattern (returns 0 if `req` is NULL or has no body, available from both `import std.http` and `import std.net`). Symmetric with the outbound `http.response_set_body_n` (#309 family). Section A.1 of `aether_changes_needed.md` from the svn-aether port — retires ~12 lines of hand-written C in `subversion/ae/svnserver/shim.c` (the three body-getter helpers).

- **`string.glob_match(pattern, s)` and `string.glob_match_pathname(pattern, s)`** (`std/string/{aether_string.h,aether_string.c,module.ae}`, `tests/regression/test_string_glob_match.ae`, `docs/stdlib-reference.md`). String-pattern matching, distinct from `fs.glob` (which enumerates matching files on disk). Implements the POSIX fnmatch(3) surface — `*` zero-or-more, `?` single, `[abc]` / `[a-z]` / `[!abc]` char classes, `\*` / `\?` literal escapes — with the `_pathname` variant treating `/` as a non-crossable boundary (the right shape for matching `src/*.c` against `src/foo.c` but not `src/sub/foo.c`). POSIX delegates to libc fnmatch; Windows gets a pure-C implementation behind the same surface. Returns 1 on match, 0 on no-match, -1 on glob-syntax error. Used by the svn-aether port's `ae/wc/ignore.ae` for svn:ignore matching and `ae/fs_fs/spec.ae` for branch-spec matching — retires 58 lines of `fnmatch` shim across two files. Section A.2 of `aether_changes_needed.md`.

- **`io.stderr_write(data, length) -> int` and `io.stdout_write(data, length) -> int`** (`std/io/{aether_io.h,aether_io.c,module.ae}`, `tests/regression/test_io_unbuffered_writes.ae`). Unbuffered writes to fd 2 / fd 1 — bypass stdio buffering so output is on the wire before the process exits / crashes / aborts. `println` and `io.print` are line-buffered on tty and block-buffered when stdout is piped; the last few lines reliably get lost when a process is killed mid-buffer. These functions take an explicit `length` (binary-safe; data may contain NULs), loop on partial writes, and retry on EINTR. POSIX uses `write(2)`; Windows uses `_write`. Reach for these in crash-trace paths or when interleaving binary output with text. **Minimal scope of A.3** — the larger fd surface (`fd_open` / `fd_read` / `fd_read_line` / dump-load helpers) the porter sketched is dump/load-specific and deferred pending a design call.

- **Per-process config doc + worked example** (`docs/per-process-config.md` (new), `examples/actors/per-process-config.ae` (new)). Aether deliberately rejects mutable assignment to module-level identifiers, so the C `static char *g_config; void set_config(s); const char *get_config(void);` shape doesn't transcribe directly. The canonical replacement is the **actor-as-singleton** pattern: spawn one ConfigActor at startup, drive writes via `Set*` messages, drive reads via a `Get*` message that carries an `actor_ref` to send the reply through. Doc walks through the pattern, three variants (write-once read-many, hot-path read with worker-actor caching, multi-tenant per-request state), trade-offs versus a hypothetical module-level `var`, and a worked replacement of a real C global-state shim from the svn-aether port. Pure-doc fix per Section A.4 (Option 2) of `aether_changes_needed.md`. Note for porters: `reply` is a reserved keyword in Aether; the example uses `to` for the reply-target field name to avoid the lex error.

- **Full fd-level I/O surface in `std.io`: `fd_open_read` / `fd_open_write` / `fd_close` / `fd_write_n` / `fd_read_n` / `fd_read_line`** (`std/io/{aether_io.h,aether_io.c,module.ae}`, `tests/regression/test_io_fd_surface.ae`). Extends the 0.110 unbuffered-write minimum (`io.stderr_write` / `io.stdout_write`, fd 1/2 only) to arbitrary fds. `fd_open_read` / `fd_open_write` return `(fd, err)` tuples; the write form is implicit `O_CREAT | O_TRUNC` mode 0644 on POSIX (`_O_BINARY` on Windows). `fd_write_n` is a looped write returning 0/-1; `fd_read_n` is a looped read returning `(bytes, count, err)` where `bytes` is a refcounted AetherString carrying the explicit byte count (binary-safe — embedded NULs survive). `fd_read_line` strips trailing `\n` (and a `\r` before it for CRLF input), returning `("", "")` on clean EOF. POSIX uses `open` / `read` / `write` / `close`; Windows uses `_open` / `_read` / `_write` / `_close` with `_O_BINARY`. Section 1 of `nuther-ask-of-aether-team.md` from the svn-aether port — retires 106 lines of fd shim code across `subversion/ae/subr/io/shim.c` and `subversion/ae/svnadmin/shim.c` (the looped `write_all` / `read_n` / `read_line` family). Note: `fd_write_n` is NUL-truncation-prone for source data passed as `string` — the auto-unwrap at the extern boundary strips the AetherString header, after which the C side sees a plain `const char*`. Length-aware writes from binary buffers should marshal through `std.bytes.finish` to get a length-bearing AetherString, or use `fd_write_n` with content already known to be strlen-safe.

- **`std.config` — persistent_term-style process-global KV store** (`std/config/{aether_config.h,aether_config.c,module.ae}` (new module), `tests/regression/test_std_config.ae`, `Makefile`). Models BEAM's `persistent_term`: `config.put(key, value)` during program initialisation, `config.get(key)` / `config.get_or(key, default)` / `config.has(key)` from anywhere for the rest of the process's lifetime. The canonical use case is CLI-flag values that get parsed once in `main()` and need to be reachable from request handlers entered later via C-callback dispatch (where Aether-typed parameters can't flow through the C `void* user_data` slot). Storage is a single process-global hashmap protected by a reader/writer lock — reads (the hot path) are concurrent with each other; writes serialize. Per Nico's design call: `std.config` is the answer for "set during init, read everywhere" immutable data, distinct from the actor-singleton pattern (which keeps the round-trip-per-read overhead and requires plumbing an `actor_ref`). Section 2 of `nuther-ask-of-aether-team.md`.

- **`std.actors` — process-global name → `actor_ref` registry** (`std/actors/{aether_actor_registry.h,aether_actor_registry.c,module.ae}` (new module), `tests/regression/test_std_actor_registry.ae`, `Makefile`). Models BEAM's `erlang:register(Name, Pid)` / `erlang:whereis(Name)`: bind an actor to a stable name at startup, look it up by name from any context. `actors.register("name", ref)` / `actors.whereis("name") -> actor_ref` / `actors.unregister(...)` / `actors.is_registered(...)`. At the C level `actor_ref` lowers to `void*`, which is exactly what the registry stores; no coercion needed. Combined with `std.config`, the two cover the porter's full per-process-state need: `std.config` for string-typed CLI flags, `std.actors` for finding long-lived actors from inside `@c_callback` handlers. **Module name is plural intentionally**: `actor` (singular) is a reserved keyword and can't appear as a namespace prefix (`actor.register(...)` would not parse). Section 2 of `nuther-ask-of-aether-team.md` / Nico's reply.

- **`const X = some_function_call()` is now a compile-time error** (`compiler/analysis/typechecker.c`, `tests/integration/const_call_reject/`). Aether's `const` is substitution-at-each-use: the compiler inlines the RHS expression at every reference. That works for `const PI = 3.14` (each use inlines a literal) but is silently wrong for `const G = make_thing()` — every reference re-calls the function, allocating fresh state and producing surprising-but-runs results. Per Nico's design call, reject these at typecheck time with a diagnostic that points the user at `std.config` / `std.actors` for process-global state, or to a regular assignment in `main()`. Allowed const RHS forms: literals (int / float / bool / string / null), other consts, unary / binary expressions over those, and string interpolation where every interpolated value is itself const. Function calls, member access, struct literals, and array literals are now rejected. Closes the silent-wrong-results trap raised in `nuther-ask-of-aether-team.md`.

## [0.109.0]

### Added

- **`std.bytes` read-back: `bytes.get(b, idx) -> int` plus `bytes.{set,get}_le16` / `bytes.{set,get}_le32` packed-int helpers** (`std/bytes/{aether_bytes.h,aether_bytes.c,module.ae}`, `tests/regression/test_std_bytes_get.ae`). `std.bytes` was write-only until `bytes.finish()` — `set` / `copy_from_string` / `copy_within` accumulated bytes and `length` reported the count, but there was no per-index read-back. The svn-aether port's svndiff encoder (227 lines, the largest hand-written shim left) needs exactly that pattern: build a packed array of 3-int ops via `add_op`, then walk it at encode time. `bytes.get(b, idx)` returns the byte as an unsigned 0..255 value, or -1 on OOB / NULL / negative index — symmetrical bounds-check shape with `bytes.set`. The `_le16`/`_le32` family stores / reads consecutive bytes in little-endian lane order; `set_*` grows the buffer to cover the write, `get_*` returns -1 if any lane in the range is past the buffer's logical length. Round-trips losslessly with their setter siblings; the encoder body lowers to 3 + 3 lane reads per op instead of 12 + 12 hand-rolled byte shuffles. Section A of `fresh-aether-requests.md`.

- **`*StructName` accepted in function return-type position** (`compiler/parser/parser.c`, `tests/regression/test_ptr_as_struct_return.ae`). The 0.102 `*StructName` rollout claimed first-class acceptance "in any type position: variable annotations, function params, return types, struct fields, extern decls" — but the function-decl parser's typed-return-disambiguation switch did not list `TOKEN_MULTIPLY`, so `name(...) -> *Foo { … }` was treated as an arrow-body expression (`-> *Foo` parsed as multiplication) and reported a misleading top-level-syntax error at the `*`. Adding `TOKEN_MULTIPLY` to the disambiguator (matching the parameter-position parser, which routes through `parse_pattern` → `parse_type` and already handled `*Foo` correctly) closes the gap. Unblocks the four "container holder" shim retirements in the svn-aether port (txn / rtree / fs_fs / ra commit-builder, ~305 lines of C across the four) where the natural shape is a `make_txn() -> *Txn` constructor that initialises field handles and returns the struct view. Section B of `fresh-aether-requests.md`.

- **Self-referential structs via `*StructName` self-pointer fields** (`compiler/analysis/typechecker.c`, `tests/regression/test_self_ref_struct.ae`). `struct ErrChain { code: int, msg: string, cause: *ErrChain, file: string, line: int }` parsed and ran for int-only fields, but mixing in `string` fields tripped two latent typechecker gaps. (1) The `AST_MEMBER_ACCESS` typecheck branch handled `TYPE_STRUCT` (value structs) but not `TYPE_PTR`-to-struct, so member-access through a `*Self` chain didn't get the field's declared type onto the AST node. (2) `typecheck_statement` had no `case AST_RETURN_STATEMENT` — return values fell into the catch-all default, which recursed via `typecheck_node` → `typecheck_statement` for each child without ever calling `typecheck_expression`; nested expressions inside a return kept TYPE_UNKNOWN regardless of (1). Combined effect: `return "${e.msg}"` printed `e.msg` as `%d` (the int default in the interp's TypeKind switch) — string fields rendered as garbage integers. Both branches now closed; the test exercises a 3-deep error chain matching the svnae_error shim shape, walked recursively, with both int reads and string interpolation through the `*Self` chain. Retires the 73-line `ae/subr/error/shim.c` in the svn-aether port and unlocks user-defined recursive-struct shapes generally (n-ary trees, dependency graphs). Section C of `fresh-aether-requests.md`.

## [0.108.0]

### Added

- **`byte` primitive type — unsigned 8-bit, lowers to `unsigned char`** (`compiler/ast.{h,c}`, `compiler/parser/{lexer.c,tokens.h,parser.c}`, `compiler/analysis/typechecker.c`, `compiler/codegen/codegen.c`, `docs/language-reference.md`, `docs/c-interop.md`, `tests/regression/test_byte_primitive.ae`, `tests/integration/byte_literal_range_reject/`). Type-precision for struct fields, function parameters, returns, and locals where the value is genuinely an octet — packed-tag bytes, opcode discriminators, NaN-boxing pointer tags, network protocol headers, on-disk file format fields. For *bulk* byte storage, `std.bytes` remains the answer; `byte` is the single-octet primitive that complements it. Motivated by the microQuickJS port's JSValue/OpCode struct fields where forcing `int` would either bloat memory or force a parallel intrinsic API. Unsigned because that's what every modern language calls "byte" (Go, Rust, C#) and because the natural meaning of "the octet at this address" is unsigned. C-side mapping is `unsigned char` (not `uint8_t`) for C's strict-aliasing exemption — `unsigned char *` can legally alias any type's bytes for read/write; `uint8_t *` cannot. Arithmetic: `byte op byte → byte`, mixed `byte op int → int` (the wider type wins). Bitwise on byte operands stays byte (so `flags & 0x80` keeps `byte` typing). `byte → int / int64 / float` is implicit safe widening. `int → byte` requires an explicit declaration / assignment — out-of-range integer literals (256, 0x100, 999, etc.) are rejected at compile time with a "byte literal out of range: N does not fit in 0..255" diagnostic; non-literal int → byte compiles and truncates at runtime (matching how other narrowings behave). Pointer arithmetic and `ptr[i]` byte indexing are deliberately NOT included — those are a separate feature with their own design surface and would need their own PR.

## [0.106.0]

### Fixed

- **Module-imported externs now trigger the #297 auto-unwrap at call sites** (`compiler/codegen/codegen_func.c`, `tests/integration/module_extern_auto_unwrap/`). The #297 auto-unwrap (`aether_string_data(arg)` injected at C extern call sites for `string`-typed parameters) only fired when the extern was declared inline in the same `.ae` file as the call. When the same extern was reached through `import std.X`, `is_extern_func` returned false (the registry stored the bare extern name like `http_response_set_body_n`; the call site searched for the dotted form `http.response_set_body_n`) and the auto-unwrap branch was skipped. The downstream svn-aether port worked around this by duplicating stdlib extern decls inline (Rounds 162/164 of the port). Fix: `find_extern_registry_index` (refactored out of `is_extern_func`) now tries both the exact form and the dot-normalised form. `is_extern_func`, `lookup_extern_param_kind`, and `lookup_extern_c_name` all route through the new helper. Module-qualified call sites match the underscore-named registry entry. The flagged caveat in v0.103.0's CHANGELOG (#309) for `http.response_set_body_n` is now resolved — port code that was using inline-extern workarounds can drop them.

## [0.104.0]

### Changed

- **VS Code theme — calmer palette, fewer colour collisions** (`editor/vscode/themes/aether.json`, `editor/vscode/package.json`). The actual root cause of "string colour is shiny yellow no matter which theme I pick" was a `configurationDefaults` block in `package.json` that force-overrode `string.quoted.double.aether` to `#FFCC00` via `editor.tokenColorCustomizations` — ignoring the user's active theme entirely. Removed that block; users' themes now decide. Also registered the bundled `themes/aether.json` as a proper `themes` contribution (it wasn't — the file shipped but VS Code couldn't pick it up from the theme menu) so the redesigned palette is reachable as a "Aether" entry under preferences > color theme. The previous palette used `#FFCC00` for strings (eye-grabbingly saturated yellow) and reused `#e06c75` for both control keywords and function names so the two collided visually. New palette is shaped after VS Code Dark+ / One Dark Pro: strings move to `#ce9178` (warm tan) with `#d7ba7d` for escape sequences, control keywords go to `#c586c0` (purple), storage / declaration keywords (`actor`, `struct`, `extern`, `import`, `as`, …) go to `#569cd6` bold (blue), function definitions and call sites to `#dcdcaa` (yellow-cream — VS Code Dark+ standard), `main()` stays `#dcdcaa` but italic-bold to flag the entry point, types (`int`, `string`, `*StructName`, custom struct names) to `#4ec9b0` (teal), variables to `#9cdcfe` (light blue), numbers to `#b5cea8` (calm green), booleans + `null` to `#569cd6` (blue), operators stay default fg. Every token class now reads as a distinct hue under prolonged inspection; no two unrelated scopes share a colour. Cons-cell pattern punctuation (`[h | t]`) gets the keyword purple so the shape pops without an explicit highlight rule.

- **TextMate grammar — accurate scoping for the full language** (`editor/vscode/aether.tmLanguage.json`). The previous grammar tagged every `name(` as `keyword.control` (so ordinary call sites rendered red like `if` / `while`), missed half the storage-class keywords, didn't recognise `*StructName` typed pointers, didn't highlight cons-cell `[h | t]` shapes, and never split bare-snake call sites from definitions. New grammar separates control-flow keywords (`if`, `else`, `for`, `while`, `match`, `return`, `break`, `continue`, `receive`, `send`, `spawn`, `defer`, `panic`, `try`, `catch`, `after`) from storage / declaration keywords (`actor`, `struct`, `message`, `state`, `extern`, `func`, `let`, `var`, `const`, `builder`, `callback`, `import`, `export`, `as`, `hide`, `seal`, `except`); adds primitive-type scope (`int`, `int64`, `long`, `float`, `double`, `bool`, `string`, `void`, `ptr`, `byte`, `actor_ref`); recognises `*StructName` typed pointers (PR #307) so the leading `*` and the struct name render as a single type token; tags actor / struct / message definitions with proper entity-name scopes; distinguishes `namespace.method(...)` qualified calls from bare ones; adds dedicated cons-cell pattern scope `meta.pattern.cons.aether` capturing `[h | t]` (and the multi-element `[a, b | rest]` shape); adds string-interpolation `${...}` as a nested patterns block so identifiers inside interpolations are highlighted; adds hex / binary / octal / float number literal patterns with underscore-separator support; splits operators by category (`assignment`, `comparison`, `logical`, `arithmetic`, `bitwise`, `actor` for `!` / `?`).

### Added

- **LSP completion catch-up + follow-up roadmap** (`lsp/aether_lsp.c`, `lsp/README.md` (new), `Makefile`). The hard-coded completion list in `lsp_handle_completion` was missing nearly every stdlib addition since the LSP shipped — no `string.seq_*` / `string.split_to_seq` / `string.array_*`, no `string_list_*`, no `intarr.*`, no `bytes.*`, no `fs.*` / `json.*` / `http.*` / `os.*` / `cryptography.*` / `math.*` / `log.*`. Refreshed with the full 2026-04-29 stdlib surface (~150 entries) plus all language keywords (control flow + storage / declaration), primitive types (`int`, `byte`, `*StructName`, `actor_ref`, …), the `StringSeq` struct, `match` / `defer` / `extern` / `import` / `export` / `as` / `panic` / `try` / `catch` / `after` / `builder` / `callback` / `state` / `message`, builtins (`print`, `println`, `sleep`, `clock_ns`, `wait_for_idle`, `exit`, `len`), the cons-cell sequence family in full. New `lsp/README.md` documents what's still unimplemented (symbol-aware completion, hover with type info, go-to-definition, document-symbol enumeration, typechecker-level diagnostics, references / rename) as the spec for a future LSP overhaul PR — each item points at the exact AST / typechecker hooks the implementation should reach into. Also fixes the `lsp:` Makefile target: it was missing `$(COLLECTIONS_SRC)` from its link line, so any new collections runtime symbol would break the LSP build (which the cons-cell `string_seq` runtime did until this fix).

- **`*StringSeq` closure-free combinators — `reverse` / `concat` / `take` / `drop`** (`std/collections/aether_stringseq.{c,h}`, `std/string/module.ae`, `lsp/aether_lsp.c`, `docs/sequences.md`, `docs/stdlib-reference.md`, `tests/regression/test_string_seq_combinators.ae` (new, 6 cases)). Builds on the cons-cell type below with the four structural ops that don't need a callback parameter — no Aether-function-from-C invocation, just pointer walks over the cons cells with refcount-correct handling of shared tails. `string.seq_reverse(s)` returns a fresh independent spine in O(n) work / O(1) auxiliary stack. `string.seq_concat(a, b)` returns a new seq with `a`'s elements copied (one new cell per element) and `b` shared via a refcount bump — O(|a|) work, `b` is never walked, freeing the result drops one ref from `b` but other handles to `b` stay valid. `string.seq_take(s, n)` returns the first `min(n, length)` elements as an independent spine; negative `n` yields empty; `n` exceeding length clamps. `string.seq_drop(s, n)` returns the n-th tail of `s` retained (caller owns one ref); negative `n` returns `s` retained; `n` exceeding length yields empty. The two equational laws the test suite locks: `reverse(reverse(s)) == s` and `take(s, n) ++ drop(s, n) == s` for any `0 <= n <= length(s)`. Closure-bearing siblings (`map` / `filter` / `foldl`) deferred to a separate change set — needs design for the Aether-callback-from-C bridge, which `@c_callback` partially handles for HTTP route handlers but hasn't been generalised for stdlib combinators yet.

- **`*StringSeq` — Erlang/Elixir-shaped cons-cell linked list of strings** (`std/collections/aether_stringseq.{c,h}`, `std/string/{aether_string.{c,h},module.ae}`, `compiler/codegen/codegen.c`, `compiler/codegen/codegen_expr.c`, `compiler/codegen/codegen_func.c`, `compiler/codegen/codegen_stmt.c`, `compiler/analysis/typechecker.c`, `Makefile`, `docs/sequences.md` (new), `docs/stdlib-reference.md`, `docs/language-reference.md`, `LLM.md`, `examples/basics/string-seq-walk.ae` (new), `examples/actors/message-with-string-seq.ae` (new), `tests/regression/test_string_seq{.ae,_literal.ae,_pattern_match.ae}` (3 new), `tests/integration/string_seq_in_message/`, `tests/integration/string_seq_shared_tail/`). Closes the gap raised by the svn-aether port: `string.split()` returns the legacy `AetherStringArray*` shape, leaving message fields on runtime split results to be declared as `ptr` instead of a typed sequence. Runtime arrays in messages now have a first-class shape that beats slices on every count: O(1) `cons`/`head`/`tail`/`length` (length cached per cell), refcount-based **structural sharing** (cons(x, t) and cons(y, t) share `t` without copying — and cycles can't form because cells are immutable after creation), iterative free for deep lists, and pattern-match `[]` / `[h|t]` arms that compose recursively (head typed `string`, tail typed `*StringSeq`). The runtime is 24 bytes per cell on 64-bit; `string.seq_free` walks the spine iteratively and stops at the first cell whose refcount remains > 0 after decrement — the other owner finishes the walk later. Surface lives under the existing `string` namespace alongside `string.array_*`, mirroring the precedent that data shapes closely associated with strings expose ops there: `string.seq_empty`, `string.seq_cons`, `string.seq_head`, `string.seq_tail`, `string.seq_is_empty`, `string.seq_length`, `string.seq_retain`, `string.seq_free`, `string.seq_from_array`, `string.seq_to_array`, plus `string.split_to_seq` as the runtime producer (sibling of `string.split`). Codegen integration: emits `#include "aether_stringseq.h"` near the prologue so `*StringSeq` resolves uniformly; lowers `[a, b, c]` to a right-fold `string.seq_cons` chain when the target type is `*StringSeq` (variable declarations and message-field initializers — same source syntax, two C lowerings driven by the target type); generalises the existing `[]` / `[h|t]` match-arm codegen to dispatch on the matched expression's type — `*StringSeq` matched expression gets a NULL-checking pointer walk (`s == NULL` for `[]`, `s != NULL` for `[h|t]`, with `s->head`/`s->tail` field reads), int-array matched expression keeps the historical `_match_arr` + `_match_len` slice lowering. Decision rationale: rejected sized slices (`{T*, len}`) — Erlang/Elixir-shaped cons cells fit Aether's actor-message ergonomics and the `[h|t]` pattern syntax that's already in the parser; per-cell length caching gives O(1) `length` while preserving structural sharing (Erlang BEAM doesn't do this because of its compact cell layout — Aether controls the layout, so we do). Verified end-to-end: 5000-cell deep list frees iteratively; two cons lists sharing a 2-cell tail free correctly when one owner releases; actor message round-trip with `sites: *StringSeq` payload + recursive `match` walk; literal `[a, b, c]` disambiguates correctly between `string[]` (existing static-array codegen, untouched) and `*StringSeq` (new cons-chain codegen) targets in the same source file.

## [0.103.0]

### Added

- **`os.getpid() -> int`** (`std/os/{aether_os.h,aether_os.c,module.ae}`, `docs/stdlib-reference.md`). POSIX `getpid(2)` / Windows `_getpid()`. Universally needed for tmpfile names (`/tmp/myprog.${os.getpid()}.tmp`), per-process locks, log prefixes, and stable tagging across forked children. Returns 0 on platforms compiled without filesystem support.

- **`cryptography.base64_encode_padded(data, length) -> (string, string)`** (`std/cryptography/{aether_cryptography.h,aether_cryptography.c,module.ae}`, `docs/stdlib-reference.md`, `docs/stdlib-api.md`). Sibling of the existing unpadded `base64_encode` — same RFC 4648 §4 standard alphabet, but with `=` padding to a multiple of 4 bytes. Reach for this when the wire format on the other end expects padded base64 (most auth headers, some JSON-encoded blob formats). Avoids 12-line C-shim "append `=` to the unpadded output" workarounds in downstream ports.

- **`sqlite.next_row(stmt, db) -> int`** (`contrib/sqlite/module.ae`, `contrib/sqlite/README.md`). Cursor-iteration sugar over `step`. Returns `1` on row available, `0` on `SQLITE_DONE`, `-1` on error. Replaces the doubled-`step()` shape that's the most common bug in cursor APIs (forget to step at the end → infinite loop; forget to step at the start → skip row 0). `step` / `errmsg` / explicit rc compare remain available for callers that want to distinguish DONE from ROW with their own control flow.

- **`http.response_set_body_n(res, body, length)`** (`std/net/{aether_http_server.h,aether_http_server.c,module.ae}`, `std/http/module.ae`, `docs/stdlib-reference.md`). Length-aware sibling of `http.response_set_body`. The plain setter uses `strdup` + `strlen` and silently truncates response bodies at the first embedded NUL — a landmine for any Aether HTTP server returning binary content (gzip-compressed payloads, image bytes, NUL-bearing files). The `_n` form treats `body` as `length` bytes verbatim, no NUL searching. Existing `set_body` callers untouched. **Caveat**: there's a pre-existing codegen bug where module-imported externs don't trigger the #297 auto-unwrap at call sites (works fine for inline-declared externs in the same `.ae` file). Until that's fixed in a follow-up, callers reaching this through `import std.http` need to manually unwrap via a small inline extern decl, or pass an explicit `aether_string_data(body)` cast. The underlying C function is correct end-to-end and binary-safe.

- **`string.substring_n(str, str_len_bytes, start, end) -> string` and `string.length_n(str, known_length) -> int`** (`std/string/{aether_string.h,aether_string.c,module.ae}`, `docs/stdlib-reference.md`, `docs/c-interop.md`). Length-aware companions to `string.substring` / `string.length` for use inside Aether libraries that take `string`-typed parameters. After #297's auto-unwrap fires at the function boundary, the helper sees only payload bytes — internal calls to `string.length(s)` fall through to `strlen` and truncate binary content at the first embedded NUL. The `_n` family threads the caller's length through and bypasses the internal `str_len` dispatch entirely. `length_n` is the identity helper that documents intent: `n = string.length_n(s, n)` reads as "yes I know my length" instead of looking like a forgotten `string.length(s)` that would have truncated. Plus a docs callout in `c-interop.md` § Length-clamp hazard explaining the symmetric Aether-side form of the post-#297 hazard previously documented only for C shims.

## [0.102.0]

### Added

- **`expr as *StructName` pointer-overlay struct cast + first-class `*StructName` type** (`compiler/ast.h`, `compiler/ast.c`, `compiler/parser/parser.c`, `compiler/analysis/typechecker.c`, `compiler/codegen/codegen.c`, `compiler/codegen/codegen_expr.c`, `docs/c-interop.md`, `docs/language-reference.md`, `tests/regression/test_ptr_as_struct_cast.ae`). Aether-side syntactic sugar for the C pattern `((StructT*)raw_ptr)`, letting systems-programming code overlay a struct header on a raw `ptr` without a parallel intrinsic surface (`ptr_set_int`, `ptr_set_ptr`, …). Motivated by ports of pointer-tagging / linked-list / arena-allocator C code (microQuickJS et al.) where the storage is C-allocated and Aether wants to manipulate fields. The pointer-ness is part of the spelled type — `*StructName` is a first-class type accepted in any type position (variable annotations, function parameters, return types, struct fields, extern declarations). Lowers to `StructName*` in C; member access on a `*StructName` value emits `view->field` rather than `view.field`. The cast `expr as *StructName` produces a `*StructName` value from a `ptr` (or coercible) operand; RHS struct must be in scope. The cast is a view — no allocation, no refcount, no auto-free; the operand's lifetime is the caller's problem (same contract as raw `extern` interaction). The `as` keyword is the same token used for `import x as y` aliasing; the two parses don't collide because import-aliasing is only recognised inside `import` statements. See `docs/c-interop.md` § Struct overlay on raw pointers.

## [0.101.0]

### Fixed

- **Bare `{ ... }` block after a function call is no longer eaten as a trailing closure** (`compiler/parser/parser.c`, `docs/closures-and-builder-dsl.md`, `LLM.md`, `tests/regression/test_bare_brace_after_call.ae`, `tests/integration/bare_brace_block_scope/`). Issue #286. Pre-fix the parser greedily consumed any `{ ... }` immediately after a function call as a trailing closure, regardless of whether `{` was on the same source line as the call's closing `)`. A user writing `base_body = read_blob(repo, sha)` followed on the next line by a bare `{ ... }` block found that the inner block became `read_blob`'s synthesised closure body — and references to `base_body` from inside surfaced as misleading `Undefined variable` errors. The reporter (and a downstream user) initially suspected a typechecker scope-rule regression; the actual cause was lexical and lived in `parse_postfix_expression`. Fix: trailing-closure consumption now requires `{` on the same source line as `)`. A `{` on a later line is left for the statement parser to handle as an independent bare-brace block, which sees outer locals via the ordinary lexical-scope chain. The compiler emits a warning when a `{` on a later line follows a call directly, suggesting the user move the `{` to the call's line if a trailing closure was intended — that catches the typical foot-gun (intending a closure but writing the brace on the next line) without requiring users to debug a downstream "Undefined variable". Existing same-line builder/closure syntax (`x = build() { ... }`, `frame("y") { ... }`, `panel(...) { ... }`, etc.) is untouched — every existing builder/closure test in the suite uses same-line braces and continues to pass. The `callback` and `|x|` branches retain their explicit-marker semantics; the line check applies only to the bare-`{` branch. Includes a fuller integration repro mirroring the original 360-line failing shape from the downstream svn-aether port (function with `if` body, function-call-RHS assignment, nested bare-brace block with multiple references to the outer local).

- **Actor receive-handler codegen no longer leaks `declared_vars` from preceding regular functions** (`compiler/codegen/codegen_actor.c`, `tests/integration/declared_vars_actor_handler_leak/`). Pre-fix the codegen's `declared_vars` list (used by `hoist_loop_vars` to decide whether to emit a C type declaration for a variable before a `while` loop) was not reset at the top of each receive-handler body. `codegen_func.c` already calls `clear_declared_vars(gen)` at the top of every regular-function body and marks each parameter as declared; the actor handler emitter did not. Effect: when a regular function defined before an actor took a parameter named (e.g.) `loop_start`, and the actor's receive arm first-assigned a local of the same name inside a `while` body, `hoist_loop_vars` saw the stale entry and skipped emitting `int64_t loop_start;` at the function scope. The handler body then assigned to an undeclared identifier and gcc/clang rejected the generated C with `use of undeclared identifier`. Now `generate_actor_definition` calls `clear_declared_vars(gen)` and `clear_heap_string_vars(gen)` at the top of each handler, mirrors `codegen_func.c`'s pattern, and additionally marks `_pattern` and each pattern field as declared so a later hoist pass can't double-declare them. Integration test reproduces the exact failure shape (a free function whose parameter shadows a name first-assigned inside an actor handler's `while` body) and asserts the program compiles and runs cleanly.

- **`syntax_test_http_response_accessors` no longer hangs at the tail of `make test-ae`** (`tests/syntax/test_http_response_accessors.ae`). The test passed in isolation but reliably consumed 700%+ CPU at the tail of the parallel runner on macOS, eventually getting killed and reported as a runtime failure. Two issues: (a) it hardcoded port 8080, which collides with anything else listening on a dev machine and is the most-conflicted port in the test runner's path; (b) it relied on natural main-return + scheduler idle-out for shutdown, which under contention (other parallel tests saturating the work-stealing scheduler) caused worker threads to spin instead of observing the shutdown signal. Now uses port `18101` (matching the `tests/integration/http_server_*/` convention of unique 18xxx ports) and ends with an explicit `exit(0)` so the test process hands control back to the OS immediately rather than relying on the scheduler to reach a quiescent state.

### Closes (issues whose code already shipped in [0.98.0]; this PR closes them on the issue tracker)

- AetherString* unwrapped at C extern call sites — code merged in [0.98.0] (PR #299). The PR title used `(#297)` reference form rather than the `Closes #297` magic phrase, so the issue stayed open. (#297)
- Same-file callers can resolve `export`-tagged sibling functions — code merged in [0.98.0] (PR #299). Same close-keyword omission. (#287)
- `std.collections.string_list_*` refcount-aware list — code merged in [0.98.0] (PR #299). Same close-keyword omission. (#274)
- `c_struct` binding DSL closed as superseded by `@extern("c_symbol")` (#234) and module namespacing. The issue reporter's own recommendation in the body is "(A) close as superseded." (#266)

- **`benchmarks/cross-language/run_benchmarks.ae` no longer fails to parse** (`benchmarks/cross-language/run_benchmarks.ae`). Eleven sites in the file used the C-style single-line `if A { if B { stmt } ; stmt ; stmt }` shape, where a brace-delimited block was followed by `;` and additional statements on the same line. Aether's parser does not accept that — `}` ends the inner block; the trailing `;` and following statements expected to be top-level inside the outer block fired `error[E0100]: Expected statement in block`. The form pre-existed the parser and `make benchmark` failed to even compile the runner. Rewrote each occurrence as a multi-line block (one statement per line), which is the canonical Aether shape. No semantic change.

- **`http_server_sse` integration test no longer races with listener startup on slower CI runners** (`tests/integration/http_server_sse/test_http_server_sse.sh`). The test waited for the server's `READY` line to land in stdout before issuing curl, but `server.ae` prints `READY` *before* sending the actor message that opens the listening socket. On macOS this gap was small enough that the existing 0.3s post-`READY` sleep covered it; on Linux GHA runners it stretched to whole seconds and curl returned no headers (no Content-Type, no body — matching the failure pattern reported on Linux/GCC CI for PR #305). Replaced the `READY`-grep + fixed-sleep with the same port-probing pattern the sibling `http_server_actor_dispatch` test already uses: a 15-second deadline loop that issues HEAD-style probes against `/events` until one succeeds, with an early bail if the server process died first. Stable across 3 back-to-back local runs after the change.

## [0.100.0]

### Fixed

- **`is_aether_string` no longer reads past the end of short string allocations** (`std/string/aether_string.h`, `tests/runtime/test_runtime_strings.c`). Pre-fix, the magic-byte check did a 4-byte read at offset 0 — on a 1- or 2-byte malloc'd `char*` (the kind `_aether_interp` produces for short interpolation pieces) that overshoots the allocation and trips ASan. Real-world impact was nil (allocators round small mallocs up to 16-byte blocks, so the OOB read landed in mapped memory and the magic check returned false correctly), but it prevented clean ASan builds for any binary that handles short strings — i.e. most of them. Fix: short-circuit byte-by-byte. The first byte of the magic (`0xDE` little-endian) mismatches for ~99.6% of arbitrary inputs, so the dispatch reads exactly one byte and returns. On the rare 1/256 case where byte 0 matches, validate that the rest of the header looks plausible (non-negative ref_count, capacity ≥ length, non-NULL data) — this also catches the 1-in-2^32 chance of arbitrary content beginning with the full magic before any downstream `s->data` deref. Found by ASan run on a downstream port (svn-aether) chasing a different bug; the OOB was incidental but worth fixing on its own.

### Documentation

- **`docs/c-interop.md` — length-clamp hazard for C shims taking `string` parameters** (`docs/c-interop.md`). The auto-unwrap from #297 means a C shim's `string` parameter arrives as payload bytes, not the AetherString header. A common defensive pattern — `min(caller_len, aether_string_length(s))` to clamp the length "to be safe" — is fatal post-#297: `aether_string_length` falls through to `strlen` on the unwrapped pointer and silently truncates at the first embedded NUL. The downstream svn-aether port hit this in `aether_pristine_concat_binary`, producing corrupted pristine files on disk. Documented the hazard and the correct rule (trust the caller's length; don't re-derive). Cross-references the existing `ptr`-typed-parameter escape hatch for shims that genuinely need to dispatch on the header.

## [0.98.0]

### Added

- **`std.collections.string_list_*` — refcount-aware list for AetherString values** (`std/collections/{aether_stringlist.c,aether_stringlist.h,module.ae}`, `Makefile`, `docs/stdlib-reference.md`, `tests/regression/test_string_list.ae`). Issue #274. Plain `std.list` stores items as raw `void*` and doesn't retain on insert / release on remove or free. That works for caller-managed lifetimes but silently dangles when the list is asked to hold AetherString-bearing values whose refcount drops to zero before the list is freed (the original variable goes out of scope, the string frees, the list holds a stale pointer). New `string_list_*` API wraps the same backing `ArrayList` storage but takes a strong reference on every insert via `aether_string_retain` and releases on remove / clear / free / set-overwrite. Plain `char*` literals pass through unchanged — `string_retain` is a no-op on values that don't bear the AetherString magic header. Surface: `string_list_new`, `string_list_add(list, s) -> int`, `string_list_get(list, i) -> string`, `string_list_set(list, i, s)`, `string_list_size(list) -> int`, `string_list_remove(list, i)`, `string_list_clear(list)`, `string_list_free(list)`. The plain `list_*` API is unchanged and still appropriate for caller-managed `void*` items. Regression test: 6 cases covering add/get round-trip, locality survival under allocator pressure, set-replaces, remove-shifts, clear-empties, and literal pass-through.

### Fixed

- **AetherString* unwrapped at C extern call sites** (`compiler/codegen/codegen_expr.c`, `docs/c-interop.md`, `tests/integration/aether_string_to_c_extern/`). Issue #297. When an Aether value typed `string` (or `ptr`-typed but flowing into a `string`-declared C extern parameter) crossed into a C function declared `const char *`, the codegen passed the raw pointer with no unwrap. For values backed by an `AetherString*` (returned by `string_from_int`, `string_concat_wrapped`, `cryptography.base64_decode`, `fs.read_binary`, etc.), the C side received the **struct header** address — `memcpy` / `strlen` on it read the magic + refcount + lengths fields instead of the byte payload. Symptom: garbage data + wildly wrong lengths, no diagnostic. Now the codegen emits `aether_string_data(value)` at the call site for any TYPE_STRING-expecting param of a non-stdlib-aware C extern. The helper dispatches on the magic header (`s->data` for wrapped, bare pointer for plain `char*`) and is idempotent on either shape. Stdlib functions whose names start with `string_` or `aether_string_` are exempted from auto-unwrap because they go through `str_data` / `str_len` internally and need the header to recover the stored length on binary content (`string.length`, `string_concat_wrapped`, `string.equals`, etc.). The dual of #270 / #273 (length-aware FFI returns inbound); together they close the AetherString boundary in both directions.

- **Same-file callers can resolve `export`-tagged sibling functions** (`compiler/analysis/typechecker.c`, `tests/regression/test_export_same_file_visibility.ae`). Issue #287. The typechecker's first pass walked `program->children` switching on `child->type`, but `export X(...)` wraps the function definition in `AST_EXPORT_STATEMENT`. Without an explicit case for that wrapper, the inner function never got registered in the symbol table — same-file callers got `Undefined function 'X'`. Two shapes were broken: `non-export → export` (a private helper calling an exported sibling) and `export → export` (one exported function calling another). Both used the workaround pattern of splitting each export into `export X(...) { return X_impl(...) }` plus a private `X_impl(...)` that holds the actual body. Now the first pass unwraps `AST_EXPORT_STATEMENT` and registers the inner function so same-file callers resolve via the ordinary lexical-scope path. The typecheck-second-pass case for `AST_EXPORT_STATEMENT` descends into the inner declaration too. The `export` modifier remains a visibility annotation for cross-module qualified calls — `is_export_blocked()` still fires there as before. Regression test covers both shapes plus the qualified-call path (verified indirectly: if the unwrap broke qualified resolution, `main()` couldn't reach the exports either).

- **`name(...) -> (T1, T2, ...) { ... }` parenthesised tuple return type now accepted on plain function definitions** (`compiler/parser/parser.c`, `compiler/codegen/codegen_func.c`, `docs/language-reference.md`, `docs/c-interop.md`, `tests/regression/test_tuple_return_function_def.ae`). Issue #285. Mirrors the form already shipped for `extern f(...) -> (T1, T2)` (#271). Pre-fix: the parser switch that disambiguated `-> Type { body }` from `-> expr` had no arm for `(`, so the form was rejected. Worse, when the same function used `return a, b` without annotation, the codegen emitted a `_tuple_T1_T2` struct literal in the body but declared the function's C return type as the first slot's scalar — silently miscompiling. Fix: parser arm for `( TypeKeyword ,` (the comma after the first inner token disambiguates from the `-> (a + b)` arrow-body form — only the tuple-type form has that shape), plus a codegen call to `ensure_tuple_typedef` before emitting the function signature so the typedef is in scope before the signature uses it.

## [0.96.0]

### Closing this PR

This issue-pack PR closes **5 of 6** open issues and ships **3 of 4
Tiers + SSE** of #260's Apache-class HTTP server umbrella:

- **Closes #243** — module merger transitive imports + per-module
  sealed namespace visibility. Round-1 shipped the BFS pass; round-2
  closes the encapsulation hole that allowed user code to call
  transitively-pulled-in namespaces directly.
- **Closes #250** — string-interp accumulator-loop pattern. The
  underlying bug — `out = "${out}${chunk}"` in a loop leaking the
  old buffer with O(n²) growth + potential corruption on allocator
  reuse — was already fixed in `4f2e356` ("fix: free intermediate
  strings on reassignment in loops", April 2026) by paul-hammant.
  This PR adds the regression test that pins the corrected
  behaviour across five shapes (plain self-aliasing loop,
  function-returned chunks, nested accumulators, branch-local
  interp, 100-iteration long-run) so the bug can't silently come
  back. We're closing the GitHub issue here because the fix had
  no test attached; if a reviewer prefers to keep #250 open until
  a separate review, drop the `Closes #250` line and reopen.
- **Closes #239** — `std.http.client` redirect support + end-to-end
  verification. Round-2 added cases 8–11 from the issue's
  acceptance criteria (3-hop chain / hop-limit / loop detection /
  absolute Location URL); the end-to-end test caught two real bugs
  in the round-1 code (inverted parse_url result check + redirect_
  error vs error field collision) which are also fixed in this PR.
- **Closes #265** — source-location intrinsics. Round-1 shipped
  the bare `__LINE__` / `__FILE__` / `__func__`; round-2 ships
  default-argument syntax + caller-site capture so
  `f(msg, line: int = __LINE__)` substitutes the call site's line.
- **Closes #261** — TinyGo host. Round-1 shipped 5 fixed wrapper
  signatures; round-2 expands to 30 covering common arg/return
  permutations + adds libffi-backed `tinygo.call_dynamic` for
  signatures the fixed set doesn't cover.
- **Addresses #260 (Tier 0 + Tier 1 + Tier 3 + SSE shipped;
  HTTP/2 + WebSocket follow up)**:
  - Tier 0 — TLS termination, HTTP/1.1 keep-alive, per-connection
    actor dispatch with reusable drain helper.
  - Tier 1 — 8 middleware (cors / basic_auth / rate_limit / vhost
    / gzip / static_files / rewrite / error_pages).
  - Tier 3 — graceful shutdown, on_start/on_stop hooks, health
    probes (live + ready), structured access logging
    (combined / JSON), per-route Prometheus metrics.
  - Tier 2 — Server-Sent Events (real streaming dispatch) +
    WebSocket (full RFC 6455 framing, handshake, ping/pong,
    close codes).
  - **HTTP/2 (via nghttp2 wrapping) is deferred to a follow-up PR**
    — full implementation estimate and design notes recorded in
    [`docs/next-steps.md`](docs/next-steps.md). HTTP/2 is the
    last remaining piece of Tier 2; the umbrella issue #260 stays
    open until it ships. Everything else (Tier 0 / Tier 1 / Tier 3
    / SSE / WebSocket) is fully closed.

### Added

- **WebSocket (#260 Tier 2 / E2 — RFC 6455)** (`std/net/aether_http_server.{c,h}`, `std/http/module.ae`, regression test `tests/integration/http_server_websocket/`). Same handler-owns-the-connection shape as SSE: server completes the upgrade handshake (`Sec-WebSocket-Accept` = `Base64(SHA-1(client_key + magic-uuid))`, raw SHA-1 via OpenSSL + Base64 via `std.cryptography`), then hands an `HttpWsConn*` to the handler.
    ```aether
    @c_callback echo_handler(req: ptr, ws: ptr, ud: ptr) {
        kind = http.ws_recv(ws)
        if kind != -1 {
            msg = http.ws_message(ws)
            http.ws_send_text(ws, "echo: ${msg}")
        }
    }
    http.server_websocket(server, "/echo", echo_handler, null)
    ```
  Frame coding implements the full RFC 6455 wire format: opcode + FIN, MASK bit, 7/16/64-bit payload-length, masking-key XOR. Server-to-client frames are unmasked per spec; incoming client-to-server frames have their masking-key applied during the read. Continuation frames (FIN=0) get reassembled into a single message before returning to the caller. Control frames (PING, PONG, CLOSE) are auto-handled inside `ws_recv` — pings get a pong with the same payload, close echoes the peer's close frame and returns -1 to the caller. Outgoing API: `ws_send_text`, `ws_send_binary` (binary-safe via explicit length), `ws_close` (with code + optional reason). The handler exits cleanly with a 1000 (normal closure) frame when it returns; the dispatch path also drains pre-buffered bytes from the connection's read buffer (some clients send the first frame in the same TCP packet as the upgrade headers — those bytes had to come from `conn->buf`, not a new `recv()`). Test fixture uses Python's `websockets` library to drive a 3-message text-echo round-trip via real RFC 6455 framing; skips when Python or the websockets library is missing.

- **Server-Sent Events (#260 Tier 2 / E3)** (`std/net/aether_http_server.{c,h}`, `std/http/module.ae`, regression test `tests/integration/http_server_sse/`). Real streaming SSE — handler owns the connection lifetime and pushes events directly to the wire. New API:
    ```aether
    @c_callback events_handler(req: ptr, sse: ptr, ud: ptr) {
        http.sse_send(sse, "tick", "1")
        http.sse_send_id(sse, "tick", "3", "evt-3")
    }
    http.server_sse(server, "/events", events_handler, null)
    ```
  Routes registered via `http_server_sse` are matched ahead of normal routes; on hit, the server emits the standard SSE response head (`200 OK`, `Content-Type: text/event-stream`, `Cache-Control: no-cache`, `Connection: close`) and invokes the handler with an `HttpSseConn*` that wraps the underlying `HttpConn` (so TLS-wrapped connections work transparently — SSE over HTTPS gets the same handling). `http_sse_send_event` / `http_sse_send_event_id` format and write one event chunk per call (correctly splitting multi-line `data:` payloads on `\n` per the spec); they return -1 on transport error so the handler can stop emitting on disconnect. The connection closes when the handler returns. End-to-end test connects with `curl -N` and verifies the Content-Type header, three event chunks with the right names + data, and the third event's `id: evt-3` line.

- **HTTP server observability: structured access logs + Prometheus metrics (#260 Tier 3 — F1 + F2)** (`std/net/aether_http_server.{c,h}`, `std/http/module.ae`, regression test `tests/integration/http_server_observability/`). Closes Tier 3 with a per-request observation hook chain on `HttpServer` plus two built-in consumers:
    - **Access log** — `http.server_set_access_log(server, format, output_path) -> string`. `format` is `"combined"` (NCSA Combined Log Format, Apache default) or `"json"` (one-object-per-line). `output_path` is a file path (opened with `"ab"` so process restarts append rather than truncate), `"-"` for stderr, or `""` to disable. The logger captures method, path, version, status, body bytes, duration in microseconds, and the `User-Agent` / `Referer` / `X-Forwarded-For` headers when present.
    - **Prometheus metrics** — `http.server_set_metrics(server, endpoint) -> string`. Tracks per-route counters (requests_total, errors_total ≥500, 4xx_total) and a latency histogram with buckets at 5ms / 25ms / 100ms / 500ms / 2s / 10s / +Inf. Exposes the standard Prometheus text format (`text/plain; version=0.0.4`) on the configured endpoint (default `/metrics`). All counters are `_Atomic long` updated lock-free; the route hashtable uses a single mutex for insertion-time growth only. Buckets follow Prometheus convention (cumulative — each bucket counts events ≤ upper bound). Sums emitted in seconds-as-double to match the histogram unit.
  Both consumers register against a new public `http_server_use_request_hook(server, hook, ud)` API — user code can register custom hooks alongside the built-ins (e.g. ship requests to OpenTelemetry, distributed tracing). Hooks fire after the response transformer chain has run (so the hook sees the final response state, including gzip Content-Encoding), with a microsecond-precision duration measured from request-parse start. Test fixture drives 4 requests (3 successful, 1 forced-500), then verifies the JSON log captured all 4 entries with the right fields and `/metrics` returns parseable Prometheus output with the expected counters and histograms.

- **HTTP server lifecycle: graceful shutdown + on_start/on_stop hooks + health probes (#260 Tier 3 — 3 of 5)** (`std/net/aether_http_server.{c,h}`, `std/http/module.ae`, regression test `tests/integration/http_server_ops/`). Three pieces of Tier 3 ops surface in one commit, all sharing the new lifecycle plumbing:
    - **Graceful shutdown** — `http.server_shutdown_graceful(server, timeout_ms) -> string`. Stops accepting new connections (delegates to `http_server_stop` to flip `is_running` and close the listen socket), then waits up to `timeout_ms` for in-flight connections to finish naturally. Returns `""` on clean drain or `"timeout"` if the deadline passed with connections still active. Backed by a new atomic `inflight_connections` counter on `HttpServer`, incremented at the top of `http_server_drain_connection` and decremented at the bottom — so the counter is accurate across both thread-pool and actor-dispatch modes (both go through the same drain helper from Phase C3).
    - **Lifecycle hooks** — `http.server_on_start(server, hook, ud)` / `http.server_on_stop(server, hook, ud)`. `on_start` fires once after the listen socket is bound, before the accept loop runs (typical use: log "ready", flip a readiness probe to 200). `on_stop` fires after the accept loop exits, before sockets close (typical use: flush logs, snapshot metrics). Hooks are C function pointers — declared with `@c_callback` on the Aether side via the existing #235 mechanism.
    - **Health probes** — `http.server_set_health_probes(server, live_path, ready_path, ready_check, ud)`. Registers two routes in one call: a stateless live probe (always 200 "ok") and a ready probe that calls the user's `ready_check` callback (1 → 200 "ready", 0 → 503 "not ready"). Either path may be `""` to skip; `ready_check` may be null. Mirrors Kubernetes / Docker probe expectations exactly. End-to-end test verifies on_start fires (writes a marker file from the hook), `/healthz` returns 200, `/readyz` returns 200 with `ready_check` returning 1, and the slow-handler `/work` route serves a 200ms request cleanly.

- **TinyGo host: 30-signature fixed wrappers + libffi-backed dynamic dispatch (#261 round 2)** (`contrib/host/tinygo/aether_host_tinygo.{c,h}`, `contrib/host/tinygo/module.ae`, `tests/scripts/contrib_host_demos.sh`). Closes #261. Round-1 shipped 5 fixed wrapper signatures; this commit expands to 30 covering common 1/2/3-argument permutations of `int`/`int64`/`double`/`string`/`ptr`/`void` — plus a libffi-backed `tinygo.call_dynamic` escape hatch for signatures the fixed set doesn't cover. Naming convention is compact: `tinygo.call_<ret>_<arg1>_<arg2>_...` with single-character codes (`v` void, `i` int32, `l` int64, `d` double, `s` string, `p` ptr) — `call_i_i_i` is `int(int, int)`, `call_s_s` is `string(string)`, etc. Fixed wrappers stay zero-overhead (one symbol lookup + indirect call) for the hot path; the round-1 names (`call_int_void`, `call_int_int`, …) are kept as backward-compat aliases so existing callers don't break. Macro-generated bodies (`DEF_R0`/`DEF_R1`/`DEF_R2`/`DEF_R3` + void-return variants) keep the C source compact even at 30 wrappers. Dynamic dispatch via libffi when `AETHER_HAS_LIBFFI` is defined: `tinygo.call_dynamic(handle, sym, return_kind, arg_kinds, args, result_out)` builds an `ffi_cif` per call, invokes via `ffi_call`. Build-time auto-detection via `pkg-config libffi`; when absent, `call_dynamic` returns 0 and `tinygo.last_error()` reports `"libffi unavailable"`. Mirrors the `AETHER_HAS_OPENSSL` / `AETHER_HAS_ZLIB` graceful-fallback pattern used elsewhere in the stdlib. The contrib host probe in `tests/scripts/contrib_host_demos.sh` picks up the libffi flags automatically when available.

- **`std.http.middleware` — gzip, static_files, rewrite, error_pages (#260 Tier 1 complete, 8 of 8)** (`std/http/middleware/aether_middleware.{c,h}`, `std/http/middleware/module.ae`, `std/net/aether_http_server.{c,h}`, regression test `tests/integration/http_middleware_d2/`). Closes the Tier 1 middleware set. Three of the four are response-side or path-mutating, which prompted two foundational additions:
    - **Response-transformer chain** on `HttpServer` (`http_server_use_response_transformer(server, xform, user_data)`). Runs after the route handler emits a response, before serialization. Transformers may mutate the response (compress body, swap error pages, add headers). Registered in chain order.
    - **Length-aware serializer** (`http_response_serialize_len(res, *out_len)`). The pre-#260 `http_response_serialize` returned a NUL-terminated string; the wire send used `strlen` on it, which silently truncated binary bodies (gzip, octet-stream payloads) at the first NUL byte. The new variant returns the explicit byte length. The connection drain path now uses it. Old `http_response_serialize` kept as a thin shim for downstream consumers.
  Four middleware in this commit:
    - **`use_gzip(server, min_size, level)`** — response-side compressor. Skips bodies below `min_size`, skips when client doesn't send `Accept-Encoding: gzip`, skips when response already has Content-Encoding, skips when compression doesn't shrink the body. Emits gzip framing (windowBits=15+16 — header + deflate + CRC + ISIZE), updates Content-Length to the post-compression size, adds `Vary: Accept-Encoding`. Built under `AETHER_HAS_ZLIB`; no-zlib build is a no-op.
    - **`use_static_files(server, url_prefix, root)`** — mounts a filesystem directory under a URL prefix. Path traversal (`../`) blocked at the middleware level (returns 403). MIME type auto-detected via the existing `http_mime_type`. Short-circuits the middleware chain when it claims a path.
    - **`use_rewrite(server, opts)`** + **`rewrite_add_rule(opts, from_prefix, to_prefix)`** — URL prefix rewriting. Mutates `req->path` in place before route matching runs. Multiple rules apply in registration order; first match wins. Cheap enough to run before route dispatch.
    - **`use_error_pages(server, opts)`** + **`error_pages_register(opts, status, body, content_type)`** — response-side substitution. When the route handler emits a registered status code (typically 404 / 500), the body and Content-Type are swapped for the configured custom page.
  End-to-end test drives all 4 against one server via curl: `/old/data` rewrites to `/api/data`, `/static/hello.txt` serves the file, `/static/../etc/passwd` returns 403, gzip response decompresses back to the original 200-byte plaintext, no compression when client doesn't accept gzip, 404 + 500 responses get the registered custom HTML.

- **`std.http.middleware` — 4 production middleware (#260 Tier 1, 4 of 8)** (`std/http/middleware/aether_middleware.{c,h}`, `std/http/middleware/module.ae`, regression test `tests/integration/http_middleware_d1/`, `Makefile`). Composable middleware for the HTTP server, plugged into the existing function-pointer chain (no new dispatch path; hot loop stays C function pointers, fastest practical shape). Aether-side factory wrappers allocate the per-middleware config once and register the middleware in one call:
    ```aether
    middleware.use_cors(server, "*", "GET, POST, OPTIONS",
                         "Content-Type, Authorization", 0, 600)
    middleware.use_basic_auth(server, "Restricted", verify_cb, null)
    middleware.use_rate_limit(server, 100, 60000)
    middleware.use_vhost(server, "api.example.com,app.example.com")
    ```
  Four middleware in this commit:
    - **`use_cors`** — emits `Access-Control-Allow-Origin/Methods/Headers/Credentials` on every response; preflight OPTIONS short-circuits with `204` plus the configured `Access-Control-Max-Age`.
    - **`use_basic_auth`** — RFC 7617 Authorization: Basic header, base64-decodes credentials, hands them to a user-supplied `@c_callback` verifier (Aether-side function exposed as a C symbol via #235's annotation), emits `WWW-Authenticate: Basic realm="..."` on 401.
    - **`use_rate_limit`** — per-client-IP token bucket: continuous refill at `max_requests / window_ms` tokens per ms, single mutex protects the table, identifies clients via `X-Forwarded-For` → `X-Real-IP` → `"anonymous"` chain, returns 429 + `Retry-After` when budget exhausted.
    - **`use_vhost`** — Host-header gate: parses a comma-separated list of allowed hostnames, returns 404 (configurable) for unknown hosts, case-insensitive, strips `:port` before comparison.
  Companion fix in `http_server_use_middleware`: changed from prepend-to-head to append-to-tail so registration order matches execution order — what every HTTP framework documents and what the factory wrappers above assume. Pre-#260 servers only ever installed at most one middleware so the LIFO bug was invisible until now. End-to-end test drives all 4 middleware against a single server: vhost rejects `Host: foo.example` with 404, basic_auth gates the route until `alice:secret` arrives, OPTIONS preflight gets a 204 with Max-Age=600, and a 15-burst over the 10-token budget surfaces a 429 with Retry-After. The remaining 4 middleware (`gzip`, `static_files`, `rewrite`, `error_pages`) ship in a follow-up commit alongside a response-transformer hook (gzip needs to run AFTER the route handler, not before).

- **Per-connection actor dispatch with reusable drain helper (#260 Tier 0)** (`std/net/aether_http_server.{c,h}`, `std/http/module.ae`, regression test `tests/integration/http_server_actor_dispatch/`). Third Tier 0 piece of #260. Extracts the keep-alive drain loop from `handle_client_connection` into a public `http_server_drain_connection(server, fd)` helper that user actor step functions (registered via `http_server_set_actor_handler`) can call to get the same TLS handshake / keep-alive / route-dispatch behaviour the thread-pool worker path uses. Long-lived connections (keep-alive sessions today, future SSE / WebSocket) now live in their own actor without copy-pasting the lifecycle logic. The thread-pool path itself becomes a one-line caller of the helper, so plain HTTP and TLS-wrapped HTTP get identical handling regardless of which dispatch mode the server uses. Aether-side wrapper exposed as `http.http_server_drain_connection`. Bonus fix surfaced by the parallel-keep-alive test: introduced a per-connection persistent read buffer (`HttpConn.buf` + read/write positions) that survives across requests on the same connection — the previous `malloc`-per-request shape silently dropped pipelined bytes the recv had already pulled past the first request's body boundary. The buffer compacts after each parsed request to reclaim head space, doubles capacity on demand for large bodies, and frees on connection close. Test fixture drives 4 parallel curl clients, each issuing 25 requests (100 total) over its own keep-alive TCP connection with a unique `X-Session-Id` header, and verifies every response carries the right session ID with no cross-pollution under contention.

- **HTTP/1.1 keep-alive on the HTTP server (#260 Tier 0)** (`std/net/aether_http_server.{c,h}`, `std/http/module.ae`, regression test `tests/integration/http_server_keepalive/`). Second Tier 0 piece of #260. Until now the connection handler closed the socket after every response — every request paid TCP handshake + (for TLS) handshake setup, even from the same client. New API `http.server_set_keepalive(server, enabled, max_requests, idle_ms) -> string` configures the connection lifecycle to read multiple requests off one socket. Loop terminates when: client sends `Connection: close` (or HTTP/1.0 default), `max_requests` reached (0 = unlimited), `idle_ms` elapses with no new bytes (0 = use default 30s `SO_RCVTIMEO`), or response status mandates close (408 / 426). Server emits the right `Connection:` and `Keep-Alive: timeout=N, max=M` headers per response so the client knows what to expect. Implementation extracts the per-request body from `handle_client_connection` into a new `handle_one_request` helper that returns a "should-loop" signal; the outer function becomes a clean drain loop on top of the `HttpConn` transport from C1 (so plain HTTP and TLS-wrapped HTTP get keep-alive identically). Default behaviour unchanged — `keep_alive_enabled = 0` matches pre-#260 single-request semantics. Test fixture issues 3 requests over a single TCP connection via `curl`'s multi-URL form (which reuses the connection by default), verifies all 3 succeed, and asserts curl's verbose log reports >= 2 connection-reuse events plus at least one `Connection: keep-alive` header in the responses. Skips when `curl(1)` is missing.

- **TLS termination on the HTTP server (#260 Tier 0)** (`std/net/aether_http_server.{c,h}`, `std/http/module.ae`, regression test `tests/integration/http_server_tls/`). First Tier 0 piece of #260 — the HTTP server can now accept TLS-encrypted connections directly without an external proxy. New API `http.server_set_tls(server, cert_path, key_path) -> string` loads a PEM cert + key, validates they match, configures an `SSL_CTX` with TLS 1.2+ minimum and compression / renegotiation disabled, then marks the server TLS-enabled. After this, every accepted connection completes a `SSL_accept` handshake before the HTTP parse runs; plaintext clients hitting the TLS port get rejected at handshake time. Uses the same `AETHER_HAS_OPENSSL` graceful-fallback pattern as `std.cryptography` and the http client (TLS): when the build doesn't include OpenSSL, the function returns `"TLS unavailable: built without OpenSSL"` rather than failing to compile. Implementation introduces a small `HttpConn` transport abstraction (`{ fd, ssl }` + `conn_recv` / `conn_send` / `conn_close`) so `handle_client_connection` reads identically whether the connection is plain or TLS — `recv` / `send` calls polymorphise via the helper. The transport layer is also the substrate Phase C2 keep-alive will plug into. The cert + key loading is idempotent (re-loading replaces the prior context); free path tears down the `SSL_CTX` cleanly. Test fixture generates a self-signed RSA-2048 cert at test time via `openssl req`, spins up the in-process TLS server on port 18102, drives one HTTPS request via `curl --cacert` (verifies handshake + body), then sanity-checks that a plain-HTTP request to the same port fails (TLS-only). Skips cleanly when `openssl(1)`, `curl(1)`, or `AETHER_HAS_OPENSSL` is missing.

- **Direct invocation of function-typed locals — `handler(args)` syntax** (`compiler/analysis/typechecker.c`, regression test `tests/integration/fn_typed_local_call/`). Foundation for #260 D pure-Aether middleware composition. Aether closures live in `_AeClosure` values; until now they were called via `call(fn, args)`. This commit makes the natural `fn(args)` syntax also work for any local variable whose inferred type is `TYPE_FUNCTION`. Implementation: when the typechecker's `typecheck_function_call` lookup finds a non-function symbol whose type is `TYPE_FUNCTION`, it transparently rewrites the AST to flow through the existing `call(fn, args...)` codegen path — the call's name changes from `<varname>` to `"call"`, a new `AST_IDENTIFIER` child holding the original name is inserted at position 0, and the rest of the args shift right. Codegen sees a normal `call(...)` invocation and emits the right closure-dispatch C. Zero new codegen paths; all existing closure-call infrastructure (`_AeClosure { .fn, .env }` heap-allocated capture, generic dispatch in `codegen_expr.c:~2155`) is reused. Four-case regression test covers: builder-DSL closure assigned to local + called directly + called twice; closure passed as a fn-typed parameter + called inside the callee body; closure capturing a local then invoked; closure that itself invokes another closure directly. All 337 integration tests green. Sets up Phase D's `gzip(handler) -> handler` middleware composition shape: middleware now reads as the same function-takes-function-returns-function pattern every modern HTTP framework uses (Go `http.Handler`, Rust `tower::Service`, Express).

- **Caller-site capture for `__LINE__` / `__FILE__` / `__func__` in default arguments — closes #265 round 2** (`compiler/analysis/typechecker.c`, `compiler/codegen/codegen_expr.c` comment update, regression test `tests/integration/source_location_default_capture/`). Closes #265. Round-1 shipped the bare intrinsics; the issue's recommended Capture-1 mechanism (default-argument substitution) was deferred. This commit closes that gap. When a function declares `logme(msg: string, line: int = __LINE__)` and a caller writes `logme("hi")`, the typechecker's default-fill (Phase A2.1) clones the default expression — but a naive clone preserves the `__LINE__` AST node's line, which is the function-definition's line, so codegen would emit the wrong number. Fix: a new helper `rewrite_caller_site_intrinsics()` walks the cloned tree and overwrites every `__LINE__` AST_IDENTIFIER node's `line`/`column` with the call site's location before the clone gets appended to the call. `__FILE__` works automatically — codegen reads `gen->source_file` (a per-translation-unit global), which is naturally the file holding the call. `__func__` works automatically too — codegen emits the C99 `__func__` keyword which the C compiler resolves to the enclosing C function (which mirrors the calling Aether function's name in the common case). The regression test pins the behaviour: `logme(msg: string, line: int = __LINE__)` called from three distinct sites in two distinct enclosing functions yields three distinct line numbers each matching the call site, plus an explicit-override case that bypasses the default. Combined with Phase A2.1, Aether now gives users the killer ergonomics the issue called for: `logme("done")` and the framework records the caller's file/line/function automatically.

- **Default function arguments — `f(name: string, greeting: string = "Hello")`** (`compiler/parser/parser.c`, `compiler/analysis/typechecker.c`, regression test `tests/integration/default_arguments/`). Foundation for #265's caller-site source-location capture, also useful on its own. Parser accepts an optional `= expr` after the type annotation in both parameter shapes (Aether-style `name: type = expr` and C-style `int name = expr`); the default expression is stashed as the first child of the `AST_PATTERN_VARIABLE` with `annotation = "has_default"` so it doesn't conflict with struct- or list-pattern children. Typechecker relaxes the function-call arity check from `got == expected` to `got >= required && got <= total` (where `required` excludes parameters that have defaults), then for any missing trailing slots clones the declared default expressions and appends them to the call's child list before the per-arg type-check loop runs — so codegen sees a fully-populated call and emits the right C without any codegen-side changes. Defaults trail required (Python rule); v1 limitation: default expressions cannot reference other parameters (parameter values aren't in scope at call sites where the default expression is type-checked). Source-location intrinsics (`__LINE__` / `__FILE__` / `__func__`) are global builtins so they DO work as default values — the next commit (Phase A2.2) wires the caller-site capture so `f(msg, line: int = __LINE__)` substitutes the call site's line number rather than the function definition's. Six-case regression test covers single-default omitted/supplied, multi-default omitted/partial/supplied, and required+default mix.

### Fixed

- **HTTP client `http_resolve_location` — fix inverted parse_url result check** (`std/net/aether_http.c`). Issue #239 (round 2). Round-1's redirect-following implementation called `parse_url(base_url, ...) != 0` to detect failure — but `parse_url` returns `1` on success, so the check fired on every well-formed base URL and the function returned NULL, surfacing as `"malformed Location header"` for every redirect attempt. Same inverted check existed in the cross-host-header-stripping path (so the strip logic never ran in practice). Caught by the new end-to-end redirect-chain test in `tests/integration/test_http_client_v2.ae` (cases 13-16); round-1 only had API-contract tests that didn't exercise the C-side redirect loop. Both inverted checks fixed; redirect-following now actually works for relative, absolute, and self-loop Location values.

- **HTTP client redirect-class errors moved to `redirect_error` field** (`std/net/aether_http.{c,h}`, `std/http/client/module.ae`, regression test `tests/integration/test_http_client_v2.ae`). Closes #239 round 2. Round-1 surfaced redirect failures (hop limit reached, loop detected, scheme downgrade, malformed Location) via the same `response.error` field used for transport failures. The v2 `client.send_request` wrapper auto-frees the response when `error` is set — which conflated "no usable response, discard" (transport failure) with "chain ended on a 3xx, here's the terminal response" (redirect failure). Callers that wanted to inspect the terminal status / body for retry logic couldn't, because the wrapper had already freed the response. Fix: new `redirect_error` field on `HttpResponse`, populated by the redirect loop instead of `error`. The `send_request` wrapper still auto-frees on `error` (transport-class) but PRESERVES the response on `redirect_error` only. New accessor `client.response_redirect_error(resp)` returns the redirect-class error string. End-to-end test (cases 13-16 in `test_http_client_v2.ae`) now exercises: 3-hop chain with relative Locations + `response_url` reflects final hop; hop-limit-exceeded surfaces `redirect_error` with response preserved; self-loop short-circuits via the visited-URL detector; absolute Location URLs resolve correctly. Round-1's `tests/integration/http_client_redirects/` API-contract test continues to pass.

- **Per-module sealed namespace visibility — closes encapsulation hole left by round-1 #243 fix** (`compiler/analysis/typechecker.{c,h}`, `compiler/analysis/type_inference.c`, `compiler/aether_module.c`, regression test `tests/integration/sealed_namespaces/`). Closes #243 (round 2). The round-1 BFS transitive-merge pass merged dependencies' decls into the program AST and registered their namespaces in the global `imported_namespaces[]` array — but that registration leaked into user-code qualified-call resolution, so a user who only wrote `import lib_a` could still call `lib_b.shout()` directly even though `lib_b` was pulled in transitively as `lib_a`'s internal dependency. Issue #243's recommended long-term shape was per-module sealed scopes; this commit ships that. Two parallel namespace registries: `imported_namespaces[]` keeps every transitively-merged namespace (used by cloned merged-module bodies that need to resolve their own internal qualified calls — `client_post_json`'s body still calls `json.parse` even though the user never imported `std.json`); a new `user_explicit_namespaces[]` only holds namespaces the user explicitly imported. Synthetic AST_IMPORT_STATEMENT nodes injected by `module_merge_into_program`'s BFS pass are now marked `annotation = "synthetic"` and skipped from the user-explicit registry. The visibility gate at qualified-call resolution (`is_visible_namespace`) picks the right registry based on a context channel: a `SymbolTable::inside_merged_body` flag (propagated from parent in `create_symbol_table`, set on a function-local table when the AST node has `is_imported == 1`) PLUS a global walk-counter (`g_inside_merged_body` with `typechecker_enter_merged_body` / `typechecker_leave_merged_body`) so type inference's constraint-collection pass — which walks against the global symbol table without per-function child tables — sees the relaxed visibility too. The negative regression test pins the closure: a fixture where user imports `lib_a` (which internally uses `lib_b`) attempts `lib_b.shout()` directly from main; build now fails with `error[E0301]: undefined function 'lib_b.shout'`. The positive `tests/integration/transitive_module_import/` round-1 fixture continues to pass — merged `lib_a.greet()` body still resolves `lib_b.shout()` cleanly. All 334 integration tests green; no regressions in the existing stdlib usage of cross-namespace internal calls.

### Deferred

- **HTTP server Apache-class capability gap (#260) — Tier 0 + Tier 1
  remain on the roadmap.** This issue-pack PR sized #260 (TLS
  termination + HTTP/1.1 keep-alive + per-connection actor dispatch +
  eight stdlib middleware modules) at 3–4 weeks of focused work and
  scoped it out of the current pack to land the smaller issues
  cleanly. Concrete next-steps inventory now lives in
  [`docs/next-steps.md`](docs/next-steps.md) → "HTTP server —
  Apache-class capability gap (#260)" with realistic per-tier
  estimates (Tier 0 ≈ 8–11 days, Tier 1 ≈ 4–5 days, Tier 2 / 3 still
  deferred behind Tier 0). The umbrella issue is the canonical
  tracker; per-tier sub-issues should be filed when scheduled. The
  existing HTTP server (routing, middleware function-pointer chain,
  multi-accept with SO_REUSEPORT, optional actor dispatch via
  `MSG_HTTP_CONNECTION`) is unchanged by this PR and continues to be
  the supported surface for HTTP/1.1 request/response use cases.

### Added

- **`string.format(fmt, args)` — Aether-callable formatter** (`std/string/{aether_string.c,aether_string.h,module.ae}`, `docs/stdlib-reference.md`, `tests/regression/test_string_format.ae`). Issue #272. The C-side `string_format(fmt, ...)` already existed but wasn't reachable from Aether (externs don't support varargs). New `string_format_list(fmt, args)` C primitive walks the format string substituting `{}` placeholders with entries from an `std.list` of strings. `{{` / `}}` are literal braces. The Aether wrapper `string.format(fmt, args)` is a one-liner over the new primitive. `{}`-placeholder choice (rather than `%s`) composes more cleanly with Aether's existing `${...}` interpolation surface and leaves `%`-prefix open for typed printf-style formatters if they land later. Non-string args (ints, floats) need explicit conversion via `string.from_int(...)` before being added to the list. Replaces nested `string.concat(string.concat(...))` patterns common in callers building runtime-formatted log lines or message templates. Regression test: 5 cases covering single-placeholder, multiple-placeholder, literal braces, missing-arg-silently-dropped, and int-via-from_int marshalling.

### Fixed

- **`response_header` snapshots its return value, no longer aliases prior calls** (`std/http/client/module.ae`, `tests/integration/test_http_client_v2.ae`). Issue #269. The underlying `http_response_header_raw` returns a borrowed pointer into a per-thread buffer that the next call overwrites. Pre-fix, calling `client.response_header` twice and holding both results gave you two pointers into the same (clobbered) slot — `string.length(tag)` read the second header's length, `string.equals(tag, "alpha")` returned false even though `tag` was the alpha-header at fetch time. The wrapper now snapshots via `string.copy` so each call's result is independent. The C-side `http_response_header_raw` is unchanged; the documented "thread-local buffer overwritten on next call" caveat is now an implementation detail of the C function rather than a footgun the Aether-level surface inherits. Regression: the existing v2 HTTP-client test gains a Test 2b that fetches two headers from one response, holds both vars, and asserts neither length nor equality was clobbered by the second call.

- **`string + string` rejected at typecheck with helpful hint** (`compiler/analysis/typechecker.c`, `tests/integration/string_plus_reject/`). Issue #276. Previously, `s = "hello" + " world"` compiled in Aether and emitted `(const char*) + (const char*)` C, which the C compiler rejected with a pointer-arithmetic error pointing at generated C — useless to the .ae author. The typechecker now catches the case directly and reports `'+' is not defined for strings — use "${a}${b}" interpolation or string.concat(a, b)` at the .ae source location. `int + int`, `float + float`, and other non-string `+` uses are unchanged. Regression test confirms the diagnostic fires for both `literal + literal` and `var + literal`, and that `int + int` still works.

- **Docs: `as` reserved keyword now in the language reference table** (`docs/language-reference.md`). Issue #275. `as` is reserved by the lexer for import aliasing (`import std.string as str`) but was missing from the Keywords table — a user trying to use `as` as a variable name got a clean compile error but couldn't front-load the check by reading the docs. One-line addition to the table.

## [0.95.0]

### Fixed

- **`--emit=lib` no longer emits broken `aether_*` aliases for tuple-returning helpers** (`compiler/codegen/codegen.c`, `tests/integration/emit_lib_unsupported/`). Issue #277. Previously, a function whose return type is a tuple (`-> { return n, n+1 }`) would still get an `aether_<name>` alias declared `int32_t aether_helper(int32_t n)` — even though the function body returns a `_tuple_int_int` struct. The mismatched signature caused either a compile failure (when both the function and the alias were in the same TU) or silent data loss (the alias path simply ignored the second tuple slot). Now the alias-emission path skips functions whose return type is `TYPE_TUPLE` with the same warning shape it already used for unsupported parameter types: "function 'X' returns a tuple; --emit=lib alias stub skipped." The function itself still emits normally; only the public ABI alias is suppressed. Wrap with a single-value-returning function if the operation needs to cross the library boundary.

- **Trailing-underscore private-helper convention now produces file-local linkage** (`compiler/codegen/{codegen.c,codegen_func.c}`, `docs/emit-lib.md`, `tests/integration/emit_lib_unsupported/`). Issue #279. Two `.ae` files in the same project that both define a private helper with the same name (`record_start_`, `helper_`, `parse_line_`) used to collide at link time because the codegen emitted them with external linkage. Now any function whose name ends in `_` is emitted with C `static` storage and gets no `aether_*` alias — the convention is enforced by the codegen, no annotation needed. The forward declaration follows suit so C accepts the file. The `--namespace` bundler still concatenates source files and would produce duplicate definitions in that path; that's a different bug (the multi-clause combined-function logic) tracked separately. The fix here covers the multi-TU case where each file compiles to its own `.o` and the linker is what would have failed.

- **Variables first-declared inside `if` branches hoisted to function scope when read outside the block** (`compiler/codegen/{codegen_stmt.c,codegen_func.c,codegen_internal.h}`, `tests/regression/test_if_branch_var_hoist.ae`). Issue #278. Pre-fix, code shaped `if cond1 { x = ... } if cond2 { x = ... } return x` failed to compile because the codegen scoped `x` C-locally to each `if` branch and the function-scope `return x` couldn't see it. The existing `hoist_if_else_common_vars` only fired for if/else pairs that both declared the same name. Now `hoist_if_branch_vars` walks every top-level `if` (with or without `else`), collects first-declared names, filters out any already declared at the function-body's top level (preserving the heap-tracker companion variable already emitted on the original-declaration path), and emits a function-scope `Type name;` so subsequent assignments and reads compose. Regression test: `tests/regression/test_if_branch_var_hoist.ae` — two-if-no-else shape with the same variable in both branches and a post-block return.

- **`ae build <bin-name>` resolves `[[bin]]` entries by name; `ae build` walks up to find `aether.toml`** (`tools/ae.c`, `docs/build-system.md`, `tests/integration/bin_name_lookup_and_walkup/`). Issue #280. Two related ergonomic gaps in the build CLI. (1) Bin-name lookup: when the positional argument doesn't exist as a file, `ae` now checks `aether.toml`'s `[[bin]]` entries for a matching `name = "..."` and uses that bin's `path` field — `ae build myapp` is now equivalent to `ae build src/main.ae` when `[[bin]] name = "myapp", path = "src/main.ae"`. Cargo's rule: name matches first if both shapes are ambiguous. (2) Toml walk-up: when there's no `aether.toml` in the current directory, `ae` walks up the directory tree looking for one. If found, `chdir` to that ancestor and re-resolve relative positional arguments against the new cwd — so `cd src && ae build main.ae` works as if you had run `ae build src/main.ae` from the project root, with `extra_sources` and other toml settings applied. Walk-up only triggers when the cwd has no toml; a project with a local `aether.toml` keeps existing behaviour. Regression test: `tests/integration/bin_name_lookup_and_walkup/` — covers both the bin-name resolution path and the subdir walk-up path with an `extra_sources`-dependent shim that fails to link if the toml isn't applied.

## [0.94.0]

### Added

- **`std.bytes` — mutable byte buffer with overlap-safe forward `copy_within`** (`std/bytes/{aether_bytes.c,aether_bytes.h,module.ae}`, `Makefile`, `docs/stdlib-reference.md`, `tests/regression/test_std_bytes.ae`). Issue #288. Aether's `string` is immutable; `std.bytes` fills the gap any binary-codec / streaming-buffer workload needs — random-access write at arbitrary indices, overlap-safe forward `copy_within` for RLE expansion, hand-off to a refcounted `AetherString` via `bytes.finish(b, n)`. The headline use case is the RLE-overlap pattern that `string.concat` can't express: when `dst > src`, each iteration of the forward-byte-by-byte loop reads bytes earlier iterations of the same call have just written. `memmove()` would defeat the purpose by choosing a safe direction. Surface: `bytes.new(initial_capacity)`, `bytes.set(b, index, byte)` (gaps zero-fill), `bytes.copy_from_string(b, dst, src, src_len)` (binary-safe via `aether_string_data` — works on AetherString and plain `char*`), `bytes.copy_within(b, dst, src, length)` (forward byte-by-byte), `bytes.length(b)`, `bytes.finish(b, length)` (consumes the buffer), `bytes.free(b)` (discard). Regression test: `tests/regression/test_std_bytes.ae` — six cases covering basic set+finish, gap zero-fill, copy_from_string, RLE expansion (`[A,B]` → `ABABAB` via `copy_within(2, 0, 4)`), non-overlapping copy, free-without-finish.

### Changed

- **`os.run_capture(prog, argv, env)` now returns `(stdout, status, err)`** (`std/os/{aether_os.c,aether_os.h,module.ae}`, `tests/regression/test_run_capture_status.ae`). Issue #289. The previous `(stdout, err)` shape discarded the child's exit code, leaving callers unable to distinguish "ran cleanly" from "ran but exited non-zero" — load-bearing for any tool that signals success/failure via exit code separately from stdout (`diff3 -m` returning 1 for conflicts, `grep` returning 1 for no-match, `gcc` returning non-zero on compile errors). New shape: `output, status, err = os.run_capture(...)`. Status is `WEXITSTATUS` on normal exit; `-1` when killed by signal or when the spawn itself failed. `err == ""` on successful spawn regardless of exit code; non-empty only when the fork/exec couldn't run. Underneath: a new tuple-returning extern `os_run_capture_status_raw(prog, argv, env) -> (string, int, string)` (built on the FFI tuple-return work from #271/#281). The original `os_run_capture_raw` C function stays in place for callers built directly against it; the Aether-level `run_capture` wrapper switches to the new tuple form. **Breaking** for any pre-1.0 caller that was destructuring the old `(stdout, err)` tuple — migration is `output, _, err = os.run_capture(...)` (one extra `_` slot). No `.ae` callers in the tree needed updating; the change is internal-stdlib-only at this point. Regression test: `tests/regression/test_run_capture_status.ae` — three cases: `/bin/true` (status 0), `/bin/false` (status 1), missing binary (status 127, the POSIX exec-in-child convention).

## [0.92.0]

### Added

- **FFI tuple return types — `extern foo(...) -> (T1, T2, ...)`** (`compiler/parser/parser.c`, `compiler/codegen/{codegen.c,codegen_func.c}`, `docs/c-interop.md`, `tests/integration/extern_tuple_return/`). Issues #271 + #273. Extern declarations now accept parenthesised tuple return types; the codegen synthesises a matching `_tuple_<T1>_<T2>` C struct typedef and the C side provides a function returning that struct by value. The destructure (`a, b = extern_fn(...)`) at the call site emits exactly the same shape as Aether-side `-> { return a, b }` already produces. Replaces two workaround patterns: (1) packing values into a delimited string the Aether side has to split, (2) splitting one logical operation into a four-extern split-accessor convention (`<op>_raw` + `<op>_get` + `<op>_get_length` + `<op>_release` with TLS-backed storage). The four-extern pattern remains in the C side for callers built directly against it; new bindings reach for the unified shape. Concrete adoption: `std.fs.read_binary` is now a one-liner over `extern fs_read_binary_tuple(path: string) -> (ptr, int, string)` — same caller surface, half the C plumbing, no TLS bookkeeping. Type-name mangling: `(int, int)` → `_tuple_int_int`, `(int, string)` → `_tuple_int_string`, `(ptr, int, string)` → `_tuple_ptr_int_string` (`string` for `const char*`, `ptr` for `void*`). Regression test: `tests/integration/extern_tuple_return/` — three forms exercised across the FFI boundary: `(int, int)`, `(int, string)` (Go-style result + error), `(ptr, int, string)` (the canonical binary-FFI shape).

- **`@c_callback` FFI annotation — export an Aether function as a C callback** (`compiler/parser/parser.c`, `compiler/codegen/{codegen.c,codegen_func.c,codegen_expr.c,codegen_internal.h}`, `compiler/aether_module.c`, `docs/c-interop.md`, `docs/language-reference.md`). Issue #235. The inverse direction of `@extern` (#234) — marks an Aether function as having a stable, externally-visible C symbol so it can be passed across the linkage boundary as a function pointer to C externs that take callbacks (HTTP route handlers, signal handlers, `qsort` comparators, libcurl callbacks, sqlite hooks, libuv `uv_*_cb`). Two forms: bare `@c_callback my_handler(...)` (Aether-side name == C symbol; namespace-prefixed when the function lives in an imported module) and explicit `@c_callback("aether_signal_handler") on_sigint(sig: int) { … }` (linker resolves the named symbol; Aether-side calls keep the unmangled name). Three codegen changes: (1) annotated functions are emitted without `static` storage even when imported, so the symbol stays externally addressable; (2) the C-decl name slot uses the bound symbol rather than `safe_c_name(func->value)`; (3) at value-position `AST_IDENTIFIER` emission, a new `lookup_c_callback_symbol` helper translates the Aether-side identifier to the bound C symbol so `http_server_add_route(server, "GET", "/", my_handler, null)` resolves at link time. The companion fix in `aether_module.c`'s `rename_intra_module_refs` extends the import-rename pass so identifier-as-value references to imported functions get the namespace prefix (was previously call-site only). Closes the FFI loop bidirectionally with #234. Direct Aether-side calls to annotated functions are unchanged. Regression: `tests/integration/c_callback/` — three forms checked (bare annotation, explicit symbol binding, pointer equality across two reference sites for stable-symbol verification), plus a direct-call sanity check that the annotation doesn't break the regular call path.

- **`@extern("c_symbol")` FFI annotation** (`compiler/parser/{lexer.c,parser.c,tokens.h}`, `compiler/codegen/{codegen.h,codegen.c,codegen_func.c,codegen_expr.c,codegen_internal.h}`, `docs/c-interop.md`, `docs/language-reference.md`). Issue #234. Binds an Aether-side function name to a chosen C symbol — no wrapper function emitted, the codegen forward-decl and every call site use the annotated symbol directly. Replaces the `extern foo_raw(...)` + thin `foo() { return foo_raw(...) }` pattern that the stdlib (cryptography, intarr, …) uses today to keep mangled wrapper names from colliding with the C symbol they wrap. Syntax: `@extern("strerror") describe_errno(errno: int) -> string` — Aether-side name (`describe_errno`) lives in the module's namespace, the linker sees `strerror`. Parameter and return types are required, same as plain `extern`. Codegen translates the Aether-side name to the C symbol via a new `lookup_extern_c_name` helper that consults the existing `extern_registry`; the registry's `ExternParamInfo` entry now carries an optional `c_name` override populated from the annotation. Plain `extern foo(...)` is unchanged. Regression test: `tests/integration/extern_annotation/` — drives two annotated externs through `ae build` against a `shim.c` that only defines the C symbols (not the Aether-side names), so a missing call-site rewrite would surface as a link-time "undefined reference".

- **`std.cryptography` — Base64 codec + algorithm-by-name dispatch** (`std/cryptography/aether_cryptography.c`, `std/cryptography/aether_cryptography.h`, `std/cryptography/module.ae`). Additive over the existing SHA-1 / SHA-256 surface. New: `cryptography.base64_encode(data, length) -> (string, err)` (RFC 4648 §4 standard alphabet, unpadded — callers needing padded output append `=` themselves to make the length a multiple of 4), `cryptography.base64_decode(b64) -> (bytes, length, err)` via the same TLS split-accessor pattern `std.fs.read_binary` uses (binary-safe via `string_new_with_length`, embedded NULs survive); `cryptography.hash_hex(algo, data, length) -> (digest, err)` for runtime algorithm selection where `algo` is `"sha1"`, `"sha256"`, or any name OpenSSL's `EVP_get_digestbyname()` recognizes — returns `"unknown algorithm"` for unrecognized names; `cryptography.hash_supported(algo) -> int` for config-time validation of user-supplied algorithm names before they hit `hash_hex`. Built on OpenSSL's `EVP_EncodeBlock` / `EVP_DecodeBlock` / `EVP_get_digestbyname`. Same no-OpenSSL fallback shape as the existing SHA wrappers (`("", "openssl unavailable")` rather than crash). Unblocks the Aether port of Apache Subversion to drop its hand-written `ae/ffi/openssl/shim.c` (~170 lines of EVP wrappers + Base64 + algorithm dispatch). HMAC, KDFs, symmetric ciphers, signing, streaming digests, URL-safe Base64, and PKCS#7/PEM parsing remain deliberately out of scope — see `docs/stdlib-vs-contrib.md` for the "one obvious shape" criterion. Regression test: `tests/integration/cryptography_v2/` — eight cases covering RFC 4648 §10 known vectors (`"Man"` → `"TWFu"`, `"hello world"` → `"aGVsbG8gd29ybGQ"`), ASCII round-trip, binary-payload-with-NUL round-trip (verified via SHA-256 of orig vs decoded), empty input, dispatcher matches direct `sha256_hex`, unknown algorithm rejected, `hash_supported` true/false. Same SKIP-when-no-OpenSSL guard as the existing `cryptography_sha` test for the Windows CI matrix. Surface documented in `docs/stdlib-reference.md` and `docs/stdlib-api.md` with a "What `std.cryptography` doesn't do" subsection setting expectations against Java's `java.security` / Python's `cryptography` / Go's `crypto/*` (no HMAC, no KDF, no streaming, no constant-time compare).

- **Erlang-style `exports (…)` module visibility list** (`compiler/parser/{parser.c,lexer.c,parser.h,tokens.h}`, `compiler/ast.h`, `compiler/aether_module.c`, `docs/language-reference.md`, `docs/module-system-design.md`). Each module now declares its public API once at the top of the file: `exports (square, cube, PI)` instead of tagging every public function with the per-function `export` keyword. Listed names are reachable from outside the module via either qualified (`mod.name(…)`) or short-alias (`import mod (*)`) forms; unlisted names are private — still callable from inside the module's own functions, but rejected at qualified-call sites with the existing `is_export_blocked()` check. Replaces three flaws of the per-function form: (1) the public API was scattered across the file, (2) only ~5% of stdlib + contrib modules used it (95% relied on default-public, which is the pit-of-failure default), (3) renaming an internal helper could accidentally remove an `export` and silently change a public contract. The new form is single source of truth, scannable, and matches the existing `import mod (a, b, c)` selective-import grammar for parser symmetry. Migration is purely mechanical and v1 ships **additive** — modules without an `exports (…)` line keep the legacy default-public behavior, so existing code (including all of `std/` + `contrib/`) continues to work unchanged. v2 will flip the default to private once the stdlib has been swept. The legacy per-function `export <fn>` form is still accepted for one release with a one-shot deprecation warning per module; mixing both forms in one module is a hard error. Test fixtures under `tests/integration/export_visibility/`, `tests/integration/pure_module/`, `tests/integration/pure_module_mixed/`, and `examples/packages/myapp/lib/utils/` migrated to the new form. New regression tests at `tests/integration/exports_list/` (positive: 3 cases — listed name qualified, listed name via glob, listed const; negative: 2 cases — non-listed function rejected, non-listed const rejected).

- **Glob-form imports — `import mod (*)`** (`compiler/parser/parser.c`, `compiler/analysis/typechecker.c`, `docs/language-reference.md`). New parser form expands at typecheck time to short-name aliases for every public name (no leading underscore) defined by the imported module, so callers can write bare `say_hello()` instead of either enumerating every symbol — `import greeter (say_hello, greet_world, …)` — or qualifying every call as `greeter.say_hello()`. Issue #171 P1. Names with a leading underscore stay private. The non-selective form `import mod` (no parens) still loads the module without registering short aliases — preserves existing behavior. Companion P2 (transitive pull-in for selectively-imported callees) and P3 (extern re-export) are tracked separately on issue #171; this lands the smallest piece that immediately unblocks `contrib/tinyweb`-style modules where examples currently need to enumerate ~70 names. Regression: `tests/integration/glob_import/` (3 cases — public name resolved unqualified, second public function resolved, qualified `greeter.say_hello()` still works alongside the glob).

- **`contrib.host.tinygo` — In-process Go via TinyGo `c-shared`** (`contrib/host/tinygo/{module.ae,aether_host_tinygo.{c,h},README.md,examples/greet.go}`, `examples/host-tinygo-demo.ae`, regression test `tests/integration/host_tinygo/`, `tests/scripts/contrib_host_demos.sh`, `Makefile`). Closes #261. The in-process counterpart of `contrib/host/go` — instead of spawning the standard `go` toolchain as a sandboxed subprocess (~ms per call), this host loads a TinyGo-built `.so` / `.dylib` / `.dll` (`tinygo build -buildmode=c-shared`) into the Aether process via `std.dl` and invokes the exported functions directly (~ns per call). Pre-defined wrapper signatures for v1 cover the most common shapes: `int(void)`, `int(int)`, `int(int,int)`, `void(int)`, `const char*(const char*)` — exposed as `tinygo.call_int_void`, `tinygo.call_int_int`, `tinygo.call_int_int_int`, `tinygo.call_void_int`, `tinygo.call_str_str`. `tinygo.load(path)` aliases `dl.open` and `tinygo.unload(handle)` aliases `dl.close` so call sites self-document the intent. `tinygo.last_error()` surfaces the most recent symbol-resolution or invocation error per-thread. Adding a new shape is a one-line C extension in `aether_host_tinygo.c` plus a matching `extern` + wrapper in `module.ae` — patches welcome. Fully-dynamic dispatch via libffi is intentionally out of scope (libffi adds a system dependency 95% of users do not need; five fixed shapes cover ~80% of real call sites). Containment trade-off vs `contrib/host/go`: this host takes a one-time per-process sandbox grant for the loaded library (sandbox preload still intercepts `dlopen` on POSIX), whereas the subprocess host gets per-libc-call grants on the Go binary; pick by whether you're optimizing for latency or fine-grained enforcement. Cookbook in `contrib/host/tinygo/README.md` covers TinyGo `//export` syntax, the platform suffix matrix, memory ownership pitfalls (`C.CString` leaks unless explicitly freed), and the single-Go-runtime-per-process limitation. Demo at `examples/host-tinygo-demo.ae` exercises every wrapper signature against the bundled `greet.go`. Regression test at `tests/integration/host_tinygo/` builds the c-shared lib at test time and round-trips four call shapes; skips cleanly when `tinygo` is not on `$PATH` (matches `contrib/host/go`'s pattern — host bridges should never break CI on machines that don't have the toolchain installed).

- **`std.dl` — cross-platform dynamic library loader** (`std/dl/aether_dl.{c,h}`, `std/dl/module.ae`, regression test `tests/integration/std_dl/`). Issue #261 prep — the substrate the upcoming `contrib/host/tinygo` (#261) and any other `dlopen`-based interop will sit on top of. Wraps `dlopen` / `dlsym` / `dlclose` (POSIX) and `LoadLibraryA` / `GetProcAddress` / `FreeLibrary` (Windows) behind one Go-style API: `dl.open(path) -> (handle, err)`, `dl.symbol(handle, name) -> (ptr, err)`, `dl.close(handle) -> err`, `dl.last_error() -> string`. Per-thread last-error buffer (matches `dlerror()`'s thread-local semantics on glibc/musl); Windows side runs `FormatMessageA` against `GetLastError()` and strips the trailing CRLF. Path suffix is the caller's responsibility — `.so` / `.dylib` / `.dll` — rather than auto-mangling, which keeps the API predictable for the cross-platform loader case where the host already knows what extension it's asking for. `dl.symbol` correctly handles POSIX's "symbol exists with NULL value" edge case via the `dlerror()` clear-then-call-then-check idiom; raw externs end in `_raw` and return NULL/0 on failure. Sandbox interaction (POSIX): `runtime/libaether_sandbox_preload.c` already intercepts the real `dlopen` for grant-table consultation, so `dl.open` inherits sandbox permissions transparently — no grant-system extension required. Regression test compiles a tiny `probe.c` into a platform-native shared library at test time, then drives `dl.open` → two `dl.symbol` calls (asserting non-null and distinct addresses) → one `dl.symbol` for an unknown name (asserting null + non-empty error) → `dl.close`, end to end.

- **Source-location intrinsics — `__LINE__` / `__FILE__` / `__func__`** (`compiler/analysis/typechecker.c`, `compiler/codegen/codegen_expr.c`, `compiler/codegen/codegen.h`, `compiler/aetherc.c`, regression test `tests/integration/source_location/`). Closes #265 minimum-viable. Three globally-visible identifiers — registered as builtins in the typechecker (`__LINE__: int`, `__FILE__: string`, `__func__: string`), substituted at codegen time in the `AST_IDENTIFIER` branch with the AST node's literal line, source-file path, and the C99 `__func__` (which expands to the enclosing C function name and matches the Aether function name in the common case). The source-file path is plumbed through `CodeGenerator.source_file` (new field, populated by `aetherc` from `input_path` before `generate_program`). String-literal escaping in the `__FILE__` substitution handles Windows backslashes and embedded quotes safely. Useful immediately for assertions, panic messages, and log formatters at user-explicit call sites: `my_log(msg, __LINE__, __FILE__, __func__)`. Caller-site capture via default-argument substitution (the issue's recommended Capture-1 mechanism) is deferred to a follow-up since it depends on the parser learning default-argument syntax — separate scope. Regression test verifies that distinct call sites yield distinct line numbers, the file path matches the source path, and `__func__` returns the enclosing function name.

- **`std.http.client` v2 — redirect support + `response_url()`** (`std/net/aether_http.{c,h}`, `std/http/client/module.ae`, regression test `tests/integration/http_client_redirects/`). Closes #239. Two new APIs on the v2 builder: `client.set_follow_redirects(req, max_hops)` configures automatic redirect-following with a hop limit (default 0 = don't follow, preserves v1/v2 behaviour), and `client.response_url(resp)` returns the URL of the response — original URL when no redirects were followed, final-hop URL when they were. C-side externs `http_request_set_follow_redirects_raw` / `http_response_effective_url_raw` mirror the wrappers. The redirect loop in `http_send_raw` walks the chain up to `max_hops`, with three safety guards: (1) reject scheme downgrade (https origin → http target) by populating `resp->error` with a "scheme downgrade" message and stopping at the last 3xx response; (2) loop detection via a visited-URLs array — if a Location points back to a URL already seen in this chain, stop with "redirect loop detected" error; (3) strip Authorization / Cookie / Proxy-Authorization headers when the redirect target's host differs from the previous host, preventing accidental cross-host credential leakage. `effective_url` is added to the `HttpResponse` struct alongside body / headers / error and freed in `http_response_free`. Stub fallbacks added at the top of the file so non-networking builds still compile. The regression test pins the API contract (set_follow_redirects accept positive / reject negative; response_url null-safe; response_url after a failed send returns the original URL); the full multi-endpoint redirect-chain integration covering acceptance criteria cases 8/9/10/11 from the issue is deferred to a follow-up that extends `tests/integration/test_http_client_v2.ae`'s in-process server.

### Fixed

- **`ptr ==/!= "literal"` now compares string payloads instead of pointers** (`compiler/codegen/codegen_expr.c`, `tests/regression/test_string_eq_on_aether_string.ae`, `tests/regression/test_fs_atomic_rename_stat_binary.ae`). Issue #267. The codegen's string-comparison shortcut required *both* operands to be typed `string` to route through `_aether_safe_str` + strcmp; if one side was typed `ptr` (which is what `string.from_int`, `string_concat_wrapped`, `fs.read_binary`, and any extern returning `-> ptr` produce), the codegen emitted a bare pointer compare. Two pointers to different memory regions are never equal, so `string.from_int(42) != "42"` evaluated to true, `bytes == "hello"` after `fs.read_binary` evaluated to false — silent wrong answers with no diagnostic. Fix: extend the string-comparison check to also fire when one operand is `string` and the other `ptr`, while preserving `ptr == null` as a real null check (the existing null-literal exclusion stays in place). Pure ptr-vs-ptr opaque-handle comparisons still go through bare pointer eq. The previously-required workaround `string.equals(data_m, "") == 0` in `test_fs_atomic_rename_stat_binary.ae` is reverted to the natural `data_m != ""` form, so the regression coverage now tracks the actual fix. Regression test: `tests/regression/test_string_eq_on_aether_string.ae` — six cases covering `string.from_int` equal/unequal, `null` preservation, plain literal-vs-literal, `fs.read_binary` round-trip equality, and `string_concat_wrapped` equality against a literal.

- **`string.length()` on string-builder results no longer truncates at the first NUL** (`std/string/{aether_string.c,aether_string.h,module.ae}`, `compiler/codegen/codegen.c`, `tests/regression/test_string_concat_wrapped.ae`). Issue #270. New `string_concat_wrapped(a, b) -> ptr` returns a refcounted `AetherString*` (length-aware) instead of a bare `char*` (NUL-terminated, strlen-bounded). Previously, `string.length(string.concat(a, b))` on inputs containing embedded NULs (base64-decoded payloads, `fs.read_binary` output, message frames) silently truncated at the first NUL because the runtime helper `str_len` saw a non-AetherString pointer and fell through to `strlen()`. The wrapped variant preserves the stored length end-to-end. Plain `string.concat` is unchanged — still useful for ASCII-text accumulation in print / interpolation contexts where NULs aren't a concern. Companion codegen fix: the print/println helper `_aether_safe_str` now dispatches via `aether_string_data()` rather than casting to `const char*` directly, so values returned by length-bearing primitives (`string.from_int`, `string_concat_wrapped`, `fs.read_binary`, …) print their payload bytes rather than the AetherString struct's magic-header bytes. The previous casting path produced visibly garbled output for any println of an AetherString-backed string. Docs: `docs/c-interop.md` § "Aether-side string-builder return types" — table of which primitives return wrapped vs raw, with the strlen-truncation hazard called out. Regression: `tests/regression/test_string_concat_wrapped.ae` — base64-decode → wrapped concat → length check on a payload containing NULs.

- **Module merger pulls in transitive dependencies** (`compiler/aether_module.c` `module_merge_into_program`, regression test `tests/integration/transitive_module_import/`). Closes #243. When user code wrote `import std.http.client` and that module's body internally did `import std.json` and called `json.stringify(...)`, the cloned `client_post_json` body referenced `json.stringify` after the existing main merge loop — but `std.json` itself was never merged because the merger only iterated the user's direct imports. The typechecker then rejected `json.stringify` as an undefined function and forced the user to add `import std.json` themselves, leaking the internal dependency C-#include-style. Fix is a BFS pass after the existing transitive-pull-in (#171 P2) phase: walk through `mod->imports` for every directly-imported module, and merge every transitively-reachable module that hasn't been merged yet. A synthetic `AST_IMPORT_STATEMENT` is appended for each transitively-pulled module so the typechecker's namespace-registration pass picks up the new namespace without further changes. Treats transitive deps as non-selective (pulls all exported names) since the user implicitly wants the whole transitive closure to work. Short-term shape per the issue's own recommendation; the long-term per-module-sealed-scopes encapsulation fix that would block user-side qualified calls to non-direct imports is deferred to a separate follow-up. Regression test ships a tiny fixture (`lib_a/module.ae` re-exports through `lib_b.shout`, `main.ae` only imports `lib_a`) that fails pre-fix with `E0301: Undefined function 'lib_b.shout'` and passes post-fix.

- **Pinned: string interpolation accumulator-loop pattern no longer corrupts** (`tests/integration/string_interp_loop_alias/`). Issue #250. The pattern `out = "${out}${chunk}"` inside a loop was reported corrupting output (garbage prefix followed by current iteration's content) at the time the issue was filed. The underlying bug was fixed in commit `4f2e356` ("fix: free intermediate strings on reassignment in loops", April 2026) by paul-hammant — that commit added the `_heap_<var>` ownership-flag codegen pattern that wraps heap-string reassignments in a save/assign/free dance, eliminating the O(n²) leak that drove the corruption. This PR adds a regression test that pins the corrected behaviour across five shapes (plain self-aliasing loop, function-returned chunks, nested two-level accumulators, branch-local interp inside an if, 100-iteration long-run) so the fix can't silently regress. The codegen pattern: `compiler/codegen/codegen_expr.c:2390 (AST_STRING_INTERP)` emits `_aether_interp(fmt, args...)` which always `malloc`s a fresh buffer; the assignment site (`compiler/codegen/codegen_stmt.c`) emits `{ const char* _tmp_old = out; out = _aether_interp(...); if (_heap_out) free((void*)_tmp_old); _heap_out = 1; }` — RHS reads `out` via `_tmp_old` before the old buffer is freed (correct ordering). The `std/http/server/vcr/module.ae` workaround (using `string_concat` extern instead of interp) is now safe to revert.

- **Selective imports pull in transitive helpers** (`compiler/aether_module.c`, `docs/language-reference.md`, `tests/integration/transitive_import/`). Issue #171 P2. `import mathy (cube)` where `cube`'s body called a sibling helper `square` (not in the import list) used to fail with `error[E0301]: Undefined function 'mathy_square'`: the merger cloned `cube` and renamed the call to `mathy_square`, but `square` itself was skipped because it wasn't selectively named, so the cloned body had an unresolvable reference. The fix runs a fixed-point pass after the per-decl loop — any prefixed call inside an already-merged sibling that points at an unmerged helper triggers cloning of that helper, recursively. Visibility from outside the module is unchanged: `is_export_blocked` still rejects `mathy.square(...)` from user code at qualified-call sites; only the cloned caller inside the merged AST can reach the helper. Companion `import mod (*)` glob form (P1) shipped earlier; extern re-export (P3) was verified already-working on current main and needed no fix. Regression: `tests/integration/transitive_import/` — selectively imports `cube` from a module where `cube` calls `square` and `_double_it`, asserts `cube(3) == 21`. Build pre-fix failed at typecheck; post-fix builds clean and runs.

- **HTTPS on Windows now finds a CA trust anchor** (`std/net/aether_http.c`, `.github/workflows/{ci,windows}.yml`, `tests/regression/test_https_client.ae`). Issue #222. Linux + macOS have system trust stores that OpenSSL's compiled-in default paths point at; Windows MSYS2 builds have nothing — the handshake completed but cert verification failed with "certificate verify failed" because no trust anchor was configured, and the test was skipped on `OS=Windows_NT`. Now: `get_ssl_ctx()` on Windows probes a path list before falling through to the default — `SSL_CERT_FILE` env var → `<exe-root>/share/ssl/ca-bundle.crt` (next to the release `ae.exe`) → standard MSYS2 paths (`C:\msys64\mingw64\etc\ssl\certs\ca-bundle.crt`, `…\ucrt64\…`) — and uses `SSL_CTX_load_verify_locations` against the first one that loads. Both Windows CI workflows now install `mingw-w64-x86_64-ca-certificates` so the second path always resolves on the runner. The `OS=Windows_NT` skip in `tests/regression/test_https_client.ae` is removed; HTTPS verification is now expected to pass on every platform.

- **Built-in `sleep(ms)` no longer collides with libc `sleep`** (`compiler/codegen/codegen_expr.c`, `compiler/codegen/codegen_func.c`, `compiler/codegen/codegen.c`, `runtime/aether_runtime.c`, `runtime/aether_runtime.h`, `docs/c-interop.md`). Issue #233. The codegen used to inline `Sleep(...)` / `usleep(1000*...)` at every call site, which was fine in isolation — but if the user (or any module they imported) declared `extern sleep(ms: int)` to make the dependency explicit, the codegen-emitted `void sleep(int);` forward declaration conflicted with libc's `unsigned int sleep(unsigned int)` and the build failed with "conflicting types for 'sleep'". Two changes: (1) the built-in lowers to `aether_sleep_ms(...)` — a new prefixed runtime helper in `libaether.a` that handles `Sleep()` on Win32 and `usleep()` on POSIX, with a 999-second usleep cap on POSIX to stay inside the documented limit; (2) when codegen sees an `extern` declaration whose name is a well-known libc symbol (`sleep`, `exit`, `printf`, `malloc`, `read`, `write`, `time`, …), the C forward declaration is **skipped** because the libc header (already pulled in by the prelude) provides the canonical prototype — the function name is still registered for type-aware call-site casting, just not redeclared. Net effect: user code can write `extern sleep(...)` to document the dependency without breaking compilation, and the Aether-side `sleep(5)` continues to work via the runtime helper. Same prefix convention as the existing `aether_pqueue_*` rename (PR #219). Documented in `docs/c-interop.md` § "Built-in primitives and the `aether_` C-symbol convention". Regression: `tests/regression/test_extern_sleep_libc_collision.ae` (declares `extern sleep(int) -> int` and uses the Aether built-in `sleep(1)` in the same file, expects clean build and execution).

- **`std.http.client` v2 — request builder with full response access** (`std/http/client/module.ae`, `std/net/aether_http.c`, `std/net/aether_http.h`). Additive over v1; `http.get` / `post` / `put` / `delete` one-liners stay as-is. New `client` namespace: `client.request(method, url) -> ptr` (method as arbitrary string, not enum — WebDAV / DeltaV / PATCH / project-specific verbs all ride through), `client.set_header(req, name, value)`, `client.set_body(req, body, content_type)` (binary-safe via AetherString), `client.set_timeout(req, seconds)` (per-request, via `setsockopt` + non-blocking connect), `client.send_request(req) -> (resp, err)` (named `send_request` rather than `send` because `send` is reserved for actor messaging — issue #233 tracks the codegen-side fix), `client.response_status(resp) -> int` (caller drives non-2xx interpretation rather than collapsing to `"http error"`), `client.response_body(resp)` (binary-safe), `client.response_header(resp, name)` (case-insensitive lookup), `client.response_headers(resp)` (raw block), `client.response_free(resp)`, `client.request_free(req)`. v2.1 sugar wrappers ship alongside: `client.get_with_headers(url, header_pairs)` and `client.post_with_status(url, body, ct)`. Unblocks the Aether port of Apache Subversion to drop its hand-written `ae/ra/shim.c` (~1,400 lines of libcurl + buffer-grow + header callback + auth helper) and switch to `std.http`. v3 streaming bodies remain intentionally deferred per the design plan (`http-client-improvement-plan.md`). Test surface: `tests/integration/test_http_client_v2.ae` — seven cases covering header round-trip, response-header lookup (case-insensitive), 200/404/403/500 status discrimination, binary-body POST with embedded NUL, timeout enforcement (server sleeps 2s, client times out at 0.5s), transport failure (refused connection), and v1-wrapper compatibility check. Self-contained: spins up an in-process Aether HTTP echo server in an actor, then drives client calls against it on the main thread — no shell wrapper, no subprocess, no port dance. Plus `tests/regression/test_https_client.ae` (HTTPS DNS-failure path) and `tests/syntax/test_http_client_*.ae` (3 files covering accessor + tuple-wrapper compile shape). Surface documented in `std/http/README.md`.

- **`std.http.server.vcr` — Servirtium record/replay for HTTP tests** (new module under `std/http/server/vcr/`). Aether's implementation of [Servirtium](https://servirtium.dev), the cross-language record/replay framework. Tapes are canonical Servirtium markdown — interoperable with the Java/Kotlin/Python/Go implementations, so a tape recorded by any of them is replayable here. The metaphor: a VCR is the device, a tape is the medium, `record` / `replay` are the operations. Surface: `vcr.load(tape_path, port)` + `vcr.eject(server)` + `vcr.tape_length()` (replay), `vcr.record(method, path, status, content_type, body)` + `vcr.flush(tape_path)` (record), `vcr.flush_or_check(tape_path)` (Servirtium step 4 — re-record byte-diff check against committed tape, `.actual` sibling left on disk for inspection on mismatch), `vcr.note(title, body)` (step 9 — per-interaction `[Note]` block, record-only, parser tolerant), `vcr.redact(field, pattern, replacement)` + `vcr.FIELD_PATH` / `vcr.FIELD_RESPONSE_BODY` + `vcr.clear_redactions()` (step 8 — flush-time scrubbing of secrets, in-memory capture untouched), `vcr.last_error()` + `vcr.clear_last_error()` (step 10 — strict request-shape matching surfaces mismatches via tearDown), `vcr.static_content(mount_path, fs_dir)` + `vcr.clear_static_content()` (step 11 — bypass-the-tape directory mount for Selenium/Cypress assets), `vcr.emphasize_http_verbs()` + `vcr.indent_code_blocks()` + `vcr.clear_format_options()` (step 12 — opt-in alternative markdown forms; playback tolerates either regardless). Servirtium roadmap steps 1-12 shipped (steps 13-16 are package distribution / proxy mode / compatibility suite / cross-impl interop — outlined in `std/http/README.md`). Header normalization: incoming + recorded request headers are sorted case-insensitively and Host stripped before comparison. Strict matching is opt-in per field — empty recorded `req_headers` / `req_body` blocks mean "no constraint" so existing tapes (and tapes from other implementations that didn't capture those fields) keep working unchanged. Test surface: 6 VCR-flavored integration tests in `tests/integration/test_vcr_*.ae` covering every step's behavior (redactions, notes, strict matching against the spec's `broken_recordings/` hostile fixtures, static mounts with mime-type detection + traversal rejection, format toggles); 3 climate tests in `contrib/climate_http_tests/` exercising live network → record → replay → re-record-check round-trip against the WorldBank climate API mirror at servirtium.github.io with the canonical 5-endpoint tape committed alongside. Implementation: tape parser + emitter + format normalization are pure Aether (`std/http/server/vcr/module.ae`); only the storage globals, the dispatcher (function pointer to `http_server_add_route`), and HttpRequest field reads stay in C (`std/http/server/vcr/aether_vcr.c`). Companion FFI ergonomics issues filed during the C→Aether port: `@extern("symbol")` annotation (#234) and Aether-functions-as-C-callbacks (#235); five smaller compiler/runtime issues collected in `compiler_notes_from_vcr_port.md`. Buildkite step `:cloud: contrib/climate_http_tests (VCR replay)` runs the offline-safe tests on every CI cycle.

- **`contrib.sqlite` v2 — prepared statements + parameter binding** (`contrib/sqlite/aether_sqlite.c`, `contrib/sqlite/module.ae`). Additive over v1; the open/close/exec/query/cell surface stays as-is. New: `sqlite.prepare(db, sql) -> (stmt, err)`, `bind_int` / `bind_text` (uses `SQLITE_TRANSIENT` so callers can drop their buffers immediately) / `bind_blob` (binary-safe via explicit length, AetherString-aware) / `bind_i64(stmt, idx, hi, lo)` (split for Aether's not-guaranteed-64-bit `int`) / `bind_null`, `step(stmt, db) -> (rc, err)` returning `SQLITE_ROW` (100) or `SQLITE_DONE` (101) on success, `column_int` / `column_text` / `column_i64` (returns `(hi, lo)` matching the bind side) / `column_blob` (returns `(bytes, length, err)` via the same TLS split-accessor pattern std.fs and std.zlib use, so embedded NULs survive), `reset` / `finalize`, plus `changes(db)` and `errmsg(db)`. Constants `SQLITE_OK` / `SQLITE_ROW` / `SQLITE_DONE` exported from `module.ae` so callers don't redeclare them. Unblocks the Aether port of Apache Subversion to drop its hand-written `ae/ffi/sqlite/shim.c` (~150 lines C deleted at the downstream side). Streaming row iteration is still v3 / out of scope; the v2 primitives here are sufficient to write a `while step() == SQLITE_ROW { … }` loop in pure Aether. Regression test: `tests/integration/sqlite_prepared/` (8 cases — bind_int + bind_text round-trip, bind_i64 across the 32-bit boundary at 2³³+7, bind_blob preserving a 4-byte NUL-bearing payload through full bind→insert→read cycle, prepare-failure surfaces non-empty `errmsg(db)`, `SQLITE_DONE` vs `SQLITE_ROW` discrimination, `reset` + re-bind + re-step on one statement, `changes(db)` after multi-row UPDATE, finalize-then-fresh-prepare). Same pkg-config probe + SKIP path as the v1 test for runners without `libsqlite3-dev`.

- **`aether_string_data()` / `aether_string_length()` — public FFI accessors for `-> string` returns** (`std/string/aether_string.h`, `std/string/aether_string.c`, `docs/c-interop.md`). A C shim that types an Aether `-> string` extern as `extern const char* foo(...)` and then runs `memcpy(dst, result, n)` / `strlen(result)` reads into the 24-byte AetherString struct header (magic + ref count + lengths + data pointer), not the payload — silent memory-corruption hazard, no build warning, no runtime diagnostic. Caught by the subversion-aether port (`new_defects.md` §1), where two shims produced garbage output for weeks before a clean `make` rebuild exposed it. `aether_string_data(s)` returns the real byte pointer; `aether_string_length(s)` returns the exact length (binary-safe). Both accept either an `AetherString*` (ref-counted, the common case) or a raw `char*` (legacy TLS-arena externs), are safe on NULL, and have an `aether_`-prefixed name matching the ABI-mangled export style so intent is obvious at the call site. Downstream shims that were open-coding the unwrap (`#define AETHER_STRING_MAGIC` + cast, pattern replicated across rep_store_shim.c / pristine_shim.c / fs_fs/shim.c / merge3_shim.c in the svn port) can delete the boilerplate and `#include "aether_string.h"`. The docs/c-interop.md section "Consuming `-> string` Returns From C" documents the ABI contract so new FFI authors avoid the trap. Regression: `tests/integration/aether_string_ffi_unwrap/` — writes a 10-byte file with an embedded NUL at offset 3 through a C shim, round-trips through `fs.read_binary` → Aether string → C shim unwrap → byte-exact comparison.
- **Menu bar + context menus in `contrib/aether_ui`** — native `HMENU`-backed on Win32 (`CreateMenu` / `CreatePopupMenu` / `SetMenu` / `TrackPopupMenu`), with matching GTK4 / AppKit entry points declared in [`aether_ui_backend.h`](contrib/aether_ui/aether_ui_backend.h). DSL: `menu_bar()`, `menu("File")`, `menu_item(m, "Open") callback { … }`, `menu_separator(m)`, `menu_bar_add(bar, m)`, `attach_menu_bar(app, bar)`, `menu_popup(m, anchor)`. Menu command IDs live at `AE_MENU_ID_BASE = 0x8000` to avoid colliding with button control IDs. See [`contrib/aether_ui/example_menu.ae`](contrib/aether_ui/example_menu.ae).
- **Grid layout** — 2-D container complementing stacks; columns align across rows so forms line up exactly. Backed by a new `AetherUIGrid` window class on Win32; GTK4 will map to `GtkGrid`, AppKit to `NSGridView`. DSL: `root_grid(cols, row_spacing, col_spacing)` + `grid_place(grid, child, row, col, row_span, col_span)`. See [`contrib/aether_ui/example_grid.ae`](contrib/aether_ui/example_grid.ae) for a login-form example.
- **Driver `/widget/{id}/children` and `/screenshot` endpoints in the shared server**. New `widget_children` and `screenshot_png` hooks on `AetherDriverHooks`; backends that don't implement them return `501 Not Implemented` rather than an obscure 404. Win32 implements both — `/children` walks `GetWindow(..., GW_HWNDNEXT)`, `/screenshot` uses `BitBlt` + GDI+ `GdipSaveImageToStream` to produce an in-memory PNG. [`tests/test_driver.sh`](contrib/aether_ui/tests/test_driver.sh) gains assertions for both endpoints (PNG magic-byte check).
- **Examples**: [`example_menu.ae`](contrib/aether_ui/example_menu.ae) demonstrates File/Edit/Help menu bars wired to reactive state; [`example_grid.ae`](contrib/aether_ui/example_grid.ae) shows a 2-column login form with a spanning button.

### Performance

- **O(1) reverse widget lookup on Win32**. `handle_for_hwnd()` was a linear scan called from every `WM_COMMAND`, every stack/grid layout pass, and every `/widgets` JSON emit — so a 500-widget app paid 500 compares per button click. Replaced with an open-addressed Fibonacci-hash table (linear probing, load factor < 0.5). Amortized O(1). Measurable on the existing 500-widget stress benchmark.

### Fixed

- **`out = 0` followed by `out = <ptr-expr>` in a `-> ptr` function no longer emits `int out = 0`** (`compiler/analysis/type_inference.c`, `widen_ptr_assigned_locals`). Prior to this fix, inferring `out`'s type from its `0` initializer locked it as `TYPE_INT`; the generated C was `int out = 0; ... out = ptr_fn();` which truncates the 64-bit pointer to its low 32 bits before the implicit zero-extension at return — the caller then dereferences garbage and segfaults. Caught by the subversion-aether port (`new_defects.md` §3); killed 17 downstream tests that used `svn commit`. The workaround (`out = null` — which correctly infers `ptr`) is preserved; the compiler now also handles the `0` literal case. New phase-7 pass in `infer_all_types` walks each ptr-returning function body; when a local declared with literal-0 initializer is reassigned a ptr-typed value in the same block (or a nested block), the declaration's type and the symbol-table entry both widen to `TYPE_PTR`. Conservative: only fires inside ptr-returning functions, only for literal `0` (not arbitrary int expressions), and only when a concrete ptr assignment is observed — int accumulators in int-returning functions are untouched. Regression: `tests/integration/ptr_return_int_zero_inference/` — two cases (ptr-returning fn widens to `void*`, int-returning fn keeps `int`), both inspect the generated C directly since the symptom is a C-level truncation invisible to Aether-side typechecking.
- **Reserved-keyword identifier errors now carry a matching `help:` line** (`compiler/parser/parser.c`, `expect_token` + `parse_block`). Previously an `E0100` error for `'state' is a reserved keyword` defaulted to the generic "check for missing parentheses, braces, or keywords" hint, which pointed users at a parse problem that wasn't there. Caught by the subversion-aether port (`new_defects.md` §2) — real cost was ~ten minutes of guess-and-check to figure out `state` was reserved rather than a malformed parameter list. Both parser sites that detect the pattern now emit the error through `aether_error_full` with a reserved-keyword-specific hint ("rename to '<name>_' or another identifier"), bypassing the generic suggestion table. Downstream errors that cascade from the same source location (e.g. "Expected RIGHT_PAREN, got STATE") keep their own hints — the fix is strictly to the primary reserved-keyword diagnostic. Regression: extended `tests/integration/reserved_keyword_error/test_reserved_keyword_error.sh` — now extracts the first error block and asserts its `help:` line mentions "rename" and NOT "missing parentheses"; adds a `state_as_param.ae` case to cover the exact scenario from the defect report (previously only `message` was tested).
- **`make test-asan` / `test-valgrind` / `test-memory` link successfully** (`Makefile`). All three targets used `COMPILER_SRC` when linking the test runner, which pulls in `compiler/aetherc.c` — and that file's `main()` collides with the test harness's own `main()` in `tests/runtime/test_main.c`. The link failed immediately with `multiple definition of 'main'`. The `test-fast` target at line ~275 already had the right shape (`COMPILER_LIB_SRC` — same list minus `aetherc.c`, same pattern `build/aether-lsp` uses at line ~698); the three broken targets now use it too. Also added `$(COLLECTIONS_SRC)` + `-Istd/collections` to match `test-fast`, which needed them after the collections refactor. Verified: `test-asan` passes 191/191 under ASan + LSan with zero leaks; `test-memory` passes 191/191. `test-valgrind` was blocked by the same link bug — will run end-to-end once Valgrind is available on the build host. This makes the "Running tests with ASan / Valgrind" checklist item in `CONTRIBUTING.md` actually usable for contributors.
- **Canvas command-buffer accumulated forever on Win32**. `aether_ui_canvas_begin_path_impl` was append-only; animated canvases leaked ~16 bytes/cmd per frame (hundreds of MB/hr for a 60 Hz scene). `begin_path` now resets `cmd_count` to 0 so each frame's command stream starts fresh. Matches the contract the name implies.
- **Shared AetherUIDriver HTTP test server** ([`contrib/aether_ui/aether_ui_test_server.c`](contrib/aether_ui/aether_ui_test_server.c) / [`.h`](contrib/aether_ui/aether_ui_test_server.h)). Pulls the socket accept loop, HTTP parser, URL router, JSON emitter, sealed-widget bookkeeping, and cross-platform socket setup (winsock2 on `_WIN32`, POSIX sockets elsewhere) out of each backend into a single file. Each backend provides a small `AetherDriverHooks` struct (widget introspection + a `dispatch_action` hook that marshals mutations onto the UI thread) and calls `aether_ui_test_server_start(port, &hooks)`. Win32 backend now uses this shared server; GTK4 and AppKit backends still have their inlined drivers and will migrate in a follow-up. Also added a comprehensive tutorial at [`docs/aether-ui-testing.md`](docs/aether-ui-testing.md) covering the HTTP API, widget sealing, headless mode, and test-suite examples in Bash, Python, and JavaScript.
- **`AETHER_UI_HEADLESS=1` parity across all three backends**. GTK4 skips `gtk_window_present` (realizes the window but never maps it), AppKit skips `makeKeyAndOrderFront:` + `activateIgnoringOtherApps:`, Win32 uses `SW_HIDE` instead of `SW_SHOW`. In every case the event loop still pumps and the test server still responds, but no window is ever presented. Required for unattended CI runners (GitHub Actions `windows-latest`, `ubuntu-latest` without X display + xvfb, Docker without a virtual display).
- **PNG / JPEG / GIF / TIFF image loading on Win32**. `aether_ui_image_create` now loads any format GDI+ supports via `GdipCreateBitmapFromFile` → `GdipCreateHBITMAPFromBitmap`, falling back to plain BMP via `LoadImageW` when GDI+ isn't available. Previously limited to BMP only.
- **Keyboard navigation on Win32**. The Win32 backend's message loop now wraps dispatch with `IsDialogMessageW`, giving every WS_TABSTOP control Tab / Shift+Tab focus traversal, Enter to activate the default button, and Esc to cancel — for free. Matches native dialog behavior.
- **Native Win32 backend for `contrib/aether_ui`** ([`contrib/aether_ui/aether_ui_win32.c`](contrib/aether_ui/aether_ui_win32.c)). The widget DSL previously only ran on Linux (GTK4) and macOS (AppKit); Windows apps had to be built manually or skipped. This PR lands a full native Win32 backend — USER32 + GDI+ + Common Controls + COMDLG32 + DWMAPI + Winsock2, no MSYS2-GTK runtime, no WebView2, no XAML/WinUI. Same ABI as the other two backends (the shared header was renamed `aether_ui_gtk4.h` → [`aether_ui_backend.h`](contrib/aether_ui/aether_ui_backend.h)); the Aether DSL layer in [`module.ae`](contrib/aether_ui/module.ae) is untouched. Covers: app lifecycle with per-monitor DPI v2 and `WM_DPICHANGED` handling, immersive dark mode via `DwmSetWindowAttribute`, a custom `AetherUIStack` window class implementing VStack/HStack/ZStack layout (spacers, padding, alignment, flex distribution), every core widget (Text, Button, TextField, SecureField, Toggle, Slider, Picker, ProgressBar, Textarea, ScrollView, Canvas with GDI offscreen replay, Image), styling (bg color, gradient, corner radius via `SetWindowRgn`, custom fonts, tooltips, opacity via WS_EX_LAYERED on top-level windows), reactive state bindings, system services (MessageBoxW, GetOpenFileNameW, clipboard as CF_UNICODETEXT, SetTimer, ShellExecuteW, dark-mode registry query), opacity animation via WM_TIMER, events (click, hover, double-click), and the AetherUIDriver HTTP test server rewired for winsock2 with UI-thread marshalling via a hidden AE_WM_DRIVER message host. UTF-8 ↔ UTF-16 boundary helpers. Cross-platform `make contrib-aether-ui-check` runs a new C-level widget suite ([`tests/test_widgets.c`](contrib/aether_ui/tests/test_widgets.c) — 40 assertions covering roundtrips, unicode, deep nesting, 500-widget stress) on every platform, plus the HTTP driver integration test ([`tests/test_driver.sh`](contrib/aether_ui/tests/test_driver.sh) — 14 assertions including banner-seal 403 enforcement) on Windows; `make ci` runs it as step [10/10]. `make benchmark-aether-ui` emits a CSV microbenchmark set ([`benchmarks/bench_widgets.c`](contrib/aether_ui/benchmarks/bench_widgets.c)) covering widget creation, reactive fan-out, layout, canvas, and UTF-8 roundtrip costs. CI-safe: `AETHER_UI_HEADLESS=1` suppresses window visibility (the message loop still pumps and the driver still responds) so `windows-latest` runners can exercise the full backend without any window ever appearing on the runner's desktop. Platform docs: [`docs/aether-ui-windows.md`](docs/aether-ui-windows.md).

## [0.90.0]

### Added

- **`aether.toml` `extra_sources` now accepts multi-line arrays** (`tools/ae.c`, `get_extra_sources_for_bin`). Both forms parse the same way now:

  ```toml
  extra_sources = ["a.c", "b.c", "c.c"]
  ```

  and

  ```toml
  extra_sources = [
      "a.c",
      "b.c",
      "c.c"
  ]
  ```

  Previously the parser only handled single-line arrays — projects with enough shims would either squash everything onto one ever-growing line (and eventually hit the assembly-side buffer limit, see earlier fix this release) or edit-and-re-squash for every new shim. The continuation-line handling is permissive: ignores whitespace and commas, strips `#` comments, and keeps reading until the closing `]`. Empty lines and mid-array comment lines are tolerated. Malformed arrays (unterminated `[` at EOF) fall through without blocking the build. Regression test: `tests/integration/toml_extra_sources_multiline/` builds 12 shims each on its own continuation line (17-line TOML total) and asserts every shim links + runs. Complements the single-line `test_long_line.sh` (30 shims / 1.3 KiB) and `test_assembly_buf.sh` (60 shims / 2.7 KiB); all three share the same probe shape.

- **`std.os` — `now_utc_iso8601()`** (`std/os/aether_os.c`, `std/os/aether_os.h`, `std/os/module.ae`). Current UTC time as a 20-char ISO-8601 timestamp string `"YYYY-MM-DDThh:mm:ssZ"`. Thread-safe (`gmtime_r` on POSIX, `gmtime_s` on Windows). Returns `""` on clock / format failure — never null. The C side is named `os_now_utc_iso8601_raw` (matching the `os_exec_raw` / `os_run_capture_raw` pattern); the Aether-side wrapper `os.now_utc_iso8601()` lives in `module.ae` and does the null-detach via `string_concat`. The raw-suffix dance is load-bearing: Aether mangles a bare `now_utc_iso8601` wrapper in `std.os` to the C symbol `os_now_utc_iso8601`, which would infinitely recurse if the extern had the same name (caught in testing — the first draft crashed on first call). Shape is deliberately "the one thing downstream code needs" — rev-blob date fields, JSON response timestamps, log-line prefixes. Sub-second precision, timezone offsets, and strftime-style format flags are out of scope for v1; additive variants (`now_utc_iso8601_ms`, `format_time`) can land without disturbing this one. Regression test: `tests/regression/test_os_now_utc_iso8601.ae` — 4 cases that can't assert an exact value (time marches on) but do check length=20, punctuation in the right slots (`-`, `-`, `T`, `:`, `:`, `Z`), every digit slot contains a digit, and plausible numeric ranges for year / month / day / hour / minute / second.

- **`std.string` — `from_char` + `index_of_from`** (`std/string/aether_string.c`, `std/string/aether_string.h`, `std/string/module.ae`). Two additions aimed at code walking packed-string record formats. `string.from_char(code) -> string` produces a 1-byte AetherString from a byte code (0..255) via `string_new_with_length` — fills the gap where callers want to emit a known single-byte marker (`\x01`, `\x02`) without routing through a NUL-terminated literal. Length is always 1, including for code=0 (which would otherwise produce an empty string through `string_new`). Codes above 255 are masked to the low byte (`code & 0xff`). `string.index_of_from(s, needle, start) -> int` is `index_of` with a starting offset — returns the *absolute* offset of the hit (not relative to `start`), or -1 on miss. Negative `start` clamps to 0; `start` past the length returns -1. Saves the `substring(s, start, length) + index_of(tail, needle) + start` allocation dance that packed-record walkers were doing per step. While I was there I also fixed `string_index_of`'s needle parameter to be data-aware — previously it ran `strlen` on the needle, which worked for `char*` literals but read garbage past the header when callers passed an `AetherString*` (e.g. `from_char(2)` as a separator). Both functions now call `str_len` / `str_data` on both haystack and needle, matching the binary-safety promise the rest of `std.string` makes. Regression test: `tests/regression/test_string_from_char_and_index_of_from.ae` — 9 cases covering 1-byte builds with code=0 / code masking, `index_of_from` scanning past an earlier hit, start clamping, and round-trip through `from_char` + `index_of` on a packed string.

- **`std.zlib` — one-shot deflate/inflate** (`std/zlib/module.ae`, `std/zlib/aether_zlib.c`, `std/zlib/aether_zlib.h`, Makefile, `tools/ae.c`). Two pure functions that operate on in-memory byte buffers: `zlib.deflate(data, length, level) -> (bytes, length, err)` and `zlib.inflate(data, length) -> (bytes, length, err)`, plus `zlib.available() -> int` (1 when the toolchain was built with zlib detected, 0 otherwise — lets callers distinguish "no backend" from runtime failure, same pattern as `std.cryptography` uses for OpenSSL). Output matches the `fs.read_binary` shape — a length-aware AetherString plus explicit byte count, so embedded NULs survive the round-trip (the unwrap helper inside the extern impl mirrors the one added in v0.86.0 for `fs.write_binary`). Under the hood `deflate` uses `compress2` with `compressBound` sizing; `inflate` uses streaming `inflate()` with a geometric-grow output buffer so callers don't need to know the decompressed size in advance. Auto-detects zlib via pkg-config (same pattern as OpenSSL); when absent the wrappers return `("", 0, "zlib unavailable")` rather than crashing. Streaming APIs and gzip-framed (RFC 1952) variants are deliberately out of scope for v1 — additive future work. Level accepts 0..9 (or -1 for default); out-of-range values are clamped to default. Regression test: `tests/integration/zlib_roundtrip/` (6 cases — text round-trip with compression-ratio check, empty-stream round-trip, empty-input error, garbage-input error, 32-byte binary payload with NULs scattered every 4th byte, level-clamp), with a SKIP path for platforms without zlib.

### Fixed

- **`aether.toml` `extra_sources` assembly-side buffer bumped 2 KiB → 8 KiB + now warns on truncation** (`tools/ae.c`). The v0.85 fix bumped the *parse-side* `fgets` buffer to 8 KiB so long TOML lines don't get chopped while reading; that unblocked most projects. What v0.85 didn't touch was the *assembly-side* `char toml_extra[2048]` buffer in `cmd_run` and `cmd_build` that holds the concatenated filenames right before they're spliced into the gcc command line, plus the matching `char extra_files[2048]` that merges `--extra` CLI args with the TOML list. A downstream project (Subversion port, `ae/svnserver/main.ae` with 63 `extra_sources`, line length ~2.3 KiB) hit the lower limit: `strncat` silently truncated mid-filename and gcc errored with `cannot find ae/svnserver/handler_copy_generat: No such file or directory` — no hint that TOML parsing was the real culprit. Both buffers now 8 KiB, matching the parse-side fgets. `get_extra_sources_for_bin` also returns an `int` truncation flag so callers can emit a clear warning (`aether.toml [[bin]] extra_sources for '<file>' exceeded 8 KiB; tail entries were dropped`) instead of silent data loss. The gcc-command `snprintf` call also checks its return for buffer overflow and warns. Regression test: `tests/integration/toml_extra_sources_assembly_buf/` builds a 60-shim project with a ~2.7 KiB `extra_sources` line and asserts every shim links + runs (sum-of-returns check).

- **Windows CI + release matrix now installs `mingw-w64-x86_64-zlib` + `mingw-w64-x86_64-openssl`** (`.github/workflows/ci.yml`, `.github/workflows/release.yml`). Previously the Windows legs only pulled `mingw-w64-x86_64-gcc make bc` via MSYS2, so `AETHER_HAS_ZLIB` and `AETHER_HAS_OPENSSL` went undefined on those builds — meaning the Windows binary release silently shipped with `std.zlib`, `std.cryptography`, and `std.net` TLS all in "unavailable" stub mode. Downstream Windows users who imported those modules got a runtime error instead of working code. Adding the two MSYS2 packages (and `pkg-config` so auto-detection works) closes the gap. Linux/macOS runners already have `libz` / `libssl` ambient and were unaffected.

## [0.89.0]

### Added

- **`contrib/sqlite/` — SQLite bindings in contrib/** (`contrib/sqlite/module.ae`, `contrib/sqlite/aether_sqlite.c`, `contrib/sqlite/README.md`). Thin veneer over the system `libsqlite3`. v1 API: `sqlite.open(path) -> (db, err)`, `sqlite.close(db) -> err`, `sqlite.exec(db, sql) -> err` for DDL and no-row DML, `sqlite.query(db, sql) -> (rs, err)` for SELECT plus `sqlite.row_count`, `sqlite.col_count`, `sqlite.col_name`, `sqlite.cell`, `sqlite.free` for result-set inspection. Lands in `contrib/` per the rubric in `docs/stdlib-vs-contrib.md` — SQLite's C API is opinionated enough that anchoring one shape in `std/` would constrain future contributors, and the 4 MiB amalgamation makes it a noticeable weight tax for projects that don't use a database. Callers opt in via `aether.toml` `[build] link_flags = "-lsqlite3"` + `extra_sources = ["contrib/sqlite/aether_sqlite.c"]`; no toolchain-level auto-detection. Parameter binding (`?1`, `:name`) is deliberately out of scope for v1 — users needing it wait for an additive API or drop down to `sqlite3_prepare_v2` / `bind` / `step` directly. Rows are materialised up-front with a 100 000-row hard cap; streaming is a separate future API. Regression test: `tests/integration/sqlite_roundtrip/` (7 cases — open `:memory:`, CREATE TABLE, 3×INSERT, SELECT shape, column names, cell values, close). The test probes `pkg-config --exists sqlite3` (with a gcc-link fallback) and SKIPs gracefully when `libsqlite3` isn't installed on the CI runner — matches `CONTRIBUTING.md` §"Coding for portability" pattern #2.

## [0.88.0]

### Added

- **`std.cryptography` — SHA-1 and SHA-256 hex digests** (`std/cryptography/module.ae`, `std/cryptography/aether_cryptography.c`, `std/cryptography/aether_cryptography.h`). Pure one-shot functions: `cryptography.sha1_hex(data, length) -> (digest, err)` and `cryptography.sha256_hex(data, length) -> (digest, err)`. Bytes in, lowercase-hex digest out. Binary-safe via an explicit byte length — embedded NULs survive; `length=0` hashes the empty string. Thin veneer over OpenSSL's EVP API (already linked for `std.net`'s TLS support). When the toolchain is built without OpenSSL the wrappers return `("", "openssl unavailable")` rather than crashing. SHA-1 is included for interop with legacy formats (Git object IDs, Subversion rep-stores, HMAC-SHA1 fixtures) — prefer SHA-256 for new work. Streaming, HMAC, KDFs, and symmetric ciphers are out of scope for v1; see `docs/stdlib-vs-contrib.md` for the "one obvious shape" criterion. Regression test: `tests/integration/cryptography_sha/` (6 cases — sha256(""), sha256("abc"), sha1("abc"), sha256/sha1 of a 10-byte AetherString with NUL at offset 3, and length=0 short-circuit).

### Fixed

- **`CHANGELOG.md` merge-conflict marker cleanup on `## [0.86.0]`**. A manual merge of the two v0.86.0 PRs left `<<<<<<<` / `=======` / `>>>>>>>` markers in the committed file on `main`. Resolved by keeping both blocks under the same version header.

## [0.86.0]

### Added

- **`std.fs.write_binary(path, data, length) -> string`** (`std/fs/module.ae`, `std/fs/aether_fs.c`, `std/fs/aether_fs.h`). Non-atomic binary write — `fopen("wb")` + `fwrite(length bytes)` + `fclose`. Completes the binary-safe I/O pair with `fs.read_binary` (v0.82.0): caller passes an explicit byte length, so payloads with embedded NULs survive the write. Cheaper than `fs.write_atomic` when a partial file on crash is acceptable (scratch writes, caches, any destination not load-bearing for another process). Regression test: `tests/integration/fs_write_binary_nul/` — seeds a 10-byte file with NUL at offset 3 via a C shim, reads it via `fs.read_binary`, writes it back via `fs.write_binary`, reads it a second time, and verifies every byte round-trips.

### Fixed

- **AetherString payloads now survive `fs.write_atomic` and `fs.write_binary`** (`std/fs/aether_fs.c`). Both extern impls took `const char* data` and passed it straight to `fwrite`, but callers hand in whatever pointer the Aether variable holds — which for `fs.read_binary`'s return value (and anything built via `string_new_with_length`) is an `AetherString*` struct pointer, not the payload. The first 40 bytes written to disk were the AetherString header (magic `0xAE57C0DE`, refcount, length, capacity, data-ptr), not the intended bytes. `fs.write_atomic` carried this latent bug since v0.82.0 — existing callers masked it by passing string literals (plain `char*`), which happen to point at the data directly, so the test suite didn't catch it. Fixed by adding a shared `fs_unwrap_bytes(data, length, &out_len)` helper that dispatches on `is_aether_string()` and unwraps when needed. Regression test covers both `write_binary` and `write_atomic` with a 10-byte NUL-embedded payload whose round-trip exposes the header-leak.

### Changed

- **`docs/stdlib-vs-contrib.md` — placement rubric for new modules** (plus cross-link from `CONTRIBUTING.md`). Captures the four-question rubric (is it expected in a stdlib; does it have one obvious API shape; are deps minimal; is the surface stable and small) for deciding whether a new module belongs in `std/` or `contrib/`. Applies the rubric to the in-flight Zero-C LOC plan: `std.cryptography.sha1/sha256` and `std.zlib` go in `std/` (OpenSSL + zlib are already ambient, both have one obvious shape), `sqlite` goes in `contrib/` (4 MiB amalgamation, opinionated API surface), the HTTP client split stays inside `std.net` / `std.http`. Documents only — no code changes.

## [0.85.0]

### Fixed

- **`ae build` now handles long `extra_sources` lines in `aether.toml`** (`tools/ae.c`). `get_extra_sources_for_bin` read the TOML one line at a time with a 1 KiB fixed buffer; a single-line `extra_sources = ["a.c", ...]` that exceeded 1023 characters got silently truncated by `fgets`, dropping every entry past the cutoff from the link command. Failure mode was loud but the diagnostic was bad — linker errored with "undefined reference to `shim_23_value`" and no hint that the TOML parser dropped the entry. Downstream projects hit this at ~33 comma-separated filenames. Bumped the buffer to 8 KiB (fits ~250 average-length filenames); projects that exceed even that should switch to multi-line TOML arrays once the parser learns them (separate work). Regression test: `tests/integration/toml_extra_sources_long_line/` builds a synthetic 30-shim project with a ~1300-byte single-line `extra_sources` array and asserts every shim links and runs.

## [0.84.0]

### Added

- **`std.string`: `strip_prefix`, `copy`** (`std/string/module.ae`). Two convenience wrappers for URL-parsing / handler patterns. `strip_prefix(s, prefix) -> (rest, stripped)` removes a prefix if present; cleaner than manual `starts_with` + `substring` length arithmetic. `copy(s) -> string` is a named alias for `string_concat(s, "")` that gives the "snapshot a borrowed TLS buffer" idiom a discoverable name. Both are pure-Aether wrappers — no new externs, built on `string_starts_with` / `string_substring` / `string_concat`. Regression test: `tests/regression/test_string_slicing_wrappers.ae` (8 cases). Complements the existing `string.starts_with` / `string.ends_with` / `string.index_of` / `string.substring` that already handle prefix/suffix/slicing. A planned third wrapper, `split_once(s, sep) -> (head, tail, err)`, was not included: the typechecker's tuple-unification path produced mis-typed storage when a `(string, string, string)` tuple shape collided with an existing `(string, int, string)` shape from `std.fs`. For now, callers compose `string.index_of` + `string.substring` directly.

## [0.83.0]

### Added

- **`std.intarr` — fixed-size packed int buffer** (`std/collections/aether_intarr.c`, `std/collections/aether_collections.h`, `std/intarr/module.ae`). Closes the "int arrays via FFI" gap: `std.list` stores `void*`-boxed items, so packing ints through it costs an allocation per entry and an extra pointer chase per read. For DP tables (blame LCS, edit-distance, string alignment), flat int-keyed lookup buffers, and other hot int-indexed patterns, `std.intarr` provides a heap-allocated `int[size]` with direct index ops, bulk fill, and no amortised growth. Three access tiers: `intarr.get(arr, i)` / `intarr.set(arr, i, v)` are Go-style wrappers with bounds-checked `(value, err)` returns; `intarr_get_raw` / `intarr_set_raw` are safe-on-OOB externs (return 0 / no-op); `intarr_get_unchecked` / `intarr_set_unchecked` skip the bounds check for validated-index inner loops. Multi-dimensional access is caller-computed (`row * N + col` on a flat buffer). Regression test: `tests/regression/test_intarr.ae` (10 cases — alloc + size, zero-init, round-trip, new_filled, fill reset, wrapper error shapes, 5×4 DP-table pattern, negative-size rejection, zero-size legality). Module aliased as `std.intarr` following the `std.list` / `std.map` pattern.

## [0.82.0]

### Fixed

- **`fs.read_binary` now preserves embedded NUL bytes** (`std/fs/module.ae`). Fixes a bug introduced in PR #200 (v0.80.0): the wrapper used `string_concat("", raw)` to copy the TLS buffer into an owned Aether string, but `string_concat` is strlen-based — any binary payload with an embedded NUL truncated at the first zero byte, even though the wrapper reported the full length. Swapped in `string_new_with_length(raw, n)` which memcpy's exactly n bytes and records the length in the AetherString header. Callers now see `string.length(content) == n` and `string.char_at(content, i)` returns the i-th byte including NULs. The extern signature is unchanged; the returned `ptr` is now a proper length-aware AetherString. Regression test: `tests/integration/fs_read_binary_nul/` (10-byte file with NUL at offset 3; checks reported length, string.length, and byte-by-byte content). Known footgun worth flagging separately: `data != "literal"` on an AetherString return now compiles to `strcmp(void*, void*)` and reads past the magic header; callers should use `string.equals(data, "literal") != 1`.

## [0.81.0]

### Fixed

- **`std.http` request/response accessors now tolerate externally-constructed pointers** (`std/net/aether_http_server.c`). Host C code that owns the HTTP dispatch loop (parses the wire format, builds `HttpRequest` / `HttpServerResponse` structs, calls an Aether handler to populate them) previously crashed on the first `http_response_set_header` or `http_response_json` because `http_response_set_header` strdup'd into a NULL `header_keys` array — `http_response_create` allocates those lazily, but a C-allocated response has them zero. Fix: lazy-allocate header_keys/values on first `set_header` when NULL. Plus defensive NULL-guards across the request-side accessors (`http_get_header`, `http_get_query_param`, `http_request_method`/`path`/`body`/`query`) so a partially-populated request from a dispatch shim doesn't NULL-deref on the unset fields. No tagging, no runtime type check — the pointers just work as long as the struct layout matches `std/net/aether_http_server.h`. Regression test: `tests/integration/http_external_ptr/` — 9 cases covering a C-allocated request fed to Aether accessors and a bare calloc'd response mutated by Aether setters then read back in C.

## [0.80.0]

### Added

- **`--emit=lib --with=fs,net,os` — capability opt-in for host-owned library builds** (`compiler/aetherc.c`, `tools/ae.c`). The v1 capability-empty default posture is unchanged: `--emit=lib` still rejects `import std.fs` / `std.net` / `std.http` / `std.tcp` / `std.os` by default. Projects that ARE the host — compiling `.ae` + handwritten C into one binary, rather than embedding Aether as an untrusted user script — opt into specific categories with `--with=fs` / `--with=net` / `--with=os`; multiple can be comma-separated (`--with=fs,os`). Unknown capability names are a hard error rather than a silent no-op. The categories mirror the existing banned-import groupings (three buckets), chosen coarsely enough that opting in is an auditable event in a project's build invocation. Rationale captured in `docs/emit-lib.md`'s new "Opting in" subsection: safe for systems code where `.ae` files are first-party and version-controlled like `.c` files; NOT safe for plugin/DSL scenarios where the library could execute untrusted Aether. Regression test: `tests/integration/emit_lib_with_capability/` (13 cases: opt-in accepts the matching category, still rejects others; multi-capability works; unknown capability rejected; no-op without `--emit=lib`). The existing `tests/integration/emit_lib_banned/` test still passes unchanged.

- **`string.from_long(value: long) -> string`** (`std/string/aether_string.c`, `std/string/aether_string.h`, `std/string/module.ae`). Sibling of `string.from_int` that preserves the full 64-bit range. Callers formatting byte counts, file sizes, revision numbers, or other values that can exceed INT_MAX reach for this instead of truncating through `string.from_int`. Two lines of runtime code (uses `long long` + `%lld` internally to cover Aether's `long` on every platform, including MSVC where C `long` is 32-bit). Regression test: `tests/regression/test_string_from_long.ae`.

- **`std.fs` follow-on APIs: `write_atomic`, `rename`, `file_stat`, `read_binary`** (`std/fs/aether_fs.c`, `std/fs/aether_fs.h`, `std/fs/module.ae`). Four additions paired with `--with=fs`. `fs.write_atomic(path, data, length)` stages into `<path>.tmp.<pid>.<n>`, fsyncs on POSIX, then renames — crash-safe and binary-safe. `fs.rename(from, to)` wraps POSIX `rename(2)` so callers can compose `write_atomic` + `rename` for stage-then-publish patterns. `fs.file_stat(path)` returns `(kind, size, mtime, err)` from one `lstat(2)` — replacing the `file_exists + fs_is_symlink + size + file_mtime` quartet when callers need more than one field; kind encoding: 1=file, 2=dir, 3=symlink, 4=other. `fs.read_binary(path)` preserves embedded NULs and reports an explicit byte length. Aether wrappers use split try/get accessor pairs backed by TLS so callers don't pass C out-parameters from Aether. Regression test: `tests/regression/test_fs_atomic_rename_stat_binary.ae` (12 cases covering round-trips, missing-source errors, empty-file edges, zero-on-error shape, and a stage-then-rename composition). `fs.rmdir` wasn't needed — `dir_delete_raw` already calls POSIX `rmdir(2)` directly so `fs.delete_dir` already has "remove only if empty" semantics.

## [0.79.0]

### Added

- **`std.json` object-key iteration** (`std/json/aether_json.h`, `std/json/aether_json.c`, `std/json/module.ae`). Four new C accessors — `json_object_size_raw`, `json_object_key_at`, `json_object_key_len_at`, `json_object_value_at` — expose the parallel-array storage that `JsonObjBlock` already keeps. Two Aether wrappers — `json.object_size(obj)` and `json.object_entry(obj, i)` with Go-style `(key, value, err)` return — cover the common loop case. Iteration order is insertion order across both parse and builder paths (documented explicitly in `docs/json-parser-design.md`). Fills a gap vs. every other general-purpose JSON library (cJSON, nlohmann::json, serde_json, Python `json`) so callers can enumerate prop maps, path-keyed dicts, headers, and other JSON where the keys themselves are user data. Regression test: `tests/regression/test_json_object_iter.ae` — empty object, single key, parse-order preservation, builder-order preservation, negative/OOB index, non-object rejection, nested iteration, 32-key stress through `obj_reserve`'s growth path.

## [0.78.0]

### Changed

- **`std.json` parser: clean-room rewrite + fast-double + SIMD string scan** (`std/json/aether_json.c`, `std/json/aether_json.h`). Three layered improvements over the previous byte-by-byte parser.

  *Generation 2 — arena rewrite.* One bump-pointer arena per document, `JsonValue` tree with flat `JsonValue**` children and parallel `char**`/`uint32_t*`/`JsonValue**` for objects, 256-entry character-class lookup table for hot-path dispatch, Bjoern Hoehrmann's public-domain UTF-8 DFA, integer fast path that skips `strtod` for pure digits, two-phase string parser that pre-scans the closing quote for exact buffer sizing.

  *Generation 3 — fast-double + SIMD.* Numbers with ≤15 significant digits and |exponent| ≤ 22 take a `POW10` fast-double path: accumulate the int + frac mantissa, apply one multiply/divide by the pow10 lookup, cast to double. Falls back to `strtod` for edge cases needing IEEE-754-correct rounding (16+ digits, huge exponents, denormals). The string fast-loop calls a compile-time-dispatched `scan_str_safe()`: SSE2 on `__SSE2__`, NEON on `__ARM_NEON && __aarch64__`, scalar LUT fallback always compiled in for WASM / embedded / non-x86-non-arm. All three produce byte-identical results — SIMD is purely additive acceleration.

  API preserved exactly. New length-taking variant `json_parse_raw_n(data, len)` for non-null-terminated input. Benchmark harness (`benchmarks/json/`) committed with corpus generator and optional reference-implementation comparison mode (`make bench-json` / `make bench-json-compare`). Methodology in `benchmarks/json/baseline.md`; absolute throughput figures are intentionally not committed — run the harness on the machine you care about. Design rationale, structural-headroom discussion, and "when to change what" table in `docs/json-parser-design.md`.

- **`JsonValue` shrunk 48 → 32 bytes; object backing consolidated behind one `JsonObjBlock*`** (`std/json/aether_json.c`). The `obj` union variant used to carry three parallel array pointers (`keys`, `key_lens`, `values`) plus count/capacity inline — 32 bytes, dominating the union. Replaced with `{ JsonObjBlock* blk; uint32_t count; uint32_t capacity; }` — 16 bytes, same width as `arr` and `str`. The block itself holds the three arrays on the arena and is allocated lazily on first reserve. Net effect: `sizeof(JsonValue)` drops from 48 to 32, so two values share a 64-byte cache line on every mainstream target. Hot accessors (`json_object_get_raw`, stringify, deep-copy, heap-free) updated to reach through `blk->`; empty-object fast-outs added where count is zero. No API change. Verified against JSONTestSuite conformance (318/318), ASan+UBSan on the full bench corpus, MinGW cross-compile, cooperative scheduler.

- **`JSON_CONTAINER_INITIAL_CAP` bumped 4 → 8** (`std/json/aether_json.c`). Initial capacity for both arrays and objects. Realistic JSON objects cluster in the 5–15-key range; starting at 4 meant two doubling cycles for the common case, and the wasted arena pointers for genuinely tiny objects round off into the chunk allocator anyway. Applied to `arr_reserve` and `obj_reserve` via the shared constant so the two paths can't drift.

- **Stdlib safety hardening.** Fixed null-deref-after-malloc in `string_new_with_length`, `string_concat`, `string_split`, `list_new`, `map_new`, `io_file_info_raw`, `path_join`, `path_dirname`, `dir_list_raw`, and the HTTP server request parser (`std/net/aether_http_server.c`'s method/path/query allocations each with a cleanup-of-already-allocated-peers path on failure). Fixed `size_t` underflow in `string_split` when the delimiter is longer than the input (previously looped past end-of-buffer). Added proper partial-cleanup helper for OOM during split so failure doesn't leak the pieces allocated so far. `string_concat` gained `size_t`-overflow guard before adding 1 for the null terminator.

- **Stdlib performance: amortised growth + cache-friendly hashmap.** `std/net/aether_http.c` response-body accumulator switched from realloc-per-recv (O(N²) on large responses) to capacity-doubling starting at 16 KB. `std/fs/aether_fs.c` directory listing (both Windows `FindFirstFileA` and POSIX `readdir` branches) same change — capacity-doubling starting at 16 entries. `std/os/aether_os.c` exec-capture buffers bumped from 256B/1 KB to 4 KB, with defensive `while` growth instead of single-doubling `if`. `std/collections/aether_collections.c` hashmap: `HashMapEntry` now caches the DJB2 hash and key length so resizes don't rewalk the key, and `key_equals` prefilters on length before `memcmp`; load-factor check is pure integer arithmetic (`size * 4 > capacity * 3`) instead of per-insert floating-point division. `string_concat` has empty-input fast paths that skip the full concat when one side is 0 bytes.

### Added

- **JSONTestSuite conformance corpus** (`tests/conformance/json/`). Imported the canonical 318-file RFC 8259 test corpus (MIT licensed, attribution in `tests/conformance/json/LICENSE`). `make test-json-conformance` gates CI on 95/95 `y_*` must-accept + 188/188 `n_*` must-reject; records `i_*` implementation-defined outcomes without gating.

- **JSON hardening targets** (`Makefile`). `make test-json-asan` builds the parser under `-fsanitize=address,undefined` and parses every corpus fixture — clean on all 6 (including 10 MB `large.json`). `make test-json-valgrind` runs under Valgrind on platforms where it's installed, skipping otherwise. Both targets are parser-focused so they don't drag in the whole stdlib.

- **`json_parse_raw_n(const char* data, size_t len)`** (`std/json/aether_json.h`). Length-taking parse variant. Accepts inputs without a trailing null byte — required for memory-mapped files, sub-slices, and fuzz harnesses.

- **Targeted regression tests for new code paths.** `tests/regression/test_json_number_precision.ae` covers every boundary in the fast-double parser (int64-max, 15/16/17 significant digits, `1e22` / `1e23` exponent split, near-max/denormal doubles, strict RFC rejection still holds on fast-path numbers). `tests/regression/test_json_simd_string.ae` stresses SIMD/scalar parity: lengths 0/1/15/16/17/31/32/33/100 bracket the 16-byte block boundaries, escapes at positions 0/14/16/19 force SIMD-to-scalar handoff mid-string, non-ASCII UTF-8 (2-byte café, 4-byte emoji) at boundaries exercises DFA fallback. `tests/regression/test_collections_hashmap_edge.ae` extended with a 256-key length-varying collision storm that verifies the cached-hash + length-prefilter chain doesn't silently overwrite or lose entries through rehash boundaries.

- **`std/json/aether_json.c` supporting tests.** Five regression files committed: `test_json_edge_cases.ae` (escape chars, empty strings, deeply-nested-but-legal), `test_json_unicode.ae` (BMP + surrogate pairs), `test_json_rfc_rejection.ae` (every `n_*` from the RFC), `test_json_position_errors.ae` (line:col accuracy across multi-line inputs), `test_json_error_tuples.ae` (Go-style wrapper error shapes).

- **Pattern-matching in examples.** `examples/stdlib/json-demo.ae` rewritten with `match` expressions on `json.type()` for variant dispatch (null/bool/number/string/array/object) — one arm per JsonType, no if-else chains. The `render(v)` function composes cleanly when called recursively on array elements.

- **`docs/json-parser-design.md`** — new design-rationale doc covering arena memory model, character classification LUT, UTF-8 DFA, SIMD string fast-loop (with its SSE2/NEON/scalar dispatch), number-parser three-path structure, container representation, `JsonValue` sizing (the 32-byte two-per-cache-line layout), error reporting, depth limit, thread safety, and the structural-headroom section (tape representation as deferred work with a concrete dual-representation design, full-SIMD parsing as a follow-on of tape).

## [0.77.0]

### Fixed

- **Sibling bare `{ }` blocks sharing a local name now compile cleanly** (`compiler/codegen/codegen_stmt.c`). Two blocks each declaring `src` compiled to C where the first block emitted `const char* src = ...;` but the second emitted a bare `src = ...;` reassignment — C scope had already dropped the first `src` at the first `}`, so gcc errored with `'src' undeclared`. Codegen's `declared_var_count` wasn't saved/restored across the bare AST_BLOCK path even though the if/else path already did this correctly. Fix: mirror the if/else save/restore pattern for AST_BLOCK. Regression test: `tests/syntax/test_sibling_block_scope.ae`.

## [0.76.0]

### Fixed

- **Two function calls on either side of `!=`/`==` inside an `if`/`while`/`for`/`match` condition no longer miscompile** (`compiler/parser/parser.c`, `compiler/parser/parser.h`). The parser greedily consumed the `{` that starts an if/while body as a trailing-closure argument attached to the rightmost call in the condition. An innocuous pattern like `while i < n { if f(a) != f(b) { return 0 }; i = i + 1 }` compiled to C where `return 0` was silently dropped, `i = i + 1` moved inside the `if`, and the loop became infinite — no compile error. New `parser->in_condition` flag is raised while parsing an if/while condition, a paren-less `match` subject, and a range-based for's end-expression; inside it, `func(args) { ... }` is parsed without the trailing block. Explicit trailing-block forms (`callback { }`, `|params| { }`) still work because they can't plausibly be mistaken for a statement body. Regression test: `tests/syntax/test_call_on_both_sides_of_comparison.ae`.

- **Reserved-keyword identifier errors now name the keyword and suggest a rename** (`compiler/parser/parser.c`). `extern f(code: int, message: string)` produced `Expected IDENTIFIER, got MESSAGE_KEYWORD`, and `message = "hello"` produced `Expected statement in block` — both forcing the user to consult the lexer to figure out which name collided. New `token_is_reserved_keyword()` helper detects an alphanumeric token whose lexer type isn't `TOKEN_IDENTIFIER`; `expect_token` (identifier sites) and `parse_block` (statement-head site) now emit `'message' is a reserved keyword and cannot be used as an identifier; rename it (e.g. 'message_' or 'msg')`. Regression test: `tests/integration/reserved_keyword_error/` (extern parameter name and local variable name, both with `message`).

- **`aether.toml` `[build] cflags` now apply to `ae run` too** (`tools/ae.c`). `user_cflags` was gated behind `optimize`, so `ae build` picked up the release build's cflags but `ae run` silently dropped them. That broke any project whose extern C shims relied on a `-D<feature>` flag or a `-Wno-<warning>` suppression — the `ae build` succeeded and `ae run` failed on the same source. Removed the gate; `get_cflags()` applies on every path. Regression test: `tests/integration/ae_run_cflags/` uses a shim.c with a `#error` that fires unless the TOML's `-DAE_CFLAGS_TEST_MARKER=1` reaches the compile.

## [0.75.0]

### Fixed

- **`name(params) -> ReturnType { body }` on user functions now typechecks correctly** (`compiler/parser/parser.c`). The parser treated `-> Foo {` after a function signature as the start of an Erlang-style arrow body (`-> expr` or `-> { stmts }`), so `g(a: int, b: int) -> int { return a + b }` tried to parse `int { return a + b }` as a single-expression arrow body, choked on the type keyword, and cascaded into parse errors; the typechecker then reported `E0200: expects 1 argument(s), got 2` because the parser had already lost parameter state. Disambiguation at the `->`: if the next token is a type keyword (int/int64/float/bool/string/message/ptr/actor_ref) or an identifier followed by `{` that is NOT a struct-literal `field: value` head, parse it as a return-type annotation and enter the traditional `{ ... }` block body. Otherwise fall through to the existing Erlang-style arrow-body paths. Mirrors the `extern f(...) -> ReturnType` convention so user functions don't have to switch to the `name(params): Type` form. Regression test: `tests/syntax/test_typed_return_annotation.ae` (2/3/5-param functions with `-> int`, 1-param with `-> int`, non-int return with `-> string`).

- **Erlang-style catch-all clauses now bind pattern variables and chain with `else`** (`compiler/codegen/codegen_func.c`). Multi-clause functions ending in a variable-pattern clause — `fib(0) -> 0; fib(1) -> 1; fib(n) -> fib(n-1) + fib(n-2)` — compiled to C that referenced `n` without a declaration and without an `else` separating the variable-pattern body from the prior literal-match bodies. GCC rejected with `'n' undeclared` and, even if `n` had been bound, earlier clauses would have fallen through. Fix in `generate_clause_condition`'s catch-all path: wrap the body in `} else { ... }` when prior clauses had conditions, and emit `int n = _arg0;` bindings for each `AST_PATTERN_VARIABLE` in the param list (the binding code the conditional path already had). Regression test: `tests/syntax/test_erlang_variable_pattern.ae` (fib with literal+literal+variable, add with all-variable, classify with literal+literal+variable returning string).

## [0.74.0]

### Changed

- **Docs audit: "Limitations" / "Caveats" / "Restrictions" sections replaced across the tree.** Sixteen such sections across nine docs files (`c-embedding.md`, `closures-and-lifetimes.md`, `containment-sandbox.md`, `emit-lib.md`, `module-system-design.md`, `named-args-and-select.md`, `type-annotation-style-guide.md`, `type-inference-guide.md`) were rewritten rather than listed. Design boundaries are now stated positively as part of the main narrative ("Host-process boundary", "Scope boundaries", "Shared-interpreter behavior", "Nesting rules", "When explicit types are required", "Type contract", "Closure patterns and workarounds", "Interception surface"). Items that represent pending engineering work (unintercepted syscalls, transitive package deps + lockfile + `ae publish`, polymorphism/type-classes/higher-rank types, `select()` type-inference propagation) moved into new sections in `docs/next-steps.md`. `docs/closures-and-lifetimes.md` was reframed from a limitations doc into a patterns+workarounds doc with per-item "proper fix" notes pointing at the roadmap. Verification: `grep -rniE "^## .*Limitations|^### .*Limitations|^## .*Caveats|^### .*Caveats|^## .*Restrictions|^### .*Restrictions|^## .*Gotchas|^### .*Gotchas" --include="*.md" .` returns empty.

- **Docs audit: specific benchmark numbers scrubbed.** Hardcoded comparison figures, cycle-count sample outputs, percentage-improvement claims, and concrete request-per-second numbers removed from `README.md`, `CONTRIBUTING.md`, `docs/language-reference.md`, `docs/profiling-guide.md`, `docs/build-system.md`, `docs/containment-sandbox.md`, `docs/stdlib-api.md`, `docs/stdlib-reference.md`, `docs/next-steps.md`, and historical entries in this file under `[0.31.0]` and `[0.55.0]`. Replacements use unit-ful descriptions of *what* is measured and how to reproduce it: profiling-guide sample output uses placeholder tokens with an explanatory note; the regression-check script in that doc reads a user-tunable `$TOLERANCE` against a saved baseline rather than a hardcoded threshold; cost-of-a-run descriptions in `docs/build-system.md` describe the dominant components (aetherc front-end, gcc compile+link, OS Gatekeeper first-run pause on macOS) without timings. A grep sweep for `M msg/sec` / `cycles/op` / `ns/op` / `Nx faster` / `req/s` / `Nx throughput` / `N% faster/slower/improvement` patterns across all markdown files returns empty.

- **`docs/next-steps.md` extended** with items relocated from the deleted "Limitations" sections: a new top-level `## Sandbox` section containing *Interception surface expansion* (catalogue of syscalls LD_PRELOAD doesn't see; points to the containment-sandbox doc's "Interception surface" table), a new top-level `## Type system` section containing *Type inference propagation through `select()`* (describes the string-typing gap that string interpolation currently works around) and *Polymorphism, higher-rank types, type classes* (surface-level description of monomorphic-only inference, not a scheduled item). The existing "Package Registry" bullet was extended to include transitive dependency resolution, lock file integrity, `ae update`, and `ae publish`.

- **Inline prose cleanup.** `docs/containment-sandbox.md` reshaped: the `## Limitations` section became `## Scope boundaries` with its sub-section `### Other limitations` merged in as `### Other boundaries`; `### Nestable restriction` became `### Nesting rules`; `### Known unintercepted libc functions and syscalls` became `### Interception surface — what LD_PRELOAD sees and what it doesn't` with a forward reference to the next-steps item; `### Shared-interpreter caveats` became `### Shared-interpreter behavior`; the "Nested maps: Not supported" framing was rewritten to state the flat-by-design decision up front. `docs/module-system-design.md` "Not yet supported" bullet list renamed to "On the module-system roadmap (language-level)".

- **Cross-reference cleanup.** The `[0.72.0]` entry that describes the new `docs/containment-sandbox.md` section was updated to reference the renamed heading ("Shared-interpreter behavior"). `contrib/host/TODO.md` references to the old heading were updated. `docs/closures-and-lifetimes.md` introduction paragraph that referred to the old "Known limitations" heading was rewritten to point at the new "Closure patterns and workarounds" section.

- **Doc-file rename.** `docs/closure-lifetime-bugs.md` → `docs/closures-and-lifetimes.md`. File title updated from "Closure Environment Lifetime Bugs" to "Closures and Environment Lifetimes"; historical bug-list framing reworked to read as mechanism documentation with a per-pattern workaround section. All five cross-references in this file were updated to the new path.

- **Docs audit: code-comment layer swept too.** The constraint pair (no limitations-style sections, no specific benchmark numbers) now applies to C/header comments and runtime printf output, not only markdown. Scope of this pass: four "Known limitation"-style comments in C files (`runtime/aether_config.c`, `runtime/libaether_sandbox_preload.c`, `contrib/host/perl/aether_host_perl.c`, `tests/syntax/test_closure_reassign_leaks_env.ae`) reworded to state the technical content positively; runtime printf blocks that emitted specific throughput tiers (`runtime/aether_runtime.c` "Expected Performance" block, `runtime/utils/aether_cpu_detect.c` SIMD recommendation block, `runtime/config/aether_optimization_config.c` Tier-1 listing, plus two example-program summary blocks and one bench-program prediction line) replaced with tier-name / component descriptions that point at the actual benchmark programs for measurement; top-of-file design comments with hardcoded speedup ratios across the actors / memory / scheduler / config headers and the `Makefile` PGO and build-help sections rewritten to describe what each optimisation does without committing to specific ratios; `benchmarks/baseline.json` string descriptions in its `optimizations` and `notes` keys reworded (its structured numeric measurement fields are unchanged — those are data, not prose). Full-tree verification: grep across `*.md`, `*.ae`, `*.c`, `*.h`, `Makefile`, `*.json` for quantitative performance-claim patterns returns empty except for `tests/runtime/test_scheduler.c:372` (factual statement about Valgrind's overhead, not an Aether claim) and `benchmarks/baseline.json` structured measurement fields.

- **Minor drift cleanup.** `docs/architecture.md` module-resolution example: the stale `import std.collections.HashMap` (no such module; the actual stdlib file is `aether_hashmap.c` behind `std.collections` / `std.map` / `import std.collections`) was replaced with the real form and the resolver steps updated to match the merger's current behaviour. `contrib/aether_ui/README.md` dead cross-reference to `aether_ui_conversion_plan.md` (file doesn't exist) was removed. Snippet fixes for renamed stdlib entry points: `docs/type-inference-guide.md` `tcp_connect(...)` / `tcp_listen(80)` → `_raw` forms; `docs/language-reference.md` `tcp_send(conn, msg)` → `tcp_send_raw(conn, msg)`; `docs/stdlib-reference.md` `net.http_get(url)` → raw + Go-style pair; `docs/next-steps.md` `tcp_connect()` → `tcp.connect()` for consistency with the neighbouring `http.get()` / `file.read()` / `sleep()` list items. `docs/api/*.html` (auto-generated API reference) refreshed by `make ci` to include entries for `http_response_status_code` / `http_response_body_str` / `http_response_headers_str` / `os_execv` / `os_which` that were missing from the committed HTML.

- **Tutorial prose reframed.** `CONTRIBUTING.md` `## Common Pitfalls` → `## Code review heuristics` with an intro explaining the BAD/GOOD/BETTER checklist form; `docs/getting-started.md` `### Common Pitfalls` → `### Quick tips when you hit trouble`, each bullet rewritten from warning-form ("Forgetting to rebuild…") to prescriptive-form ("Rebuild from scratch if anything looks stale…"). `docs/module-system-design.md` `## Future` section's intro line "Features not yet implemented:" → "Work planned on top of the current module system:". `docs/performance-benchmarks.md` and `benchmarks/cross-language/README.md` CV% interpretation notes gained an explicit line clarifying that the <5% / 5–15% / >15% bands are general statistical stability thresholds, not Aether-specific targets. `docs/next-steps.md` "33% size overhead" for base64 rephrased as "4:3 expansion ratio as a matter of the encoding's arithmetic" so it's clear the number is math, not a benchmark.

## [0.73.0]

### Added

- **`contrib/host/go/` — eighth language host module.** Separate-subprocess host (same containment model as `contrib/host/aether/`: `fork` + `execvp` under `LD_PRELOAD=libaether_sandbox.so`, grants serialized to `shm_open` segments named `/aether_host_go_<pid>`). Two usage modes: `go.run_script_sandboxed(perms, path)` shells out to `go run script.go` (wider grant surface because the `go` toolchain itself runs under the sandbox), and `go.run_sandboxed(perms, binary)` runs a pre-built binary under tight grants. Also ships `_with_map` variants that share a `uint64_t` map token with the child for bidirectional key/value passing. Six externs in `module.ae`; C-side in `aether_host_go.{h,c}` with Windows stub fallbacks. README documents the `$GO_BIN` override, the Linux-vs-macOS SIP caveat, and when to prefer `run_sandboxed` over `run_script_sandboxed`. End-to-end demo in `examples/host-go-demo.ae` (two grant profiles: wide-open vs narrow env+tmpfs, with the narrow profile expected to fail the `go run` toolchain's requirements on Linux).

- **`make contrib-host-check` target + `ci-contrib-host` workflow job.** Closes the "no CI coverage for contrib/host" gap. The target runs in two phases: phase one syntax-checks every bridge (`js`, `lua`, `perl`, `python`, `ruby`, `tcl`, `go`) in stub mode (no dev libs required) so missing headers never break the tree; phase two delegates to `tests/scripts/contrib_host_demos.sh`, which probes for each language's headers/libraries, emits `-I/-L/-l` fragments when available, and runs the per-language demo if present. Absent dev libs exit with a distinguishable "skip" code (3) rather than failure. The `ci-contrib-host` GitHub Actions job installs `liblua5.3-dev python3-dev ruby-dev libperl-dev duktape-dev tcl-dev golang-go` on `ubuntu-22.04` and invokes the target after `make compiler ae stdlib`.

- **`contrib/host/js/` — JS bindings for `writeFile` and `exec`.** Duktape doesn't go through libc for its own I/O (it has no built-in filesystem or process APIs), so the bridge explicitly exposes each capability as a native binding that routes through the in-process sandbox checker. Two new bindings join the existing `print`, `env`, `readFile`, `fileExists` set: `writeFile(path, content)` checks `"fs_write"`/path and returns a boolean success flag (creates or truncates); `exec(cmd)` checks `"exec"`/cmd, shells via `system()`, and returns the exit code unwrapped through `WEXITSTATUS` (or `-1` when denied).

- **`contrib/host/java/module.ae` + `grant_jvm_runtime()` helper.** The JVM needs ~29 grants before any application code runs (kernel/procfs probes for container detection, `/usr/lib/jvm/*` + `/lib*/` + `/etc/java-*` + `/etc/alternatives/*` for libs, `/etc/ssl/*` + `/etc/pki/*` + `/etc/ca-certificates/*` for trust stores, `/etc/localtime` + `/usr/share/zoneinfo/*` + `/usr/share/locale/*` for locale/TZ, and the `JAVA_HOME` / `JAVA_TOOL_OPTIONS` / `CLASSPATH` / `LANG` / `LC_*` / `TZ` env vars). Grant paths captured empirically via `strace java -version` on Corretto 24 (Debian) and Temurin 21 (Ubuntu). Callers `import contrib.host.java` and invoke `java.grant_jvm_runtime()` inside a `sandbox() { … }` block alongside application-specific grants. README extended with a usage snippet.

- **`contrib/host/tcl/` — seventh embedded-language host module.** Tcl 8.5+ joins the existing six hosts (aether, js, lua, perl, python, ruby). Four files under the new directory follow the lua template: `module.ae` declares `tcl_run_sandboxed` / `tcl_run` / `tcl_init` / `tcl_finalize` externs; `aether_host_tcl.{h,c}` implements the bridge against `libtcl` (single lazy-initialized `Tcl_Interp*`, `Tcl_Eval` for script execution, self-contained permission stack, native `aether_map_get`/`aether_map_put` Tcl commands registered via `Tcl_CreateObjCommand` for the `run_sandboxed_with_map` variant, stub-mode fallback under `#else` when `AETHER_HAS_TCL` isn't set). `README.md` documents prerequisites (`apt install tcl-dev` / `brew install tcl-tk`) and the canonical `aether.toml` cflags/link_flags recipe. Tcl is now listed in the "Available modules" table and the host-module matrix in `docs/containment-sandbox.md`.

- **`examples/host-tcl-demo.ae`** — end-to-end demo exercising the tcl host under two different grant profiles (worker with env/fs_read/exec grants, and a minimal sandbox with only env). Verified: compiles to C, links with libtcl + libaether, runs on macOS with system Tcl 8.5.

- **`examples/host-python-demo.ae`** — replaces the TODO reference to an untrackable `lazy-evaluation` branch with a fresh demo matching the current stdlib + sandbox API. Uses heredoc strings for multi-line Python snippets. Demonstrates the `ctypes.CDLL(None).getenv` workaround for CPython's cached `os.environ` (flagged in the host-module matrix in `docs/containment-sandbox.md`). Verified: compiles to C, links with libpython3.12 + libaether, runs on macOS.

### Fixed

- **All six contrib/host bridges are now self-contained.** Previously, `aether_host_{python,lua,perl,ruby,js,tcl}.c` declared `extern void* _aether_ctx_stack[]` and `extern int _aether_ctx_depth`, referencing symbols that the compiler emits as `static` in its preamble (`compiler/codegen/codegen.c:1213-1214`). The extern references had no visible definition at link time; host bridges could not link end-to-end for any language. Each bridge now owns a private `<lang>_perms_stack[64]` + `<lang>_perms_depth` pair, pushes its perms locally before installing its checker via the already-public `_aether_sandbox_checker`, and pops on exit. The only cross-translation-unit symbol required is `_aether_sandbox_checker` from `runtime/aether_sandbox.c`. Verified end-to-end: `examples/host-tcl-demo.ae` builds against libaether + libtcl and runs on macOS with system Tcl 8.5. Same pattern applied to the existing lua/python/perl/ruby/js hosts (aether host unaffected — it uses separate-process `spawn_sandboxed` rather than in-process embedding).

- **Import path drift across all six host modules.** Every host README and the `module.ae` header comments said `import std.host.<lang>`, but Aether's stdlib resolver (`compiler/aether_module.c:435`) only probes under `std/` while these modules live under `contrib/host/`. The working form — `import contrib.host.<lang>` — is now documented consistently across all `contrib/host/*/module.ae` headers and READMEs, matching what `docs/containment-sandbox.md` already got right. No compiler change needed; this was pure documentation drift.

- **Pre-existing Perl stub typo.** In `AETHER_HAS_PERL`-undefined mode, `aether_host_perl.c`'s fallback stubs called `perl_init()` — the real libperl symbol, unavailable in stub mode. Corrected to call the stub's own `aether_perl_init()`. Makes the perl host syntax-check clean in both modes.

- **IPv4-mapped IPv6 addresses now match across grant/resource sides.** A grant for `10.0.0.1` matches a resource reported as `::ffff:10.0.0.1` (and vice versa). The normalization strips the `::ffff:` prefix from both sides of `pattern_match` before any glob comparison. Applied in eight places: all six in-process bridges (`aether_host_{js,lua,perl,python,ruby,tcl}.c`), the `LD_PRELOAD` checker (`runtime/libaether_sandbox_preload.c`), and the Java-side `AetherGrantChecker.patternMatch`. The prefix literal is safe for non-TCP categories because it doesn't appear in filesystem paths, env var names, or exec command strings.

- **`clock_ns()` codegen no longer emits preprocessor directives inside an expression.** The builtin previously generated `loop_start = \n#if AETHER_GCC_COMPAT\n({ ... })\n#else\n_aether_clock_ns()\n#endif\n;` at every call site. That is only valid C when the `#` column is forced to 0 of its own line, so any surrounding context that didn't flush a newline first (macro expansion, some nested codegen paths) collapsed the assignment to `loop_start =` followed by empty lines, which the C compiler then reported as an undeclared identifier on the LHS with nothing on the RHS. The statement-expression branch was a micro-optimization that saved one function call on GCC/Clang; `_aether_clock_ns()` is already defined in the preamble with the correct per-platform `clock_gettime` / `QueryPerformanceCounter` / freestanding variants, and the helper is now marked `static inline` so the optimizer has the same freedom. Every `clock_ns()` call site now emits a plain `_aether_clock_ns()`, which is robust in every expression position. Surfaced by a report where a `while (cond) { t = clock_ns() ... }` inside an actor receive arm failed to compile the generated C, even though the same pattern inside a `for` body worked.

- **`ActorBase.dead` field plumbed through codegen.** The runtime struct gained `atomic_int dead` alongside `timeout_ns` and `last_activity_ns` in the scheduler panic/recovery work. The codegen actor struct in `compiler/codegen/codegen_actor.c` emitted the two timeout fields but not `dead`, leaving a layout mismatch so that scheduler reads of `actor->dead` read garbage (typically a user state field) and silently skipped message processing as if the actor had crashed. Codegen now emits `atomic_int dead;` after `last_activity_ns` and initializes it to 0 alongside the other atomic fields in the spawn path. Eight `.ae` integration tests that exercise multi-actor messaging (`test_actor_communication`, `test_msg_basic`, `test_coop_chain`, etc.) were failing as a result; all pass after the fix.

- **Scheduler test stub structs updated to mirror current `ActorBase`.** `tests/runtime/test_scheduler.c` and `test_scheduler_stress.c` manually duplicate the `ActorBase` layout field-by-field (five struct definitions total) with a `MUST match ActorBase layout exactly` comment. The duplicates missed the new `timeout_ns` / `last_activity_ns` / `dead` fields so the scheduler's reads past `step_lock` clobbered the tests' own counter fields, producing `expected 100, got 0` in `Scheduler message ordering`. Added the three fields to all five structs; `make test` now reports 191/191.

- **Windows portability of the panic/recover runtime.** `runtime/actors/aether_panic.h` used `sigjmp_buf`, `sigsetjmp`, and `siglongjmp` unconditionally, which MinGW and the `make ci-windows` cross-compile don't provide. Added a portability shim in the header: `aether_sigjmp_buf` and the macros `AETHER_SIGSETJMP` / `AETHER_SIGLONGJMP` expand to the POSIX `sigjmp_buf` family on Linux and macOS and to plain `jmp_buf` / `setjmp` / `longjmp` on Windows, where the signal-mask semantics of the POSIX variants aren't meaningful. The signal-handler installer (`aether_panic_install_signal_handlers`) guards its `sigaction` / `SA_SIGINFO` block under `#ifndef _WIN32` with a no-op stub for Win32 (SEH is a separate design). Callers updated: `runtime/scheduler/multicore_scheduler.c`, `compiler/codegen/codegen_stmt.c` (try/catch codegen), `runtime/actors/aether_panic.c`. `make ci-windows` passes 70/70 examples after the fix (previously 0/70).

- **`make examples` excludes `host-*-demo.ae` files.** The host language bridge demos (`host-go-demo.ae`, `host-python-demo.ae`, `host-tcl-demo.ae`) need bridge-specific C sources and `-l<lang>` link flags that the plain `examples` target doesn't know about. They are built and run by `make contrib-host-check` instead, which probes for dev libs and emits the right flags. The `examples` target filter now skips `host-.*-demo.ae` the same way it already skips `/lib/`, `/packages/`, and `/embedded-java/`.

- **`tests/integration/closure_nested_return` gcc invocation gains `-Iruntime/actors`.** The codegen preamble now emits `#include "aether_panic.h"`, which lives under `runtime/actors/`. The standalone gcc command the shell test uses to validate `-Werror=return-type` on the generated C previously passed only `-Iruntime -Istd -Istd/io`. The header wasn't found and the test failed for reasons unrelated to the return-type regression it guards against. Added `-Iruntime/actors` to match what `ae build` already passes.

- **`tests/runtime/test_worksteal_race.c` `WStealActor` struct updated to mirror current `ActorBase`.** The three work-steal race tests (`Work-steal no message loss`, `Work-steal re-route under migration`, `Work-steal atomic assigned_core read`) manually duplicate the `ActorBase` layout field-by-field. The duplicate was still missing the `timeout_ns` / `last_activity_ns` / `dead` fields the scheduler added, so `scheduler_send_remote`'s `atomic_load(&actor->dead)` read 16 bytes past the end of the 1616-byte allocation. AddressSanitizer reported it as a `heap-buffer-overflow` (seen in the `memory-checks` CI job); without ASAN the reads silently returned garbage, which produced intermittent `Work-steal no message loss` / `Work-steal re-route under migration` failures on CI runners due to messages being dropped when the garbage happened to be non-zero. Added the three fields; all four work-steal tests pass under ASAN with zero errors.

- **Emscripten/wasm32 cannot encode label-address dispatch tables; route wasm through the switch-case path.** The actor message dispatch in `compiler/codegen/codegen_actor.c` emits two implementations gated on `#if AETHER_GCC_COMPAT`: a computed-goto dispatch using GCC's "labels as values" (`&&handle_FOO` stored in a `void* dispatch_table[256]`) and a `switch`-case fallback that targets MSVC. Emscripten's clang identifies as GCC-compatible, so it took the computed-goto path, which wasm cannot represent: storing a function-local label address in a data-section array requires a code-section relocation, and wasm reports `fatal error: error in backend: relocations for function or section offsets are only supported in metadata sections`. Threshold dependent: `counter.ae` (2 label addresses) compiled; `test_platform_caps.ae` / `test_coop_chain.ae` (5 label addresses each) failed. The guard is now `#if AETHER_GCC_COMPAT && !defined(__EMSCRIPTEN__)`, so wasm builds take the switch-case path the same way MSVC does. No runtime impact on native GCC/Clang targets; they still get computed-goto dispatch.

- **`aether_panic.h` sigjmp_buf shim extended to Emscripten and freestanding targets.** The shim that maps `aether_sigjmp_buf` / `AETHER_SIGSETJMP` / `AETHER_SIGLONGJMP` to the signal-mask-unaware `jmp_buf` family was guarded by `#ifdef _WIN32` only, which missed two more targets: Emscripten's wasm32 clang supports `setjmp` / `longjmp` via `-mllvm -enable-emscripten-sjlj` but not `sigsetjmp` / `siglongjmp`, and freestanding ARM bare-metal (`arm-none-eabi-gcc -ffreestanding` against newlib-nano, as used by `make ci-embedded`) has no `sigjmp_buf` declaration at all because POSIX signals aren't part of the freestanding C surface. The guard is now `#if defined(_WIN32) || defined(__EMSCRIPTEN__) || (defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 0)`. The matching `aether_panic_install_signal_handlers()` implementation is gated with the negation: the POSIX `sigaction` path compiles only on hosted non-Windows non-wasm targets; the other three get a no-op stub. The rest of the panic / try / catch path (built on `setjmp` / `longjmp`) works unchanged on all of them.

### Changed

- **`docs/containment-sandbox.md` — host-module matrix extended with a Tcl column and a new "Shared-interpreter behavior" section.** That section is the proper home for the long-standing design details Perl, Ruby, and Tcl exhibit (shared interpreter retains scrubbed env across run/run_sandboxed boundaries) plus Ruby's `Fiddle.dlopen` interaction. Keeps this information out of user-facing per-module READMEs (which should stay minimal) and puts it in the one design-level reference doc where sandbox users already look for behavior detail.

- **`contrib/host/TODO.md`** — closed every doc-only item that now has a canonical home in `docs/containment-sandbox.md` (Python os.environ, Perl prefix + %ENV, Ruby ENV + Fiddle.dlopen, Tcl ::env) and the two new-universal items that the bridge-self-containment + import-path fixes resolved. Also struck through the unachievable `git show lazy-evaluation:...` recovery instruction (the branch no longer exists on GitHub or in the local reflog) now that a fresh Python demo ships in this branch.

## [0.72.0]

### Fixed

- **`contrib/tinyweb` compiles on the current stdlib.** `filter_all` now inlines its body instead of calling `filter(_ctx, …)` — the DSL `_ctx` auto-injector was treating that as a 4-arg call against a 3-effective-arg function. Renamed the module's `server_start` / `server_stop` to `tw_start` / `tw_stop` to avoid colliding with `std.http`'s Go-style `server_start` wrapper. Examples now prefix every top-level tinyweb name with `tinyweb.` (non-selective `import contrib.tinyweb` does not expose names as bare) and add `import std.http` / `.string` / `.list` / `.map` / `.tcp` so the inlined module body's `http.xxx` / `string.xxx` / etc. qualified calls resolve in the importer's scope. `test_integration.ae` swapped `http.server_start` / `http.get` / `http.post` for the raw externs (`http_server_start_raw`, `http_get_raw`, `http_post_raw`): the Go-style wrappers now return `(body, err)` tuples that the test's ptr-based `_chk_resp` / `_chk_code` / `_chk_body_contains` assertions can't consume. `test_integration.ae` runs green end-to-end (8/8).

## [0.71.0]

### Fixed

- **Closure inside actor handler writing a state field is now rejected at compile time** (`compiler/codegen/codegen_expr.c`). Closures inside actor receive arms have no access to `self`, so a write like `inc = || { count = count + 1 }` (where `count` is an actor `state` field) compiled to a stale local read — a silent wrong answer. Codegen now walks every closure body inside every receive arm and, for each write to a state field, emits a compile-time error pointing at the offending line with a suggestion to use the arm-local workaround. New helper `aether_error_full()` in `compiler/aether_error.c` for line-numbered errors with suggestion + context + code. `aetherc` now checks `aether_error_count()` after codegen (compared against pre-codegen count, so legacy parser-noise tests don't regress) and aborts the build instead of leaving a half-written `.c` behind. Regression test: `tests/integration/closure_actor_state_reject/`. L4 in `docs/closures-and-lifetimes.md`.

### Changed

- **`docs/closures-and-lifetimes.md`**: enumerated all five currently-tracked closure limitations (L1–L5), split the previous lumped "Known limitations" paragraph into per-limit sections with minimal reproducer, workaround, and proper-fix shape for each. Added a note on L1 explaining why a trivial `intptr_t` widening in the generic `call()` dispatch doesn't fix the truncation (the destination variable's C type is pinned via the symbol table, not just the AST, so a real fix needs typechecker work).

## [0.69.0]

### Fixed

- **Closure body references a later-numbered closure** (`compiler/codegen/codegen_expr.c`). A closure's body can construct inline closure literals and pass them as arguments to other functions. Each lambda gets its own `_closure_fn_N` in the emitted C. When the outer closure is numbered before its inline lambdas, its body referenced `_closure_fn_N` symbols that hadn't been declared yet at that point in the file (`'_closure_fn_N' undeclared` error). `emit_closure_definitions` now runs in two passes: pass 1 emits every env typedef and every function prototype, pass 2 emits bodies and constructors. A closure body can reference any `_closure_fn_N` by name regardless of numbering. Three helpers extracted for readability: `resolve_closure_return_type`, `emit_closure_signature`, `emit_closure_env_typedef`. Regression test: `tests/syntax/test_closure_forward_references.ae`.

- **Nested lambda's `return` mis-typed the enclosing closure** (`compiler/codegen/codegen_func.c`). `has_return_value` walked an AST subtree looking for return statements with values. A nested lambda's `return` bubbled up through the recursive walk and mis-typed the enclosing closure as `int` — producing `static int _closure_fn_N(...) { ...; }` with no return statement, undefined behavior caught by `-Wreturn-type`. One-line fix: `has_return_value` now stops at `AST_CLOSURE` boundaries so a nested closure's return belongs to that closure, not to any enclosing scope. Regression test: `tests/integration/closure_nested_return/` (compiles the generated C with `-Werror=return-type` and requires a clean build).

- **Captures across nested trailing blocks** (`compiler/codegen/codegen_expr.c`). A variable declared inside a trailing block (e.g. `root = grid() { c = 42; ... }`) lives in the enclosing function's scope because trailing blocks are inlined at the call site, not hoisted. Previously a closure inside a sibling or nested trailing block could not capture such variables — capture discovery stopped at ANY `AST_CLOSURE`, including trailing-block closures (value == `"trailing"`), so names declared inside one trailing block were invisible to inner closures (`'c' undeclared` in generated C). Two scope-analysis helpers updated: `subtree_declares` now recurses through trailing-block closures while stopping at real closures; a new `scope_declares_at_top_level` helper is used by `is_top_level_decl_in_function` to walk trailing blocks but NOT nested if/for/while blocks — preserving the Python-style rule that `v = ref_get(num)` inside `if key == EQUAL { ... }` stays a block-local (examples/calculator-tui.ae still builds cleanly). Regression test: `tests/integration/closure_trailing_block_capture/`.

  Design note worth flagging: the nested-trailing-block fix was subtler than it looked. A naive "recurse everywhere" version broke calculator-tui. The final version threads the needle — trailing blocks are transparent to scope lookup (they inline at the call site), but nested if/for/while blocks still aren't. That's the right answer mechanically (it matches how trailing blocks work at codegen time) but it's the kind of detail that could surface a different corner case later. Tracked in `docs/closures-and-lifetimes.md` bugs 6-8.

## [0.67.0]

### Added

- **Embedded namespaces — Aether scripts as typed Java / Python libraries via `ae build --namespace`**: a directory containing a `manifest.ae` plus sibling `.ae` scripts is now compilable into a single `.so` / `.dylib` *plus* per-language SDKs (Java + Python in v1) that wrap the native library behind an idiomatic class API. The host application — Java, Python, or any FFI-capable language — `import`s the generated SDK, gets typed setters for inputs, typed event handlers for the script's `notify()` calls, and typed methods for every exported script function. No SWIG to invoke, no JNI to write, no Panama boilerplate visible at the user's level.

    Composing pieces:

    - `notify(event: string, id: long) -> int` extern in a new `std.host` Aether module (`runtime/aether_host.{h,c}` + `std/host/module.ae`): claim-check primitive following the EAI / Hohpe pattern. Aether scripts emit thin notifications carrying just an event name and an int64 id; the host registers handlers via `aether_event_register()` and looks up details (if any) through normal typed downcalls. Single-threaded synchronous; cap of 64 handlers (raisable). Tests: `tests/integration/notify/` covers register, missing-handler-returns-0, replace-in-place re-registration, unregister, clear, NULL-safety.

    - Manifest builder DSL in `std.host`: `describe(name)`, `input(name, type)`, `event(name, carries)`, `bindings()`, `java(package, class)`, `python(module)`, `go(package)`. A `manifest.ae` is conventionally an `abi() { describe("name") { input(...) event(...) bindings() { java(...) } } }` block — same trailing-block + `_ctx`-injection idiom as `contrib/tinyweb`'s `web_server() { path() { end_point(...) } }` or `examples/calculator-tui.ae`'s `grid() { btn(...) callback { ... } }`. Each builder declares `_ctx: ptr` first so the codegen auto-injects context inside the trailing block; the C side ignores `_ctx` (the manifest registry is global state). `tests/integration/manifest/` exercises every builder via dlopen + manifest_get(). Two compiler enhancements made the trailing-block grammar land cleanly: (a) the `_ctx`-first builder pre-pass in `codegen.c` now also recognizes externs from imported modules (previously only walked locally-defined functions, so std.host's externs were missed); (b) auto-injection at call sites fires whenever the user's arg count is exactly one less than the function's declared param count (previously gated on `gen->in_trailing_block > 0`, which broke the outermost call in a manifest's body — `_aether_ctx_get()` returns NULL at the top of the stack, which builders that ignore `_ctx` handle correctly).

    - `aetherc --emit-namespace-manifest <manifest.ae>` writes the manifest as JSON to stdout (declaration order preserved, every binding sub-object scoped). `aetherc --emit-namespace-describe <manifest.ae> <out.c>` writes a self-contained `.c` stub with a static const `AetherNamespaceManifest` and the `aether_describe()` discovery entry point. `aetherc --list-functions <file.ae>` prints `name|return|p1:t1,...` lines for every top-level function. Both extractors walk the parsed AST structurally — we deliberately don't execute the manifest at compile time, matching how `protoc-gen-*` and `openapi-generator` work.

    - `ae build --namespace <dir>` orchestrates the pipeline: discovers sibling `.ae` files, deduplicates `import` lines and at-most-one `main()`, concatenates them into one synthetic translation unit, runs `--emit=lib` with the describe stub appended via `--extra`, embeds the manifest as the static struct, and dispatches to per-language SDK generators. Default output: `<dir>/lib<namespace>.so` / `.dylib`, with the namespace name coming from the manifest (not the directory). Artifacts land **inside** `<dir>` so the namespace is shippable as a self-contained unit.

    - **Python SDK generator**: when a manifest declares `python("module_name")`, emits `<dir>/<module_name>.py` — a self-contained ctypes-based class. `set_<input>(value)` per input (v1 stores on instance), `on_<event>(handler)` per event with proper trampoline keepalive (callbacks held in `self._callbacks` so the GC doesn't reclaim the `CFUNCTYPE` while C still has a pointer), per-function methods with auto string ↔ bytes marshalling, `describe()` returning a typed `Manifest` populated from the embedded `_NamespaceManifest` struct via `ctypes.Structure`. Tests: `tests/integration/namespace_python/` covers two inputs, two events, three function signatures end-to-end. Skips cleanly if `python3` isn't installed.

    - **Java SDK generator**: when a manifest declares `java("com.example.foo", "Class")`, emits `<dir>/com/example/foo/Class.java` using Panama Foreign Function & Memory API (stable in JDK 22+). Cached `MethodHandle` per function via `Linker.downcallHandle`, `setX(value)` per input, `on<EventName>(LongConsumer)` per event using `MethodHandles.publicLookup().findVirtual(LongConsumer.class, "accept", ...)` (avoids the lambda nestmate-private lookup error), camelCased methods per function with `arena.allocateFrom` ↔ `MemorySegment.getString(0)` string marshalling, typed `Manifest describe()` walking the static struct with explicit byte offsets. Implements `AutoCloseable` so try-with-resources releases the `Arena`. Tests: `tests/integration/namespace_java/` mirrors the Python tests. Skips cleanly if `javac`/`java` aren't installed or the JDK is older than 22.

    - **Ruby SDK generator**: when a manifest declares `ruby("module_name")`, emits `<dir>/<module_name>.rb` using Ruby's stdlib `Fiddle` (no extra gem required, ships with MRI Ruby 1.9.2+). The generated module wraps the `.so` via `Fiddle.dlopen`; the SDK class exposes `set_<input>(value)` accessors, `on_<event_snake_case> { |id| ... }` block-based event handlers with proper `Fiddle::Closure::BlockCaller` trampoline keepalive (callbacks held in `@callbacks` so Ruby's GC doesn't reclaim them while C holds the function pointer), per-function methods with auto `String` ↔ `Fiddle::Pointer` marshalling, and `describe()` returning a typed `Manifest` populated by walking the embedded `AetherNamespaceManifest` struct with explicit pointer offsets (mirrors the Python and Java approaches). Event names are converted from PascalCase (manifest convention) to snake_case (Ruby convention): `event("OrderPlaced", "int64")` → `on_order_placed`. Tests: `tests/integration/namespace_ruby/` mirrors the Python and Java tests. Skips cleanly if `ruby` isn't installed.

    - **Worked example**: `examples/embedded-java/trading/` ships a `trading` namespace with `placeTrade.ae`, `killTrade.ae`, `getTicker.ae`, a `manifest.ae`, a `TradingDemo.java`, and a `build.sh` that runs the whole pipeline and prints a trade book built up from the events. `tests/integration/embedded_java_trading_e2e/` runs the worked example end to end and asserts every expected line of output appears.

    Out of scope for v1 (each tracked in `docs/embedded-namespaces-and-host-bindings.md`):

    - Live host-supplied callbacks (`host_call`) — Aether scripts can't yet call back into host functions. The v1 ABI is host → script for data and script → host for events; the bidirectional case is the "Shape B" milestone.
    - Escape-hatch `import trading.manifest` to add a non-sibling script to a namespace.
    - `@private_to_file` annotation to exclude a sibling script.
    - Wall-clock timeout / allocation budget for scripts running inside a host.
    - Go SDK generator (parser captures the `go(package)` binding but the emitter is a stub).

    The v2 work folds in the `--emit=lib` transport layer (compiler flag, capability-empty default, `runtime/aether_config.{h,c,i}`, opaque `AetherValue*`, eight `tests/integration/emit_lib*/` directories) that was originally planned as a standalone v1 PR; that layer was never shipped on its own — it's the foundation v2 builds on. The full design lives at `docs/embedded-namespaces-and-host-bindings.md`; the speculative `docs/aether-embedded-in-host-applications.md` and `docs/aether-dsl-as-a-rules-engine.md` are annotated with what's now real and what's still future.

- **`notify()` flushes stdout before invoking the host event handler**: when an Aether namespace is loaded as a `.so` by a Java / Python / Ruby host via `dlopen`, libc's `stdout` is fully buffered (the `.so` doesn't see a TTY). Script-side `println()` calls would accumulate in the C-side buffer and only flush at process exit, so demo console output appeared scrambled — host event-handler `println`s landed in order, but the Aether script's preceding lines all came out at the very end. Adding `fflush(NULL)` to `notify()` in `runtime/aether_host.c` (just before the registered handler runs) ensures that anything the script printed leading up to the event is on stdout in the right order. Verified by `tests/integration/embedded_java_trading_e2e/`, which now sees `[ae] place_trade order_id=100` immediately before `[event] OrderPlaced id=100`. Cosmetic-only — does not affect correctness of the values returned by the script, only the on-screen ordering of pre-event log output.

- **Embedded namespace tests + worked example tag script-side prints with `[ae]`**: every `println()` inside `tests/integration/namespace_{python,ruby,java}/calc.ae` and `examples/embedded-java/trading/aether/{placeTrade,killTrade,getTicker}.ae` now starts with the literal `[ae] ` so a human reading the test output (or the worked example's stdout) can tell at a glance which lines came from inside the embedded Aether script versus which came from the Java / Python / Ruby host. The four test scripts grep for at least one `[ae]` line in the captured output and report `[PASS] Aether [ae] script-side output visible to host` if it landed; this is the regression test that pairs with the `notify()`-flush fix above.

- **Embedded namespace test SDKs renamed to `*_generated_sdk` to make the generated nature obvious**: the Python / Ruby modules emitted by `ae build --namespace` for `tests/integration/namespace_{python,ruby}/manifest.ae` are now `calc_generated_sdk.py` / `calc_generated_sdk.rb` instead of `calc_sdk.py` / `calc_sdk.rb`; the Java class is `CalcGeneratedSdk` instead of `Calc`. No change to the SDK generators themselves — the manifest's `python("calc_generated_sdk")` / `ruby("calc_generated_sdk")` / `java("com.example.calc", "CalcGeneratedSdk")` declarations decide the output filename, and we just chose more obvious names. Reduces the chance a future contributor accidentally edits a file in a temp directory thinking it's hand-written source.

## [0.55.0]

### Changed (breaking)

- **Stdlib migrated to Go-style `(value, err)` result types**: every stdlib function that can fail and where the failure reason matters now returns a tuple. The raw ptr/int/NULL-returning externs are preserved under `*_raw` names as an escape hatch for advanced callers. Migrated modules and the new idiomatic calls:
  - `std.http`: `body, err = http.get(url)`, `http.post(url, body, ct)`, `http.put(...)`, `http.delete(url)`. Auto-free the underlying response, return transport errors and non-2xx status as strings. Server lifecycle: `http.server_bind(server, host, port)` and `http.server_start(server)` now return an error string. Raw externs renamed: `http_get_raw`, `http_post_raw`, `http_put_raw`, `http_delete_raw`, `http_server_bind_raw`, `http_server_start_raw`.
  - `std.file` / `std.fs`: `content, err = file.read(path)` (opens, reads, closes), `handle, err = file.open(path, mode)`, `err = file.write(path, content)`, `err = file.delete(path)`, `size, err = file.size(path)`. Raw externs renamed: `file_open_raw`, `file_read_all_raw`, `file_write_raw`, `file_delete_raw`, `file_size_raw`.
  - `std.io`: `content, err = io.read_file(path)`, `err = io.write_file(path, content)`, `err = io.append_file(path, content)`, `err = io.delete_file(path)`, `info, err = io.file_info(path)`, `err = io.setenv(name, value)`, `err = io.unsetenv(name)`. Raw externs renamed: `io_read_file_raw`, `io_write_file_raw`, `io_append_file_raw`, `io_delete_file_raw`, `io_file_info_raw`, `io_setenv_raw`, `io_unsetenv_raw`.
  - `std.string`: `value, err = string.to_int(s)` and the same for `to_long`, `to_float`, `to_double`. Replaces the out-parameter anti-pattern (`string_to_int(str, &out)`). Raw externs renamed: `string_to_int_raw`, `string_to_long_raw`, `string_to_float_raw`, `string_to_double_raw`. Supporting C shims `string_try_int`/`string_get_int` (and the four variants) split the parse into a validity check plus a value extractor so the wrapper can use tuple returns.
  - `std.json`: `value, err = json.parse(json_str)`. Raw extern renamed: `json_parse_raw`.
  - `std.os`: `output, err = os.exec(cmd)`. Raw extern renamed: `os_exec_raw`. `os_system` still returns an exit code by the POSIX convention.
  - `std.tcp` / `std.net`: `sock, err = tcp.connect(host, port)`, `n, err = tcp.write(sock, data)`, `data, err = tcp.read(sock, max)`, `server, err = tcp.listen(port)`, `sock, err = tcp.accept(server)`. Raw externs renamed: `tcp_connect_raw`, `tcp_send_raw`, `tcp_receive_raw`, `tcp_listen_raw`, `tcp_accept_raw`. (Byte-transfer wrappers are named `write`/`read` because `send` and `receive` are reserved actor keywords in Aether.)
  - `std.dir` / `std.fs`: `err = dir.create(path)`, `err = dir.delete(path)`, `list, err = dir.list(path)`. Raw externs renamed: `dir_create_raw`, `dir_delete_raw`, `dir_list_raw`.
  - `std.fs`: `list, err = fs.glob(pattern)`, `list, err = fs.glob_multi(patterns)`. Raw externs renamed: `fs_glob_raw`, `fs_glob_multi_raw`.
  - `std.list` / `std.collections`: `err = list.add(list, item)`. Previously silent void; now returns an error string on resize/OOM failure. Raw extern renamed: `list_add_raw` (returns `int` 1/0 for success/failure).
  - `std.map` / `std.collections`: `err = map.put(map, key, value)`. Previously silent void; now returns an error string on resize/OOM failure. Raw extern renamed: `map_put_raw` (returns `int` 1/0).
  - `std.log`: `err = log.init(filename, level)`. Previously silent void; now returns an error string if the log file can't be opened (logging still works via stderr as a fallback). Raw extern renamed: `log_init_raw` (returns `int` 1/0).

  **User migration**: every call site that uses a migrated function needs updating. The old ptr/int-returning externs are still available via the `_raw` suffix for code that needs direct struct access. Existing tests and examples have been updated in the same change.

  **Not migrated** (infallible or already-correct): all of `std.math` (pure computation), all of `std.path` (pure string manipulation), predicate functions (`file_exists`, `map_has`, etc.), defensive lookups (`map_get`, `list_get`, `json_object_get`, `io_getenv`, `os_getenv`), HTTP response accessors, print functions, setters, and `*_free` functions. `os_system` retains its POSIX exit-code convention.

### Added

- **Aether-native stdlib wrappers**: stdlib modules can now define Aether functions alongside their C externs. Previously the module merger in `compiler/aether_module.c:894` explicitly skipped `std.*` imports on the assumption that stdlib was extern-only, which blocked Go-style result-type wrappers from being written in the module files themselves. The skip is removed; the typechecker now always registers every extern from a stdlib module (regardless of selective-import filter) so merged wrapper bodies can resolve their dependencies. Selective-import visibility for user code is still enforced at qualified-call sites via a new `is_selective_import_blocked` check in `lookup_qualified_symbol`, so `import std.math (sqrt)` still rejects `math.pow` at the call site even though `math_pow` is now in the symbol table for internal use. Codegen's stdlib-import extern emission at `codegen.c:1320` correspondingly drops its selective filter so every extern from an imported stdlib module is declared in the generated C. The int→ptr call-site cast at `codegen_expr.c:1140` now matches both the original call-site name and the dot-normalized C name, so merged stdlib wrappers with ptr parameters also get their arguments auto-cast, and now also handles `string → ptr` conversion to silence C's discards-qualifiers warning when a string is passed into a ptr-typed stdlib wrapper. Supporting changes include a new syntax test `tests/syntax/test_string_parse_tuple.ae` covering the new `string.to_int/to_long/to_float/to_double` wrappers.

- **HTTP client response accessors**: `http.response_status`, `http.response_body`, `http.response_headers`, `http.response_error`, and `http.response_ok` let Aether code read fields out of the struct returned by `http.get_raw`/etc. Previously the client was a smoke test: you could fire a request and free the response but had no way to read status code, body, headers, or transport error from Aether. All accessors are NULL-safe. `http.response_ok` returns 1 only when there's no transport error AND the status is 2xx, which is the idiomatic "did it work?" check. Added in `std/net/aether_http.{c,h}`, exported in `std/http/module.ae` and `std/net/module.ae`, with C runtime tests in `tests/runtime/test_runtime_http.c`, a syntax test in `tests/syntax/test_http_client_accessors.ae`, and a runnable example in `examples/stdlib/http-client.ae`.

- **`net.await_io(fd)` — reactor-pattern async I/O for actors**: exposes the runtime's per-core I/O reactor (`scheduler_io_register` / `MSG_IO_READY`) to Aether code. An actor calls `net.await_io(fd)` to suspend until `fd` becomes readable; when the scheduler's poller observes the event, it delivers an `IoReady { fd, events }` message to the actor's mailbox and resumes it on any available core — without blocking any scheduler thread. The infrastructure already existed in `runtime/scheduler/` (PR #140 demonstrated it at the C level delivering substantially higher HTTP throughput than the blocking keep-alive worker it replaced); this change makes it reachable from `.ae` sources. Implementation: (1) `MSG_IO_READY` renumbered from 300 to 255 to fit inside the 256-slot generated actor dispatch table; (2) `register_message_type` in `runtime/actors/aether_message_registry.c` reserves ID 255 exclusively for the name `IoReady` and skips it when handing out user IDs, so any Aether program that defines `message IoReady { fd: int, events: int }` lands on the slot the scheduler delivers to; (3) a new TLS `void* g_current_step_actor` declared in `runtime/scheduler/multicore_scheduler.h` is set by every generated `*_step()` function at entry (`compiler/codegen/codegen_actor.c:347`), making the currently-running actor identifiable from the bridge regardless of scheduling mode (main-thread sync, scheduler dispatch, work-stealing); (4) new `std/net/aether_actor_bridge.c` reads `g_current_step_actor` + `current_core_id` from TLS and calls `scheduler_io_register`. The Aether surface lives in `std/net/module.ae` and `std/http/module.ae`: `net.await_io(fd) -> string` returns `""` on success or an error string, plus minimal pipe/fd helpers (`net.pipe_open`, `net.fd_write`, `net.fd_close`) that let user code produce a testable readable fd without going through TCP. The bridge is linked into `libaether.a` / tests / user programs but excluded from the `aetherc` compiler binary via a new `STD_REACTOR_SRC` split in the Makefile (the compiler doesn't link the scheduler runtime). End-to-end test in `tests/syntax/test_await_io.ae` creates a pipe, spawns a Reader actor that calls `await_io` on the read end, then writes "hello" to the write end from main; the scheduler's I/O poller observes the fd is ready, delivers `IoReady` to the actor, and the handler prints success. Proposed by Ariel Mirra.

- **macOS Gatekeeper handling for `ae version install` / `ae version use`**: new `macos_prepare_binary` / `macos_prepare_bin_dir` helpers in `tools/ae.c` re-sign freshly installed/copied binaries in place with `codesign --force --sign -` and clear resource-fork/xattr state. Without this, the first run of any binary from a fresh install hung for several minutes under `syspolicyd` or died with `Killed: 9`, because released Aether binaries inherit an adhoc signature but no local Gatekeeper assessment. Called from both `cmd_version_install` (after extraction) and `cmd_version_use` (after copy to `~/.aether/bin/`). Also removed the self-invoke `--sync-from` call in `cmd_version_use` which triggered the same Gatekeeper hang on the just-copied binary — the existing in-process sync of `lib/`, `include/`, `share/` replaces it. Replaced the silent-failure `cp -f "src"/* "dest"/ 2>/dev/null; true` with an explicit `sh -c 'set -e; for f in ...; do cp ...; done'` plus a post-copy check that the destination `ae` binary exists. Verified end-to-end on macOS arm64: `ae version use v0.30.0` and `ae version use v0.45.0` both complete in under a second with no hang and no SIGKILL. Fixes #88.

### Added

- **`hide` and `seal except` scope directives** — declare at the top of any block that one or more identifiers from outer scopes are unreachable from this block (and every nested block within it). `hide name1, name2` blocks specific outer bindings. `seal except name1, name2` is the inverse — blocks ALL outer bindings except the listed whitelist. Both directives are scope-level (position within the block doesn't matter), apply to reads and writes, propagate to all nested blocks, and don't reach through call boundaries (a visible function defined in an outer scope can still use the names via its own lexical chain). Forbids redeclaring a hidden name in the same scope. New error code `E0304` ("`X` is hidden in this scope by `hide` or `seal except`"). See `docs/hide-and-seal.md` for the full design rationale and edge cases. Tests: `tests/syntax/test_hide_basic.ae`, `tests/syntax/test_seal_except.ae`, `tests/integration/hide_seal_directives/test_hide_reject.sh`.
- **Filesystem stdlib bundle in `std.fs`**: five small POSIX wrappers that let Aether programs stop shelling out for routine filesystem operations, each following the 0.55.0 `_raw` + Go-style wrapper convention:
  - `fs_mkdir_p_raw(path)` / `err = fs.mkdir_p(path)` — `mkdir -p` semantics: creates the path and any missing parent directories, treats already-existing directories as success.
  - `fs_symlink_raw(target, link_path)` / `err = fs.symlink(target, link_path)` — create a symbolic link. The target string is recorded verbatim, so relative targets stay relative.
  - `fs_readlink_raw(path)` / `target, err = fs.readlink(path)` — read a symlink's target. Returns `("", "not a symlink")` when `path` isn't a symlink.
  - `fs_is_symlink(path)` — pure boolean query: returns 1 if `path` is itself a symlink, 0 otherwise. Does NOT follow the link. No Go-style wrapper (matches `file_exists` / `dir_exists` shape).
  - `fs_unlink_raw(path)` / `err = fs.unlink(path)` — remove a file or symlink. Refuses to remove directories (use `dir.delete` for that).
  Windows symlink ops (`fs_symlink_raw` / `fs_readlink_raw` / `fs_is_symlink`) are stubbed pending `CreateSymbolicLinkW` + a junction fallback for directories; `fs_mkdir_p_raw` and `fs_unlink_raw` work on Windows via `_mkdir` / `_unlink`. Tests: `tests/syntax/test_fs_stdlib_bundle.ae` (14 sub-cases including nested-dir creation, idempotent `mkdir_p`, symlink/readlink/is_symlink round-trip, refuse-to-unlink-a-directory, refuse-to-unlink-missing-path).

- **`os_which` in `std.os`**: search `$PATH` for an executable. Returns the absolute path to the first hit, or `""` if not found. Kept as a plain extern (not `_raw` + wrapper) because "not found" is a valid answer, not a failure — matches how `file_exists` / `dir_exists` are modelled. If `name` already contains `/`, it's returned as-is when it's executable (matches POSIX `command -v`). Empty `PATH` entries match the current directory (POSIX). Windows is stubbed pending PATHEXT-aware lookup. Tests: `tests/syntax/test_os_which.ae` (5 sub-cases).

### Fixed

- **Composite types in message fields (`string[]`, `int[]`) silently corrupted data**: Parser accepted `sites: string[]` in message definitions, but the runtime message registry only stored a single `type_kind` per field, dropping element-type information. Receive-pattern destructuring generated `int* sites = _pattern->sites;` when the struct field was actually `const char**`, and the send-side emitted `.sites = {"a", "b"}` inside designated initializers where C interprets the braces as a scalar initializer list rather than a pointer. Reading any element in the handler produced garbage (e.g. `1819043176` = ASCII bytes of "hell" read as int). Four-part fix: (1) `MessageFieldDef` now carries resolved `c_type` and `element_c_type` strings populated at registration time via `get_c_type`; (2) receive-pattern destructuring in `codegen_actor.c` uses the stored `c_type` instead of rebuilding a stub `Type` from `type_kind`; (3) send-side codegen pre-walks message field initializers and hoists each array literal to a `static const T _aether_arr_N[] = {...}` local before the `{ Msg _msg = ...; send; }` block, so the array's storage has program lifetime instead of expiring when the send-expression block exits. Hoisting is essential for fire-and-forget sends because the receiver processes the message asynchronously, after the sender's compound literal would have gone out of scope; without the hoist the receiver reads freed memory (observed on Linux gcc and Windows, worked by coincidence on macOS clang); (4) typechecker now propagates element type onto `AST_ARRAY_ACCESS` nodes so print format specifiers and string interpolation pick the correct specifier. Added `tests/syntax/test_message_array_fields.ae` covering `string[]`, `int[]`, and mixed scalar+array messages, and `examples/actors/message-with-arrays.ae` demonstrating the pattern. Reported by a downstream port.
- **HTTP client example taught the wrong error-handling pattern**: `docs/stdlib-api.md` and `docs/stdlib-reference.md` showed `if response != 0 { println("Got response") }` as a success check, but `http.get_raw` always returns a non-null response (unless out of memory); on DNS/connect/send failure it returns a struct with `error` set. The documented pattern ran the success branch for failed requests. Both docs now use the new `http.response_ok` accessor and explicitly warn that `response != 0` is not a success check. Reported by a downstream port.
- **Array type notation in style guide did not match parser**: `docs/type-annotation-style-guide.md` showed the prefix form `[int]` / `[string]` for array types, but the parser has only ever accepted the C-style suffix form `int[]` / `string[]` (consistent with `docs/type-inference-guide.md`). Updated the style guide to the correct form and added a note flagging the prefix form as historical. Reported by a downstream port.
- **String memory leak in loops**: `string_concat`, `string_substring`, `string_to_upper`, `string_to_lower`, `string_trim`, and string interpolation in loops no longer leak the previous value on reassignment. The codegen now emits a heap-ownership flag per string variable and frees the old value before each reassignment, preventing O(n²) memory growth that caused OOM on tight loops.
- **`_heap_<var>` undeclared in late string reassignment**: When a string variable was first introduced inside a nested block (e.g. the else branch of an `if`/`else`) via a heap-string expression, the codegen emitted a reassignment wrapper referencing `_heap_<var>` without ever declaring the flag, producing "undeclared identifier" errors in the generated C. The reassignment path now lazy-declares the heap-ownership flag if it isn't already tracked, and the initial-declaration path marks the variable so the lazy path doesn't double-declare. Regression test: `tests/syntax/test_string_late_heap_reassign.ae`.
- **`fs_glob` recursive walker hid dot-prefixed files**: `walk_recursive` previously skipped every directory entry whose name started with `.`, so patterns like `**/.build.ae` or `**/.*.ae` always returned zero results — both `.git`/`.aeb` directories AND legitimate dot-files like `.build.ae` were filtered out at the dir-entry level. The walker now skips only `.` and `..` unconditionally, skips dot-prefixed *directories* from recursion (preserving the original intent), and passes `FNM_PERIOD` to `fnmatch` so dot-prefixed *files* match when the pattern explicitly includes a leading dot, matching POSIX shell-glob semantics. Regression test: `tests/syntax/test_glob_dotfiles.ae`.

- **`make test-ae` swallowed runtime-failure diagnostic output**: the parallel test runner captured compile-phase stderr to a per-test file but piped the run-phase stdout and stderr directly to `/dev/null`, so any test that built cleanly and then failed at runtime produced only `[FAIL] name (runtime error)` followed by an empty `--- name ---` block in FAILURE DETAILS. A Windows CI run on an unrelated PR made this obvious: `syntax_test_glob_dotfiles` failed with no recoverable error output, no exit code, no stdout, no stderr — nothing actionable. The inline runner built by the `test-ae` target now redirects the run-phase streams to per-test files under the shared tempdir (`build_<name>.{out,err}`, `run_<name>.{out,err}`, `rc_<name>.txt`, `phase_<name>.txt`), and FAILURE DETAILS emits a structured block per failed test with a horizontal rule, the failing phase (`compile` / `runtime` / `shell`), the runtime exit code, and the captured stdout/stderr indented for readability. Verified by injecting a deliberate runtime failure (`exit(42)` after three `println` calls) and a deliberate compile error (undefined variable + undefined function): both now surface their full diagnostic output in the CI log.

## [0.48.0]

### Added

- **`callback` keyword for trailing blocks**: Third trailing-block mode — `func(args) callback { body }` creates a real closure (hoisted, captures variables from scope) rather than an inline DSL block. The block runs later when invoked, not at construction time. Also supports explicit params (`callback |x: int| { ... }`) and arrow bodies (`callback |x| -> x * 2`).

## [0.47.0]

### Changed

- **Module resolver state moved from globals to registry**: `source_dir` and `lib_dir` are now fields on `ModuleRegistry` instead of file-level statics, so state is scoped to the registry lifecycle. Prepares the compiler for safe reuse as a library (e.g. from the LSP).
- **`AETHER_LIB_DIR` environment variable**: Set `AETHER_LIB_DIR` to configure the module library directory without passing `--lib` on every invocation. `--lib` takes precedence if both are set.

## [0.48.0]

### Added

- **`callback` keyword for trailing blocks**: Third trailing-block mode — `func(args) callback { body }` creates a real closure (hoisted, captures variables from scope) rather than an inline DSL block. The block runs later when invoked, not at construction time. Also supports explicit params (`callback |x: int| { ... }`) and arrow bodies (`callback |x| -> x * 2`).

## [0.47.0]

### Added

- **Builder functions** (renamed from `defer`): `builder` keyword for function definitions enables "configure then execute" DSL pattern. The trailing block runs first to fill a config object, then the function executes with it via implicit `_builder` parameter. Renamed to avoid overloading `defer` (which remains for scope cleanup). Complements the existing regular trailing-block pattern ("function first, block decorates").
- **Configurable builder factory with `with` clause**: `builder func(...) with factory_fn { ... }` lets SDK authors specify what config object the trailing block operates on. Defaults to `map_new`; alternatives include `list_new` or any user-defined zero-arg factory.
- **`--lib` flag for custom module library directory**: `aetherc --lib DIR` resolves imports from a custom directory instead of the default `lib/`. Threaded through `ae run` and `ae check`. Enables build tools like aetherBuild to use `.aeb/lib/` without polluting the project's `lib/`.
- **Unqualified selective imports**: `import mymodule (foo, bar)` now registers short names so `foo()` can be called without the `mymodule.` prefix. Qualified calls (`mymodule.foo()`) continue to work alongside unqualified ones. Enables clean DSL blocks where setter functions don't need module qualification:
  ```aether
  import build
  import build (release, lint, werror)

  main() {
      b = build.start()
      build.javac(b) {
          release("21")    // no build. prefix needed
          lint("all")
          werror()
      }
  }
  ```

### Fixed

- **Builder functions not merged from modules**: `module_merge_into_program` only handled `AST_FUNCTION_DEFINITION`, silently skipping `AST_BUILDER_FUNCTION`. Module-defined builder functions are now merged correctly.
- **Builder context `_ctx` injection failed for module-qualified calls**: The builder function registry compared names with underscores (`build_release`) against dotted call names (`build.release`), so `_ctx` was never injected. Fixed: normalize dots to underscores before comparison. Same fix applied to `is_builder_func_reg` and `get_builder_factory`.
- **Duplicate function definitions from repeated module imports**: `import build` followed by `import build (foo)` merged `foo` twice, with the second copy using generic `_argN` parameter names. Fixed: `module_merge_into_program` now skips already-merged functions.
- Fixed `-Wunused-result` warnings in `tools/ae.c` for `fread` and `system` calls.
- `install.sh` now prints a message before `git fetch --tags` so users know why SSH credentials may be requested.

## [0.45.0]

### Added

- **Core placement hint**: `spawn(Actor(), core: N)` distributes actors across scheduler cores. Actors on different cores run on different OS threads, enabling parallel I/O-heavy workloads. Use with `num_cores` builtin: `spawn(Worker(), core: i % num_cores)`.
- **Platform I/O poller**: `runtime/io/aether_poller.h` provides a unified event notification API with epoll (Linux), kqueue (macOS/BSD), and poll() (portable) backends. Foundation for non-blocking I/O and [PR #71](https://github.com/nicolasmd87/aether/pull/71) actor-integrated HTTP.
- **Socket timeouts on all stdlib TCP operations**: `tcp_connect`, `tcp_accept`, and HTTP server connections now set 30-second `SO_RCVTIMEO`/`SO_SNDTIMEO`. A dead or slow peer returns an error instead of hanging the thread forever.
- **HTTP server bounded thread pool**: Fixed pool of 8 worker threads replaces unbounded thread-per-connection. Prevents resource exhaustion under load.
- **HTTP server graceful shutdown**: `poll()`-based accept loop with 1-second timeout. Server checks `is_running` between connections and shuts down cleanly without waiting for the next client.

### Changed

- **Inline message path extended to int64/ptr/bool/actor_ref**: Single-field messages with `long`, `ptr`, `bool`, or actor ref types now skip heap allocation entirely — value stored directly in `Message.payload_int`. Previously only `int` (32-bit) qualified. Eliminates malloc+free for single-field messages, the most common pattern in tree-spawn and request-response workloads.
- **TLS caching in scheduler hot path**: `current_core_id` cached in a local variable at function entry in `scheduler_send_local` and `aether_send_message`, reducing TLS accessor overhead on macOS.
- **Partial batch enqueue**: `queue_enqueue_batch` now returns how many messages fit instead of failing the entire batch when the queue is near full. Eliminates the all-or-nothing behavior that forced remaining messages through the slower per-message fallback path.
- **Batch flush uses partial retry instead of per-message fallback**: `scheduler_send_batch_flush` retries `queue_enqueue_batch` with remaining messages instead of falling back to individual `scheduler_send_remote` calls, reducing kernel yield overhead in high-throughput fan-out patterns.
- **Missing benchmark implementations**: Added Pony, Java, and Scala skynet benchmarks for complete cross-language coverage (11 languages × 5 patterns = 55 benchmarks, zero skips).
- **Benchmark fairness**: Standardized skynet throughput to count total tree nodes across all 11 languages. Fixed Java detection on macOS (GNU sed `\U` → portable case statement). Fixed Scala package namespacing for sbt compilation.
- **Benchmark statistical rigor**: 5 runs per benchmark (median reported). JVM/BEAM languages get warmup runs before measurement. JSON results include min/max, coefficient of variation (CV%), and individual run values.
- **Benchmark visualization rewrite**: Sortable table columns, CV% color coding (green/orange/red), relative performance bars, min-max range, efficiency metric (throughput/MB), methodology explanation, Savina paper citation. Skynet tab added.
- **Benchmark runner rewritten in Aether**: `run_benchmarks.ae` replaces the bash script — compiles all 11 languages, runs benchmarks, parses output, computes statistics, writes JSON results. Dogfoods `std.os`, `std.string`, `std.io`.
- **Benchmark visualization server rewritten to pure stdlib**: `server.ae` replaced 26 `extern` FFI declarations and 200 lines of hand-written C (`server_ffi.c`) with stdlib imports (`std.net`, `std.io`, `std.string`, `std.os`). Zero C code required.
- **Inner benchmark Makefile removed**: All benchmark orchestration moved to root Makefile. `make benchmark` directly builds the Aether runner, runs benchmarks, builds the server, and launches the UI.
- **`string_array_get` bug fix**: Returned `AetherString*` (struct pointer) instead of `const char*` (raw string data). Any code using `string_split` + `string_array_get` with `print`, `string_contains`, etc. got garbled output. Fixed to return `s->data`.
## [0.44.0]

### Fixed

- **Module function return types not inferred across module boundaries**: When calling a module function that returns `string` or `ptr` (e.g., `result = mymod.greet("world")`), the type inferrer defaulted to `int` because `infer_type()` used `lookup_symbol()` which couldn't resolve dotted names like `mymod.greet`. Fixed: use `lookup_qualified_symbol()` instead, with guards to skip `void`/`unknown` types (which occur for pure-Aether functions whose return types haven't been inferred yet). Regression test added: `tests/integration/module_return_types/`.

### Changed

- **`MAX_MODULE_TOKENS`** increased from 2,000 to 20,000 — modules with many functions (e.g., a build system SDK) silently truncated at the old limit with no error message.
- **`MAX_TOKENS`** increased from 10,000 to 50,000 — same issue for large source files.

## [0.43.0]

### Changed

- **Sandbox preamble only emitted when needed**: Generated C no longer includes ~40 lines of sandbox bridge code (permission checker, `list_size`/`list_get` externs, `spawn_sandboxed` declaration) for programs that don't use sandboxing. Follows the same AST-scanning pattern as the existing actor detection.
## [0.42.0]

### Added

- **`fs_glob_multi(patterns)` in `std.fs`**: Multi-pattern glob that takes a list of patterns and returns merged results. Enables Starlark-style `glob(["**/*.c", "**/*.h"])` for build DSLs.

## [0.41.0]

### Fixed

- **`ae test` included library modules** (#109): `ae test` discovered `lib/*/module.ae` files and tried to build them as standalone programs, failing because they have no `main()`. Fixed: convention-based test discovery — only files named `test_*.ae` or `*_test.ae` are recognized as tests (like pytest's `test_*.py` or Go's `*_test.go`).
- **Module resolution didn't search relative to source file** (#99): `aetherc` resolved `lib/` and `src/` imports relative to CWD only, so integration tests with local `lib/` directories failed when run from the repo root. Fixed: `module_resolve_local_path()` now also searches relative to the source file's directory.
- **Windows: stdlib path functions used backslashes** (#99): `path_join()` used `\` on Windows, breaking cross-platform tests. `path_is_absolute()` didn't recognize `/` on Windows. Fixed: `path_join()` uses `/` universally; `path_is_absolute()` accepts both `/` and drive letters.
- **Windows: regression tests hardcoded `/tmp/`** (#99): File I/O tests used `/tmp/` which doesn't exist on native Windows. Fixed: tests use `TEMP`/`TMPDIR` env vars with `/tmp` fallback.
- **`ae version` showed stale version after install** (#88): `ae version list` checked the `current` symlink (stale from previous `ae version use`) before the `active_version` file. Fixed: `active_version` file is now the authoritative source. Also fixed `install.sh` reading the old version from the `ae` binary instead of the `VERSION` file.
- **Non-existent version install gave misleading message** (#95): `ae version install v0.99.0` downloaded a 404 HTML page, extracted 0 files, and suggested `--force`. Fixed: validates downloaded archive magic bytes (gzip/zip/xz) and fails immediately with a clear error if the version doesn't exist.
- **CI now runs `ae test`**: Added step [8/9] to `make ci` that runs the user-facing `ae test` command, catching divergence between the Makefile test runner and the CLI test runner.
## [0.40.0]

### Added

- **Runtime containment sandbox**: Deny-by-default grant system for filesystem, network, process, and environment access. Stdlib functions (`tcp_connect`, `file_open`, `os_exec`, `os_getenv`) check grants transparently — contained code has no idea it's sandboxed.
- **Sandbox DSL**: `sandbox("name") { grant_tcp("host"); grant_fs_read("/path/*"); ... }` with builder-style trailing blocks and invisible `_ctx` injection.
- **Grant pattern matching**: Prefix (`/etc/*`), suffix (`*.example.com`), exact, and wildcard (`*`) patterns for all grant types.
- **Nested sandbox restriction**: Inner sandboxes can only narrow, never escalate past outer sandbox grants.
- **`spawn_sandboxed(perms, program, arg)`**: Cross-process sandbox enforcement via POSIX shared memory and LD_PRELOAD. No temp files.
- **`libaether_sandbox.so`**: LD_PRELOAD library intercepting `open`, `connect`, `getenv`, `execve`, `dlopen`, `mmap(PROT_EXEC)`, `mprotect(PROT_EXEC)`, `fork`, `vfork`, `clone3`, `bind`, `listen`, `accept`. Built as part of `make stdlib`.
- **Denial logging**: File (default, `./aether-sandbox.log`), stderr (`AETHER_DENIED:` prefix), or silent. Controlled via `AETHER_SANDBOX_LOG` env var.
- **Six language host modules** (`contrib/host/`): Python, Lua, JS (Duktape), Perl, Ruby, Java. Each runs hosted code inside an Aether sandbox.
- **Token-guarded shared map**: `string:string` data exchange between Aether and hosted languages with frozen inputs, one-time token, and revoke-on-return. Native bindings (`aether_map_get`/`aether_map_put`) for all six languages.
- **Escape prevention**: `dlopen("libc.so.6")` blocked, `syscall()` blocked, `mmap(PROT_EXEC)` anonymous blocked, `mprotect(PROT_EXEC)` blocked, `fork`/`vfork`/`clone3` blocked by default (grant with `fork:*`).
## [0.39.0]

### Added

- **Named arguments**: `func(name: "alice", count: 3)` syntax in function calls. Names are documentation at the call site — consistent with Aether's `param: type` definition syntax. Positional and named can be mixed.
- **List literal tests**: Confirmed `[1, 2, 3]` and `["a", "b", "c"]` array literal syntax works (already existed in parser/codegen, now tested).
- **`select()` platform conditional**: Compile-time platform selection via named args. `select(linux: 8080, windows: 80, macos: 8080)` emits `#ifdef` chain in generated C. Supports `other:` fallback. Integer values work; string values require interpolation workaround pending type inference improvement.

## [0.35.0]

### Added

- **Heredoc strings**: `<<MARKER ... MARKER` syntax for multiline string literals. Preserves newlines, indentation, and special characters. Literal only (no `${expr}` interpolation — use regular strings for that). Left-shift operator `<<` is unaffected (heredoc only triggers when followed by an identifier). Dynamic buffer (no 64KB limit). Windows CRLF line endings handled.
- **`fs_glob(pattern)`**: Match files by pattern with `*`, `?`, and `**/` (recursive). Returns `DirList*` iterable via `dir_list_count`/`dir_list_get`/`dir_list_free`. Uses POSIX `glob()` for simple patterns and recursive directory walk for `**` patterns.
- **`dir_list_count(list)`** and **`dir_list_get(list, index)`**: Iterate `DirList` results from `dir_list()` and `fs_glob()`.
- **`aether_args_count()`** and **`aether_args_get(index)`**: Access command-line arguments via `std.os`. Exposes the runtime's existing `argc`/`argv` to Aether code. Returns `NULL` for out-of-bounds or negative indices.
- **`file_mtime(path)`**: File modification time as Unix timestamp (int). Returns 0 for nonexistent files. For incremental build support.
- **Lazy evaluation**: `lazy(closure)`, `force(thunk)`, `thunk_free(thunk)` builtins for deferred computation with memoization. Explicit forcing, eager by default.

## [0.32.0]

### Fixed

- **macOS Gatekeeper quarantine**: `install.sh` and `ae version use` now run `xattr -cr` to remove quarantine on macOS, fixing `Killed: 9` errors when switching versions
- **`ae version use` preserves initial install**: On both POSIX and Windows, the currently active version is backed up to `~/.aether/versions/` before switching, so it can be switched back to later
- **Uninitialized `tuple_types` in Type struct**: `FLUSH_LIT` macro in string interpolation parser used raw `malloc` without initializing `tuple_types`/`tuple_count`, causing `free_type()` to follow garbage pointers on Windows (ACCESS_VIOLATION `0xC0000005`)
- **Uninitialized `interp_as_printf` in CodeGenerator**: String interpolation in value assignments emitted `printf()` (returns int) instead of `_aether_interp()` (returns void*) on Windows due to uninitialized flag
- **Windows `_spawnvp` for command execution**: Replaced `system()` + `cmd /c` with `_spawnvp` on Windows to avoid shell quoting issues that caused random test failures
- **Unused variable checker false positive**: Match list patterns implicitly reference `<array>_len` variables via codegen — checker now marks them as used
- **Test runner failure details**: Failure summary now prints at the end of test-ae showing captured stderr for each failing test

## [0.31.0]

### Added

- **`ae check` command**: Type-check without compiling. Runs lexer → parser → typechecker → type inference, then exits. No C code generated, no gcc invoked, so iteration is much faster than `ae build`. Also available as `aetherc --check`
- **Unused variable warnings `[W1001]`**: Warns on declared-but-never-referenced local variables. Prefix with `_` to suppress (e.g., `_unused = 42`). Excludes function parameters and pattern bindings
- **Unreachable code warnings `[W1002]`**: Detects code after `return`, `exit()`, or exhaustive `if`/`else` blocks where all branches terminate. Recurses into nested blocks
- **Match expressions**: `msg = match x { 0 -> "ok", 1 -> "err", _ -> "unknown" }` — match can now be used as an expression on the right side of an assignment. Type is inferred from the first arm's result
- **For-in loop and match tests**: `test_for_in.ae` (range loops, variable bounds, nested), `test_match.ae` (statement match, expression match, wildcard, string/int patterns)
- **`ae build --target wasm`**: Compiles Aether to WebAssembly via Emscripten. Detects `emcc` on PATH, uses cooperative scheduler, sets `AETHER_NO_*` flags automatically. Produces `.js` + `.wasm` pair. Also reads `[build].target` from `aether.toml`
- **Actor timeouts**: `receive { Pattern -> body } after N -> { timeout_body }` fires timeout handler if no message arrives within N milliseconds. One-shot: cancelled when a message is received. `TOKEN_AFTER` keyword, `AST_TIMEOUT_ARM` node, `timeout_ns`/`last_activity_ns` fields in `ActorBase`, scheduler awareness for timeout polling
- **Cooperative preemption (opt-in)**: Two levels, both zero-cost when disabled. Scheduler-side: `AETHER_PREEMPT=1` env var enables time-based drain loop break after 1ms (configurable via `AETHER_PREEMPT_MS`). Codegen-side: `aetherc --preempt` inserts `sched_yield()` at loop back-edges with reduction counter (10000 iterations per yield). Prevents tight loops from starving other actors
- **`_aether_clock_ns` always available**: Moved nanosecond clock helper out of `#if !AETHER_GCC_COMPAT` guard so it's available on all platforms (needed by timeout checks)
- **Result types (multiple return values)**: Go-style `a, err = func()` tuple destructuring. Functions return multiple values with `return val, err`. `_` discards unwanted values. TYPE_TUPLE in type system, AST_TUPLE_DESTRUCTURE in parser, tuple struct generation in codegen. Chained error propagation works correctly across function boundaries
- **Package registry v1**: `ae add host/user/repo[@version]` downloads packages from any git host (GitHub, GitLab, Bitbucket, Codeberg, self-hosted) with optional version tags. Module resolver searches `~/.aether/packages/` for installed packages. Version stored in `aether.toml` `[dependencies]`

### Breaking Changes

- **Stdlib I/O functions** will migrate to `(value, error)` return types in a future release. Current `int` returns still work. The `error-handling.ae` example demonstrates the new pattern.

### Fixed

- **Selective imports**: `import std.math (sqrt, abs_int)` now works correctly. The prefix-stripping comparison was comparing user-facing names (`sqrt`) against C-level names (`math_sqrt`). Fixed in typechecker, codegen, and module merge. Non-selected symbols are properly rejected at both type-check and code generation time
- **Match-as-expression codegen**: Parser now attaches match as child of variable declaration. Codegen declares variable, then generates match with `var = arm_result;` in each arm. Type inference propagates arm result type to variable

## [0.30.0]

### Added

- **Platform portability layer**: Compile-time `AETHER_HAS_*` flags in `runtime/config/aether_optimization_config.h` (Tier 0) auto-detect platform capabilities and degrade gracefully. 9 feature flags: `AETHER_HAS_THREADS`, `AETHER_HAS_ATOMICS`, `AETHER_HAS_FILESYSTEM`, `AETHER_HAS_NETWORKING`, `AETHER_HAS_NUMA`, `AETHER_HAS_SIMD`, `AETHER_HAS_AFFINITY`, `AETHER_HAS_GETENV`, `AETHER_HAS_MALLOC`. Override any flag with `-DAETHER_NO_<FEATURE>` (e.g. `-DAETHER_NO_THREADING`)
- **Cooperative scheduler** (`runtime/scheduler/aether_scheduler_coop.c`): Single-threaded scheduler backend implementing the same API as the multi-core scheduler. Enables Aether programs to run on platforms without pthreads — WebAssembly (Emscripten), embedded systems, bare-metal. All actors run on core 0 via `aether_scheduler_poll()`. Multi-actor programs work cooperatively including ask/reply patterns
- **Makefile `PLATFORM` variable**: `PLATFORM=native` (default), `PLATFORM=wasm`, `PLATFORM=embedded`. Selects scheduler backend and sets appropriate `-DAETHER_NO_*` flags automatically. Also auto-detects `AETHER_NO_THREADING` in `EXTRA_CFLAGS` and switches to cooperative scheduler
- **Stdlib platform stubs**: `std/fs/`, `std/io/`, `std/os/`, `std/net/` modules return errors gracefully when `AETHER_HAS_FILESYSTEM` or `AETHER_HAS_NETWORKING` is 0. Console I/O (`print`, `println`) always works
- **C11 atomics fallback**: When `AETHER_HAS_ATOMICS == 0`, `atomic_int` → `volatile int`, `atomic_load` → identity, `atomic_store` → assignment. Safe for single-threaded builds
- **Emscripten timing fallback**: Generated code uses `emscripten_get_now()` instead of `rdtsc`/`clock_gettime` when compiled with Emscripten
- **Docker CI for cross-platform verification**: `docker/Dockerfile.wasm` (Emscripten SDK), `docker/Dockerfile.embedded` (ARM Cortex-M4 via arm-none-eabi-gcc)
- **Makefile CI targets**: `make ci-coop` (cooperative scheduler on native), `make ci-wasm` (Emscripten cross-compile + Node.js execution), `make ci-embedded` (ARM syntax-check), `make ci-portability` (all three)
- **Cooperative scheduler tests**: `test_platform_caps.ae` (multi-actor state), `test_coop_chain.ae` (4-actor message chain), `test_coop_many_actors.ae` (10 actors), `test_coop_ask_reply.ae` (synchronous ? operator with queued messages), `test_coop_self_send.ae` (self-send countdown, tick loop, concurrent self-senders), `test_coop_stubs.ae` (platform-independent operations), `test_stub_behavior.ae` (stub error handling)
- **Cooperative demo**: `examples/actors/cooperative-demo.ae` — supervisor distributing tasks to 3 workers, works identically in threaded and cooperative modes

### Fixed

- **Use-after-free in cooperative sync path**: `aether_send_message_sync()` passed a stack pointer as `msg.payload_ptr` and relied on `g_skip_free` to prevent freeing. When other messages were queued ahead in the mailbox, the immediate `step()` consumed a different message (FIFO order), and the stack-allocated message was freed later by `scheduler_wait()` after the stack frame was gone. Cooperative mode now heap-allocates message data via `malloc()`
- **`pthread_attr_t` typedef conflict on macOS/Linux**: `aether_thread.h` no-thread stubs redefined `pthread_attr_t` as `int`, conflicting with system headers. Now includes `<pthread.h>` for types on hosted platforms and uses macro redirects for no-op function stubs
- **Duplicate `g_sync_step_actor` symbol**: Defined in both `aether_scheduler_coop.c` and `aether_send_message.c`, causing linker errors. Changed to `extern` in cooperative scheduler
- **`AETHER_HAS_GETENV` operator precedence bug**: Missing parentheses around `defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 0)` caused incorrect evaluation
- **`AETHER_HAS_NUMA` incomplete**: Was only enabled on Linux, but Windows has NUMA APIs (`VirtualAllocExNuma`). Now enabled on both Linux and Windows
- **WASM computed goto crash**: Generated `static void* dispatch_table[256]` with label addresses caused LLVM WASM backend crash (`relocations for function or section offsets`). `AETHER_GCC_COMPAT` now excludes `__EMSCRIPTEN__`, using switch-case fallback for WASM
- **Missing `#include <emscripten.h>`**: Was buried inside `#if !AETHER_GCC_COMPAT` block, not reaching `rdtsc()` function. Moved to top-level includes conditional on `__EMSCRIPTEN__`
- **Dead `#include <pthread.h>` in codegen.c**: Included but never used — removed
- **`system()` return value warnings in ae.c**: 5 calls to `system()` with unchecked return values now check results and warn on failure
- **Duplicate `-lm` linker flag**: `ae` build used `-lm $(LDFLAGS)` where `LDFLAGS` already contained `-lm`

## [0.29.0]

### Added

- **`free()` builtin**: `free(ptr)` is now a language builtin for releasing heap-allocated memory. Use `defer free(ptr)` after calling stdlib functions that return malloc'd strings (`io.getenv()`, `io.read_file()`, `os.exec()`, `os.getenv()`, `file.read_all()`, `json.stringify()`, `json.get_string()`, `fs.path_join()`, etc.). Generates a clean `free((void*)ptr)` cast in the C output — no wrapper functions needed
- **Memory-safe stdlib examples**: All stdlib examples (`file-io.ae`, `io-demo.ae`, `os-demo.ae`, `json-demo.ae`) now use `defer free()` to release heap-allocated strings returned by stdlib functions

### Fixed

- **Release pipeline computed wrong next version**: `actions/checkout@v4` with `fetch-depth: 0` doesn't guarantee all tags are fetched. The prepare job's `git tag -l --sort=-version:refname` got a partial tag list, computing the wrong next version (e.g., 0.22.0 instead of 0.28.0). Added `fetch-tags: true` to prepare, bump, and tag jobs

## [0.28.0]

### Added

- **Pure Aether modules**: Write reusable `.ae` libraries without C backing files. `import mymath` loads `lib/mymath/module.ae`, and `mymath.func()` calls functions defined in pure Aether. Supports functions with type inference, constants (`const PI = 3`), and intra-module calls (module functions calling each other). Implemented via AST merge with namespace renaming — cloned module functions are inserted into the main program as regular top-level definitions, so the entire downstream pipeline (type inference, typechecking, codegen) works unchanged
- **Export visibility enforcement**: `export` keyword controls which functions and constants are part of a module's public API. `export double_it(x) { ... }` and `export const PI = 3` mark symbols as public; non-exported symbols are private — used internally by exported functions but not accessible via `module.name()` from importers. If a module has no `export` declarations, all symbols remain public (backwards compatible). Works with Aether-style functions (no `fn` keyword), `fn`-keyword functions, C-style typed functions, and constants
- **`clone_ast_node()`**: Deep-copy utility for AST nodes (used internally by the module merge system)
- **Pure module test suite**: 15 tests across `test_pure_module.ae` covering basic functions, intra-module calls, constants, constant-to-constant references, type inference through module boundaries, nested expressions, parameter/local variable shadowing, multi-module imports (mymath + strutil), and cross-module expressions
- **Export visibility test suite**: `test_export_visibility.ae` — 5 tests covering exported functions, exported constants, internal helpers, and mixed expressions. `test_export_reject.sh` — 2 compile-failure tests verifying non-exported functions and constants are rejected
- **Backwards compatibility test**: `test_noexport.ae` — 4 tests verifying modules with no `export` declarations keep all symbols public
- **Mixed import test**: `test_mixed.ae` — 5 tests verifying pure module imports work alongside `std.string` and `std.math` stdlib imports in the same program
- **Updated `examples/packages/myapp/`**: Now uses actual `import utils` with `export` visibility — `utils.double_value()`, `utils.MULTIPLIER` are public; `multiply()` is private
- **REPL rewrite**: `ae repl` rebuilt from scratch with session persistence — assignments and constants survive across evaluations, variable reassignment replaces previous value in history, multi-line blocks auto-continue until braces close. Single-line auto-execute: complete statements run immediately without needing a blank line. Retro box-drawing banner with dynamic width. Three-state prompt: `ae>` (normal), `...` (inside braces), `..` (multi-line accumulation). Commands: `:help` (with usage examples), `:quit`, `:reset`, `:show`
- **REPL integration tests**: `tests/integration/repl/test_repl.sh` — 40 tests covering basic output (integers, strings, arithmetic, negatives), variable persistence (int, string, const, derived expressions), reassignment (single and triple), string interpolation, multi-line blocks (if, if-else, nested if, while, while with accumulator), all commands (`:help`, `:h`, `:show`, `:reset`), all exit variants (`:quit`, `:q`, `exit`, `quit`), error recovery (compile errors, session continues after error, failed evals not persisted), single-line auto-execute (4 tests), and banner/goodbye messages
- **Shell test discovery in `make test-ae`**: Integration tests using `.sh` files (e.g., REPL tests, export rejection tests) are now auto-discovered and run alongside `.ae` tests

### Removed

- **Standalone REPL binary (`tools/aether_repl.c`)**: Deleted 657-line standalone REPL with readline dependency — redundant with the integrated `ae repl` which uses correct toolchain infrastructure, has fewer bugs, and requires no external dependencies

### Fixed

- **Module constants not renamed inside module functions**: When a pure module function referenced a module constant (e.g., `return x * SCALE`), the constant name was not namespace-prefixed during AST merge — generated C used the bare name `SCALE` instead of `mymath_SCALE`, causing "undeclared identifier" errors. Added `collect_module_const_names()` and extended `rename_intra_module_refs()` to rename `AST_IDENTIFIER` references to module constants alongside function calls
- **Parameter/local shadowing of module constants**: A function parameter or local variable with the same name as a module constant (e.g., `check(SCALE) { return SCALE }` where `const SCALE = 10`) had the body reference incorrectly renamed to `mymath_SCALE`, returning the constant instead of the parameter. `rename_intra_module_refs()` now collects all locally-bound names (parameters + variable declarations) per function scope via `collect_local_names()` and skips renaming identifiers that match a local name
- **Constant value expressions not renamed**: `const DOUBLE_SCALE = SCALE * 2` — the `SCALE` reference in the value expression was not namespace-prefixed during merge, causing "undeclared identifier" in generated C. Constant clones now run through `rename_intra_module_refs()` like function clones
- **Non-exported function calls produced misleading "Undefined function" error**: Calling a private function like `mathlib.internal_multiply()` reported "Undefined function 'mathlib.internal_multiply'" with help text suggesting a typo. Now reports "'internal_multiply' is not exported from module 'mathlib'" with error code E0303 and actionable help text
- **`export` keyword didn't work with Aether-style functions**: `export double_it(x) { ... }` (identifier followed by parentheses) was parsed as exporting a bare identifier `double_it`, leaving `(x) { ... }` as unparsed junk. The export parser now detects identifier-then-`(` as a function definition, and also handles `export const`, C-style return types (`export int func(...)`), and `export fn func(...)`
- **`export const` produced `VARIABLE_DECLARATION` instead of `CONST_DECLARATION`**: The export parser consumed the `const` token before calling `parse_statement`, which then saw the identifier and created a variable declaration. Fixed by letting `parse_statement` handle the `const` token directly
- **Unused function `is_type_token` compiler warning**: Removed dead code in `parser.c` — eliminated the only `-Wunused-function` warning in the compiler
- **`ae examples` and `make examples` failed on package example files**: `module.ae` (library file) and `main.ae` (uses local `import utils`) under `examples/packages/` were compiled as standalone programs by `aetherc`, which doesn't support module orchestration. Now skips files under `lib/` and `packages/` directories — these require `ae run` with full module orchestration, not bare `aetherc`

## [0.27.0]

### Added

- **Local `const` declarations**: `const` now works inside function bodies (previously only top-level). `const AGE = 5` inside `main()` emits `const int AGE = 5;` in generated C — the C compiler enforces immutability

### Fixed

- **`ae version use` didn't sync stdlib files**: Switching versions with `ae version use` updated binaries but left stale `lib/`, `include/`, and `share/` directories from previous versions — caused `string_length` to use `void*` params instead of `const char*` when a stale `module.ae` shadowed the version-managed one. Now syncs all subdirectories on version switch

## [0.26.0]

### Added

- **`std.os` module — shell & process execution** ([Issue #39](https://github.com/nicolasmd87/aether/issues/39)): New stdlib module with `os.system(cmd)` (run command, get exit code), `os.exec(cmd)` (run command, capture stdout as string), and `os.getenv(name)` (get environment variable). Cross-platform (POSIX `popen`/Windows `_popen`). Example: `examples/stdlib/os-demo.ae`, tests: `test_os_module.ae` (7 tests)
- **Release archive CI test (`test-release-archive`)**: New Makefile target and CI step [9/9] that packages a tarball exactly like the release pipeline, extracts it, and verifies `ae init` + `ae run` work from the extracted layout — catches archive structure bugs that `test-install` (which tests `install.sh`) would miss
- **Regression test for printing stdlib returns**: `test_print_stdlib_returns.ae` — covers `file.read_all`, `io.read_file`, `io.getenv` through `print()`, `println()`, and string interpolation paths
- **Filesystem return value regression test**: `test_fs_return_values.ae` — 15 tests covering `file.write`, `file.close`, `file.delete`, `dir.create`, `dir.delete`, `dir.exists` return values including success, failure, non-existent targets, duplicate operations, and NULL inputs

### Fixed

- **`ae version list` showed wrong "current" after `ae version use`**: The "current" marker was based on the compiled-in `AE_VERSION`, not the actually active version. After switching with `ae version use v0.21.0`, the list still showed v0.25.0 as current. Now reads the active version from `~/.aether/current` symlink (set by `ae version use`) or `~/.aether/active_version` file (set by `install.sh`), falling back to compiled-in version only if neither exists
- **`ae version use` didn't persist active version**: Switching versions updated the symlink and copied binaries but didn't write a version marker file. Now writes `~/.aether/active_version` so the active version is always queryable
- **Source-built installs invisible to `ae version list`**: `install.sh` installed to `~/.aether/` directly but never registered in `~/.aether/versions/`, so the source-built version never showed as "installed" or "current" in `ae version list`. Now writes `~/.aether/active_version` after install
- **Release pipeline computed next version from stale VERSION file**: The `prepare` job derived the next version by reading `VERSION` and incrementing — but if `VERSION` was stale (e.g. stuck at `0.21.0` while latest tag was `v0.25.0`), every bump PR proposed `0.22.0` instead of `0.26.0`, and the existing `release/v0.22.0` branch check caused it to skip silently. Now computes next version from the latest `v*.*.*` git tag, making the pipeline self-healing even if VERSION drifts

### Changed

- **`make ci` expanded to 9 steps**: Added `test-release-archive` as step [9/9] — every CI run now verifies both `install.sh` and release archive extraction paths end-to-end

## [0.25.0]

### Added

- **5 stdlib regression test suites (71 tests)**: Comprehensive edge-case coverage for every stdlib module:
  - `test_string_plain_char.ae` — 18 tests: every `std.string` function with plain `char*` (length, to_upper, to_lower, contains, starts_with, ends_with, index_of, substring, concat, equals, char_at, trim, split, to_cstr, release no-op, mixed managed+plain, empty string)
  - `test_stdlib_edge_cases.ae` — 25 tests: path return types and edge cases, file ops on missing files, JSON parsing edge cases, string ops on plain strings, mixed managed/plain equality
  - `test_json_edge_cases.ae` — 17 tests: escape handling (`\n`, `\t`, `\"`, `\\`, `\/`), booleans, negative numbers, floats, mixed-type arrays, type-safe getters on wrong types, stringify round-trip, nested arrays, empty strings, out-of-bounds, empty object/array creation
  - `test_io_edge_cases.ae` — 10 tests: read non-existent file, write+read round-trip, append, file_exists, delete, delete non-existent, empty content, getenv known/unknown, file_info on missing
  - `test_collections_edge_cases.ae` — 16 tests: empty list/map ops, out-of-bounds get, negative index, remove+re-add, set, clear+re-use, put+overwrite, remove non-existent key, many keys (trigger resize), all keys accessible after resize, managed string values in map

### Fixed

- **String interpolation used `%d` for ptr/string types**: `println("${file.read_all(f)}")` and similar interpolations with `TYPE_PTR` values fell through to the `%d` default in `EMIT_INTERP_FMT`, printing pointer addresses as integers instead of string content. Added `TYPE_PTR` case to format specifier switch and `_aether_safe_str()` wrapping for `TYPE_STRING`/`TYPE_PTR` in `EMIT_INTERP_ARGS`
- **`string.length()` returned garbage on plain `char*`**: `string_length("hello")` produced values like 168427553 because the `AetherString` struct layout interpreted raw bytes as the `length` field. Added `AETHER_STRING_MAGIC` (0xAE57C0DE) marker to `AetherString` struct with `is_aether_string()` runtime detection — all `std.string` functions now transparently handle both `AetherString*` and plain `char*` via `str_data()`/`str_len()` helpers. `string_retain()`/`string_release()` are safe no-ops on plain strings
- **`std/string/module.ae` param types caused `-Wincompatible-pointer-types-discards-qualifiers`**: String functions declared params as `ptr` (`void*`) but C signatures use `const void*` — codegen passed `const char*` to `void*`, triggering clang warnings. Changed all string-accepting params to `string` (`const char*`) in module.ae; return types for `string_concat`, `string_substring`, `string_to_upper`, `string_to_lower`, `string_trim` changed from `ptr` to `string` (they return `char*`)
- **`std/path/module.ae` returned `ptr` instead of `string`**: `path_join`, `path_dirname`, `path_basename`, `path_extension` declared `-> ptr` but return `char*` — fixed to `-> string`
- **`std/tcp/module.ae` declared `tcp_receive -> ptr` instead of `-> string`**: Inconsistent with `std/net/module.ae` which correctly declared `-> string`. Now both match
- **`std/string/module.ae` missing exports**: Added `string_to_long` and `string_to_double` declarations
- **JSON parser stack overflow on deeply nested input**: `parse_value`, `stringify_value`, and `json_free` recursed without depth limit — added `JSON_MAX_DEPTH` (256) guard to all three recursive paths
- **JSON parser read past end on truncated escape**: `parse_string` advanced past `\\` without checking for end of input — added `if (!**json) break;` guard
- **JSON parser missing `\/` escape**: Valid JSON escape `\/` (forward slash) was not handled — added `case '/'` to escape switch
- **JSON `parse_string` missing malloc NULL check**: `JsonValue` allocation after string parsing had no NULL check — added guard with `free(buffer)` cleanup
- **`io_read_file` / `file_read_all` missing `ftell`/`malloc` checks**: `ftell()` returning -1 was passed to `malloc()` causing undefined behavior — added `if (size < 0)` guard and malloc NULL check with proper `fclose()` cleanup
- **`file_open` missing malloc NULL check**: `malloc(sizeof(File))` failure caused NULL dereference — added check with `fclose(fp)` cleanup
- **`dir_list` unsafe realloc**: `realloc()` failure leaked original `entries` array — fixed with safe realloc pattern (temp variable, break on failure)
- **`list_add` unsafe realloc**: Same pattern — `realloc()` failure lost original `items` pointer. Fixed with temp variable
- **`hashmap_resize` unsafe calloc**: Failure overwrote map state with NULL — now allocates new buckets first, only updates map on success
- **`map_keys` on empty map**: `malloc(0)` is implementation-defined — added special case returning `keys->keys = NULL` with `count = 0`
- **`map_put` missing malloc NULL check**: `HashMapEntry` allocation had no NULL guard — added `if (!new_entry) return;`
- **`tcp_connect`/`tcp_accept`/`tcp_listen` NULL dereference on malloc failure**: All three allocated structs without checking — added NULL checks with `close(fd)` cleanup on failure
- **`tcp_receive` missing malloc NULL check**: Buffer allocation had no guard — added `if (!buffer) return NULL;`
- **HTTP `parse_url` buffer overflow**: Fixed-size `host[256]`/`path[1024]` buffers used `strcpy()`/`strncpy()` without bounds checking — refactored to pass buffer sizes and use `snprintf()`/bounded `memcpy()`
- **HTTP `http_request` missing malloc NULL checks**: `HttpResponse` and response buffer allocations had no guards — added NULL checks with proper cleanup
- **HTTP server header overflow**: Request parsing and `set_header` had no bounds check on header count — added `header_count < 50` guard

## [0.24.0]

### Fixed

- **Toolchain discovery silently used broken install**: `discover_toolchain()` accepted a `current` symlink or `AETHER_HOME` that had `aetherc` but no `lib/` or `share/aether/`, then passed non-existent source paths to the C compiler producing cryptic clang errors — now validates that sources or prebuilt lib exist before accepting a toolchain root, prints clear diagnostic ("installation is incomplete") and falls through to other strategies
- **`ae version install` extracted only one directory from release archives**: The POSIX extraction logic assumed release archives had a single wrapper directory and used `ls -d tmp/*/ | head -1` to find it — but release archives contain `bin/`, `lib/`, `share/`, `include/` at root with no wrapper. `head -1` picked only one directory (e.g. `bin/`), so `lib/libaether.a` and `share/aether/` were lost, causing "flat layout" detection and compilation failures when running `ae run` on an installed version
- **`ae version install` incomplete install detection**: Pre-existing version directories with binaries but missing `lib/` or `share/aether/` (caused by old extraction bug) were treated as complete — now detects and auto-reinstalls; also probes for `aetherc` binary and reinstalls if missing
- **Spurious "installation is incomplete" warning on `ae run`**: Toolchain discovery checked `~/.aether/current/lib/` first — if a stale `current` symlink existed from `ae version use` but `install.sh` had put files directly in `~/.aether/`, the warning fired even though the fallback strategy found a working toolchain. Now suppresses the warning when the direct `~/.aether/` layout has valid `lib/` or `share/`
- **`install.sh` left stale `current` symlink**: Direct installs via `install.sh` didn't remove the `~/.aether/current` symlink created by `ae version use`, causing the above warning on every `ae run`
- **Windows `ae version use` missing lib/share**: Only copied `bin/` subdirectory contents — now copies the entire version directory so `lib/`, `include/`, `share/` are available

## [0.23.0]

### Fixed

- **VERSION file stuck at `0.17.0`**: Release pipeline updates from 0.18.0–0.20.0 never persisted on main — corrected to `0.21.0` so the next merge triggers the `v0.21.0` release
- **Makefile version detection picked wrong tag**: `sort -t. -k1,1n` on `v0.X.0` tags tried numeric sort on the `v` prefix — behavior varies across `sort` implementations, causing some systems to pick e.g. `0.18.0` instead of `0.22.0`. Fixed by stripping the `v` prefix before sorting

## [0.22.0]

### Added

- **4 regression tests for CLI helper battle-testing**: `test_actor_print_char.ae` (print_char/escapes in actor handlers, self-send animation), `test_box_drawing.ae` (ASCII boxes, ANSI escapes, progress bars, tab tables, nested boxes), `test_interp_escape_combo.ae` (10 hex/octal + interpolation combos), `test_file_io_char_return.ae` (file I/O roundtrip, char* returns, append, cleanup)

### Fixed

- **`file.write`/`file.close`/`file.delete`/`dir.create`/`dir.delete` returned raw POSIX values**: These functions returned `0`/`-1` (C convention) instead of `1`/`0` (Aether convention where `1` = success, `0` = failure), inconsistent with `io.write_file`, `io.delete_file`, and the rest of the stdlib. Fixed all five to return `1` on success, `0` on failure

## [0.21.0]

### Fixed

- **Stdlib functions returned `AetherString*` instead of `char*`**: All stdlib modules (fs, io, json, net) returned opaque `AetherString*` pointers from functions like `file_read_all()`, `io_read_file()`, `json_stringify()`, `tcp_receive()` — but Aether's native string type is `const char*`, so codegen generated `printf("%s", ...)` which interpreted the struct pointer as a string, producing garbage output on all platforms. Changed all public stdlib APIs to return `char*` directly; module.ae declarations updated from `-> ptr` to `-> string`
- **`file_write` / `file_size` ABI mismatch**: `file_write()` C signature used `size_t length` (8 bytes on 64-bit) but module.ae declared `int` (4 bytes) — misaligned stack on ARM64. `file_size()` returned `size_t` but module declared `int`. Fixed C signatures to use `int` matching the module declarations
- **`json_stringify` crashed after `string_concat` API change**: `json_stringify` internally used `string_concat` expecting `AetherString*` return and accessed `->data` — after `string_concat` was changed to return `char*`, this dereferenced a `char*` as a struct, causing segfaults. Refactored JSON stringify to use its own `append_cstr` buffer approach, removing all dependency on `string_concat`

## [0.20.0]

### Added

- **6 new regression tests**: `test_actor_ref_message_fields.ae` (actor ref routing through messages), `test_format_specifier_typecheck.ae` (format specifier selection for all types), `test_stdlib_file_module.ae` (file module read/write), `test_stdlib_resolution.ae` (stdlib module resolution), `test_string_escape_sequences.ae` (escape sequence roundtrips), `test_win32_actor_self_scheduling.ae` (Windows actor self-scheduling)
- **2 new examples**: `actor-ref-routing.ae` (passing actor refs through messages), `self-scheduling.ae` (actor self-send patterns), `cross-platform-strings.ae` (cross-platform string handling)

### Fixed

- **Windows 11 thread compatibility**: Fixed `aether_thread.h` for Windows 11 — proper `CONDITION_VARIABLE` initialization and `SleepConditionVariableCS` usage
- **`--emit-c` flag handling**: Fixed flag parsing in `aetherc` for `--emit-c` output mode
- **Scheduler thread startup on Windows**: Added `scheduler_ensure_threads_running()` call in Windows codepath for actor self-scheduling

### Changed

- **`install.sh` improvements**: Better error handling, cleaner output, improved readline detection

## [0.19.0]

### Added

- **`--emit-c` compiler flag**: `aetherc --emit-c file.ae` prints the generated C code to stdout — useful for debugging codegen, inspecting optimizer output, and verifying MSVC compatibility guards
- **20 new integration tests** (46->66):
  - `test_print_null.ae` — 5 tests for `print`/`println` with NULL string values
  - `test_match_complex.ae` — 8 tests for match statement edge cases (NULL strings, many arms, sequential matches)
  - `test_series_long.ae` — 4 tests for series collapse optimizer with `long` (int64) types
  - `test_long_type.ae` — 7 tests for `long` type declarations, arithmetic, comparisons, and printing
  - `test_functions_advanced.ae` — 5 tests for recursive functions, nested calls, deep call chains
  - `test_while_edge_cases.ae` — 6 tests for zero-iteration loops, break, continue, nested loops
  - `test_nested_expressions.ae` — 7 tests for operator precedence, deep nesting, unary ops, if-expressions
  - `test_string_edge_cases.ae` — 7 tests for empty strings, escapes, interpolation with arithmetic
  - `test_type_coercion.ae` — 9 tests for long arithmetic, integer division, boolean logic, modulo
  - `test_control_flow.ae` — 6 tests for else-if chains, break/continue, nested loops, range-for, match default
  - `test_optimizer_booleans.ae` — 6 tests for `if true`/`if false` dead code elimination, constant folding type preservation
  - `test_reserved_words.ae` — 5 tests for C reserved word collision (functions named `double`, `auto`, `register`, `volatile`)
  - `test_edge_cases.ae` — 6 tests for nested loops, break, while-in-for, many variables, wildcard match, deep arithmetic
  - `test_actor_self_send.ae` — 4 tests for actor self-send pattern (finite loop, multiple self-sends, animation/stop, immediate exit)
  - `test_math_stdlib.ae` — 8 tests for math namespace functions (sqrt, abs, min/max, clamp, sin/cos, pow, floor/ceil/round, random)
  - `test_operator_precedence.ae` — 8 tests for operator precedence (mul/div before add/sub, parentheses, modulo, comparisons, logical ops, negation)
  - `test_string_compare_null.ae` — 3 tests for string comparison NULL safety (normal comparison, ordering, empty strings)
  - `test_nested_functions.ae` — 5 tests for nested/chained function calls (deep nesting, clamp composition, calls in conditions/arithmetic)
  - `test_logical_ops.ae` — 5 tests for logical operators (AND, OR, NOT, with comparisons, complex combinations)
  - `test_defer_advanced.ae` — 3 tests for defer edge cases (LIFO ordering, nested scopes, conditional defer)
- **3 new examples**: `recursion.ae` (factorial, fibonacci, GCD, fast exponentiation), `long-arithmetic.ae` (64-bit values, nanosecond timing, large multiplication), `string-processing.ae` (interpolation, escapes, multi-type printing)
- **17 new tests** (66->83):
  - `test_actor_communication.ae` — 4 tests for actor-to-actor messaging (bidirectional ping-pong, multi-phase wait_for_idle, actor ref in message fields)
  - `test_ask_reply.ae` — ask/reply pattern tests
  - `test_defer_loops.ae` — defer inside loops and nested scopes
  - `test_escape_hex_octal.ae` — hex (`\xNN`) and octal (`\NNN`) escape sequences
  - `test_escape_sequences.ae` — escape sequence coverage (hex, octal, interpolated strings, print_char)
  - `test_extern_functions.ae` — extern function declarations and calls
  - `test_float_operations.ae` — float arithmetic, comparisons, and printing
  - `test_io_operations.ae` — io module read/write/append/exists/delete
  - `test_list_operations.ae` — list create/add/get/size/remove operations
  - `test_map_operations.ae` — map put/get/has/remove/size operations
  - `test_pattern_guards.ae` — pattern matching with guard clauses
  - `test_print_char.ae` — print_char builtin for ASCII byte output
  - `test_print_format.ae` — print/println format specifiers for all types
  - `test_scope_shadowing.ae` — variable shadowing across scopes
  - `test_string_stdlib.ae` — string module functions (length, contains, split, etc.)
  - `test_struct_usage.ae` — struct creation, field access, nested structs
  - `test_typed_params.ae` — typed function parameters and return types
- **5 new examples**: `formatted-output.ae` (print formatting), `escape-codes.ae` (ANSI escape sequences), `ascii-art.ae` (character art with print_char), `io-demo.ae` (file I/O), `log-demo.ae` (logging module)
- **`print_char()` builtin**: `print_char(65)` emits a single byte by ASCII value — enables ANSI escape codes and character-level output without extern functions
- **Hex and octal escape sequences**: `\xNN` (hex) and `\NNN` (octal) in string literals and interpolated strings — enables ANSI terminal control codes like `"\x1b[1;32m"` directly in Aether strings

### Changed

- **Locality-aware actor placement**: Actors are now placed on the caller's core at spawn time instead of round-robin distribution. Main thread spawns default to core 0, keeping top-level actor groups co-located for efficient local messaging. Actors spawned from within actor handlers inherit the parent's core. This benefits tightly-coupled communication patterns such as ring and chain topologies where actors communicate with their immediate neighbors.
- **Aggressive message-driven migration**: Cross-core sends now set `migrate_to` to the sender's core directly, rather than migrating to the lower of the two core IDs. This produces faster convergence for communicating actor pairs.
- **Targeted migration checks**: The scheduler now checks migration hints immediately after processing messages from the coalesce buffer (O(batch_size)), in addition to the existing full actor scan during idle periods. This provides faster migration response without adding overhead to the idle scan path.
- **Non-destructive `scheduler_wait()`**: `scheduler_wait()` (backing `wait_for_idle()`) now only waits for quiescence without stopping or joining threads — programs can call `wait_for_idle()` multiple times to synchronize between phases of actor messaging. New `scheduler_shutdown()` handles final teardown (wait + stop + join) and is emitted once at program exit.
- **`Message.payload_int` widened to `intptr_t`**: Changed from `int` to `intptr_t` in the runtime Message struct — prevents 64-bit pointer truncation when actor refs are passed through single-int message fields on 64-bit platforms

### Fixed

- **Batch send buffer overflow on actor migration**: `scheduler_send_batch_flush()` re-read each actor's `assigned_core` at flush time, but the per-core counts (`by_core[]`) were recorded at `batch_add` time. If an actor migrated between buffering and flushing, the count/core mismatch caused the radix-sort to write past the end of the `sorted_actors[]` stack buffer — a stack buffer overflow confirmed by AddressSanitizer. Fixed by snapshotting `assigned_core` once per message at flush time and recomputing per-core counts from the consistent snapshot.
- **`printf("%s", NULL)` crash from `print(getenv(...))`**: Printing a NULL string (e.g. from `getenv()` on an unset variable) caused undefined behavior — now uses `_aether_safe_str()` inline helper in generated C that returns `"(null)"` for NULL pointers; helper evaluates the expression exactly once (no double-evaluation of side-effecting expressions like function calls)
- **Series collapse optimizer int32 overflow**: The closed-form formula `N*(N-1)/2` emitted by the loop optimizer overflowed for large N (e.g. 100000) because it used int32 arithmetic — now casts to `(int64_t)` for both linear-sum and constant-addend formulas; the int64 result correctly truncates back to the variable's declared type
- **Match expression evaluated multiple times**: `match (expr) { ... }` re-evaluated `expr` for every arm — if `expr` was a function call, the function ran N times with potential side effects; now emits `T _match_val = expr;` once and uses `_match_val` in all arm comparisons
- **Match `strcmp` crash on NULL strings**: String match arms used bare `strcmp()` which crashes on NULL input (e.g. `match (getenv("X")) { "a" -> ... }`) — now emits `_match_val && strcmp(_match_val, ...)` with proper NULL guard
- **Triple-evaluation of send target in actor handlers**: Inside actor receive handlers, `actor ! Msg { ... }` evaluated the target actor expression 3 times (condition check + local send + remote send) — if the target was a function call, it ran 3 times; now stores in `ActorBase* _send_target` once
- **NULL dereference in compound assignment codegen**: `stmt->children[0]->value` accessed without null check in `AST_COMPOUND_ASSIGNMENT` handler — added guard for `stmt->children[0] && stmt->children[0]->value`
- **NULL dereference on return type with `defer`**: `stmt->children[0]->node_type` could be NULL when type inference failed, causing crash in `get_c_type()` — added fallback to `int` when `node_type` is NULL or unresolved
- **`print`/`println` string literal overhead**: String literals (never NULL) were unnecessarily wrapped in `_aether_safe_str()` — literals now use `puts()` or `printf()` directly; only runtime string values go through the NULL-safe path
- **Message pool pre-allocation ignores OOM**: `message_pool_create()` called `malloc(256)` in a loop without checking return values — a failed allocation stored NULL in the pool, causing later NULL dereference; now cleans up and returns NULL on allocation failure
- **Overflow buffer out-of-bounds access**: `overflow_append(target, ...)` did not validate `target` against array bounds — an invalid core ID could write beyond `tls_overflow[MAX_CORES+1]`; now returns early with diagnostic on out-of-range target
- **Partial `realloc` failure corrupts overflow buffer**: Two sequential `realloc()` calls for `actors` and `msgs` arrays — if the second failed, `actors` pointed to the new allocation while `msgs` still pointed to the old (freed) memory; now updates `b->actors`/`b->msgs` immediately after each successful realloc so `abort()` on the second failure leaves consistent state
- **`strdup` return unchecked in message registry**: `register_message_type()` used `malloc()` and `strdup()` without checking return values — NULL results propagated silently, causing later `strcmp()` crash; now returns `-1` on allocation failure
- **Parser `is_at_end()` NULL dereference**: `peek_token()` returns NULL when past end of tokens, but `is_at_end()` immediately dereferenced it to check `->type == TOKEN_EOF` — now checks for NULL first
- **Parser `peek_ahead` negative index**: `peek_ahead(parser, -N)` could pass a negative `pos` to the token array — now returns NULL for negative positions
- **Parser direct token array access without bounds check**: Two call sites used `parser->tokens[current_token + 1]` directly — replaced with bounds-checked `peek_ahead(parser, 1)`
- **Lexer buffer overflow in error token**: Unknown characters created tokens with `&c` (pointer to single stack char) which `create_token()` passed to `strlen()`/`strcpy()` — now uses properly null-terminated 2-char array
- **Parser unchecked `realloc` in string interpolation**: Two `realloc()` calls during interpolation literal buffer growth did not check for NULL — realloc failure caused NULL pointer write; now checks and returns partial result on failure
- **Parser format string bugs**: Four `parser_message()` calls contained `%d` format specifiers without corresponding arguments — replaced with literal numbers
- **Lexer `create_token` missing malloc checks**: `malloc()` for token struct and value string not checked — now returns NULL on allocation failure
- **Parser `create_parser` missing malloc check**: `malloc(sizeof(Parser))` not checked — now returns NULL on failure
- **Type checker NULL dereference on `symbol->type`**: Module aliases have `symbol->type = NULL`, but 6 call sites in typechecker and type inference used `symbol->type` without NULL guards — crash when identifiers resolved to module aliases; added `!msg_sym->type ||` guards on send validation, `symbol->type ? ... : create_type(TYPE_UNKNOWN)` on identifier/function/compound-assignment type assignment
- **Missing semicolons in `sleep()` codegen**: Generated `Sleep()`/`usleep()` calls inside `#ifdef _WIN32`/`#else` blocks lacked semicolons — the preprocessor removed the `#endif` line leaving bare function calls without terminators; added `;` after both paths
- **Non-printable characters in string literal codegen**: Control characters (0x00-0x1F except \\n/\\t/\\r, and 0x7F) were emitted as raw bytes in generated C strings — could produce invalid C or compiler warnings; now escaped as `\\xHH`
- **Command injection in `ae test` and `ae examples`**: User-supplied directory paths passed directly to `popen(find "..." ...)` without validation — shell metacharacters in paths could execute arbitrary commands; now validates paths reject `` ` ``, `$`, `|`, `;`, `&` and other shell metacharacters
- **Unsafe `strcpy` in toolchain discovery**: `strcpy(tc.compiler, standard_paths[i])` used without bounds checking — replaced with `strncpy` + explicit null termination
- **`ftell()` failure causes `malloc(-1)` in `ae add`**: `ftell()` returns -1 on error, then `malloc(sz + 1)` with `sz=-1` causes `malloc(0)` and subsequent `fread()` with negative size (undefined behavior) — now checks `sz < 0` and returns error
- **TOML parser `strdup` NULL checks**: Three `strdup()` calls for section names and key/value pairs did not check for NULL returns — allocation failure stored NULL pointers later dereferenced by `strcmp()`; now checks all `strdup` returns and rolls back partial entries
- **TOML parser `realloc` capacity tracking**: Section capacity was incremented before `realloc()` — if `realloc` failed, the capacity variable was already wrong, causing OOB access on next insert; now only updates capacity after successful `realloc`
- **TOML parser `toml_get_value` NULL dereference**: `strcmp()` called on section/key names without NULL check — if a section had NULL name (from failed `strdup`), lookup would crash; added NULL guards in comparison loops
- **`if true { ... }` body silently eliminated by optimizer**: The dead code optimizer called `atof("true")` which returns `0.0`, treating `true` as falsy and removing the entire `if true` body — added `is_constant_condition()` helper that handles boolean literals separately from numeric constants; `true` -> truthy, `false` -> falsy
- **Constant folding always produced `TYPE_FLOAT`**: `create_numeric_literal()` unconditionally set `TYPE_FLOAT` for all folded constants, so `3 + 4` produced a float `7.0` — now takes an `is_int` parameter and preserves `TYPE_INT` when both operands are integers
- **C reserved word collision in function names**: Aether functions named `double`, `auto`, `register`, `volatile`, etc. generated invalid C because the function name is a C keyword — added `safe_c_name()` that prefixes colliding names with `ae_`; applied to function definitions, forward declarations, and call sites; `extern` functions are excluded since they refer to actual C symbols
- **AST `add_child()` silent failure on OOM**: `realloc()` failure in `add_child()` silently returned without adding the child — a corrupted AST caused unpredictable codegen; now calls `exit(1)` on allocation failure
- **Pattern variable mapping array bounds**: Guard clause codegen used a fixed `mapping[32]` array without bounds checking — more than 32 pattern variables silently overwrote the stack; now guards against overflow
- **Type checker namespace leak**: `imported_namespaces[]` strings allocated via `strdup()` were never freed on early return from `typecheck_program()` — added cleanup on all return paths
- **Extern registry unchecked `realloc`**: `register_extern_func()` updated capacity before confirming `realloc()` success — a failed realloc left capacity wrong and `extern_registry` pointing to freed memory; now only updates after success
- **Optimizer NULL checks in tail call detection**: `optimize_tail_calls()` accessed `node->children[i]` without NULL guards — a NULL child caused segfault during recursive optimization; added NULL checks before recursing
- **Runtime non-atomic message counters**: `messages_sent` and `messages_processed` in the scheduler were plain `uint64_t` read from the main thread while being written by worker threads (data race) — changed to `_Atomic uint64_t`
- **Codegen NULL dereference in `AST_IDENTIFIER`**: `strcmp(expr->value, ...)` in actor state variable lookup crashed if `expr->value` was NULL — added NULL guard before the loop
- **Codegen NULL dereference in `AST_ACTOR_REF`**: `strcmp(expr->value, "self")` crashed on NULL value — added NULL guard
- **Codegen NULL dereference in match arm iteration**: `match_arm->type` dereferenced without checking if `match_arm` was NULL — added `!match_arm ||` guard
- **Codegen NULL dereference in reply statement**: `reply_expr->value` passed to `lookup_message()` without NULL check — added `&& reply_expr->value` guard
- **Codegen NULL message name in error comments**: Two error-path `fprintf` calls used `message->value` which could be NULL — added ternary fallback to `"<?>"`
- **float/double ABI mismatch in std.math**: All 18 math functions (`sqrt`, `sin`, `cos`, `pow`, `floor`, `ceil`, `round`, `abs_float`, etc.) used C `float` with `f`-suffixed implementations (`sqrtf`, `sinf`, etc.) but Aether's `float` type maps to C `double` — caused wrong return values on ARM64 (e.g. `math.sqrt(16.0)` returned `0.0`); changed all signatures and implementations to `double`
- **float/double mismatch in `io_print_float`**: `io_print_float` took `float` parameter but received `double` from compiled Aether code — changed to `double`
- **`log_get_stats()` struct-by-value ABI mismatch**: Function returned `LogStats` struct by value but Aether's codegen expects pointer returns for non-primitive types — changed to return `LogStats*` (pointer to static storage)
- **Unary `!` operator precedence bug**: `!(a || b)` generated `!a || b` in C — the `!` only applied to the first operand because `AST_UNARY_EXPRESSION` codegen did not parenthesize complex subexpressions; now wraps binary/unary subexpressions in parentheses
- **String comparison NULL crash**: `strcmp()` in string equality/ordering codegen crashed on NULL string values — wrapped both operands with `_aether_safe_str()` to return `""` for NULL
- **io module.ae param type mismatches**: 8 functions (`io_print`, `io_print_line`, `io_read_file`, `io_write_file`, `io_append_file`, `io_file_exists`, `io_delete_file`, `io_file_info`) declared `ptr` parameters in module.ae but the C implementations take `const char*` — changed to `string` to match
- **collections module.ae map key type mismatch**: `map_put`, `map_get`, `map_has`, `map_remove` declared `ptr` for key parameter but the C implementations take `const char*` — changed to `string`
- **Lexer silently accepts unterminated strings**: Strings missing a closing `"` were lexed without error — now returns `TOKEN_ERROR` with "unterminated string literal"
- **Lexer silently accepts unterminated multi-line comments**: `/* ...` without `*/` was silently ignored — now prints error to stderr
- **Lexer missing `\0` escape sequence**: `\0` in string literals was not recognized — added null byte escape
- **Array index type not validated**: Array access like `arr["hello"]` passed through the type checker without error — added validation that array indices must be `int` or `long`
- **Extern function argument types not validated**: Calling an extern function with wrong argument types (e.g. passing `int` to a `string` parameter) produced no type error — added type validation for extern function arguments using declared parameter types
- **Type inference missing `long` (int64) arithmetic promotion**: `infer_from_binary_op()` only handled `TYPE_INT` and `TYPE_FLOAT` — mixed `int`/`long` arithmetic silently inferred as `TYPE_INT` instead of `TYPE_INT64`; now promotes to `TYPE_INT64` when either operand is int64
- **Type inference memory leak in binary expression**: `node->node_type` was overwritten without freeing the old type when reassigning from `infer_from_binary_op()` — added `free_type()` before reassignment
- **Type inference NULL guard in `has_unresolved_types`**: `ctx->constraints` could be NULL if no constraints were collected — added defensive NULL check
- **Parser match statement now supports optional parentheses**: `match val { ... }` and `match (val) { ... }` both work — parentheses are consumed if present but no longer required
- **Parser 10 unchecked `expect_token` calls**: Missing tokens (colons, parens, arrows, braces) caused the parser to continue with corrupted state — added return/break on failure in `parse_for_loop`, `parse_case_statement` (2x), `parse_match_statement` (3x), `parse_match_case`, `parse_message_definition`, `parse_reply_statement`, `parse_message_constructor`, `parse_struct_pattern` (2x)
- **Parser NULL dereference in `parse_for_loop`**: `name->value` accessed without checking if `expect_token(TOKEN_IDENTIFIER)` returned NULL — added NULL guard
- **Parser unchecked `malloc` in for-loop children**: Two `malloc(4 * sizeof(ASTNode*))` calls for for-loop child arrays did not check for NULL — OOM caused NULL pointer dereference when assigning children; now checks and returns NULL
- **`print()` not flushing stdout**: `print(".")` in a loop did not show dots immediately because `printf` buffers partial lines — now emits `fflush(stdout)` after every `print()` call in generated C
- **`self` in actor handler generated undefined `aether_self()`**: The codegen for `AST_ACTOR_REF` with value `"self"` emitted `aether_self()` which doesn't exist in the runtime — now emits `(ActorBase*)self` when inside an actor handler context
- **Actor self-send crashed in main-thread mode**: `self ! Message {}` from inside a handler in main-thread mode caused either a double-free (recursive `g_skip_free` flag corruption) or an infinite blocking loop (drain loop) or silent message loss (scheduler threads never started) — now properly transitions out of main-thread mode on self-send: disables `main_thread_mode`, clears `main_thread_only`, and starts scheduler threads on demand via `scheduler_ensure_threads_running()` so self-sent messages are processed asynchronously by the scheduler
- **`scheduler_wait()` destroyed threads after first call**: `scheduler_wait()` (backing `wait_for_idle()`) called `scheduler_stop()` + `pthread_join()`, permanently destroying scheduler threads — second call to `wait_for_idle()` hung forever because no threads existed to process messages; split into non-destructive `scheduler_wait()` (quiescence only) and `scheduler_shutdown()` (final teardown at program exit)
- **Actor refs truncated in message fields on 64-bit**: Passing an actor ref through a single-int message field (e.g. `PingWithRef { sender_ref: counter }`) truncated the 64-bit pointer to 32-bit `int` — `Message.payload_int` widened to `intptr_t`, codegen emits `intptr_t` for single-int message struct fields and proper `(intptr_t)` casts when storing actor refs
- **`void*`/`int` comparison warnings in generated C**: Comparing `list.get()` return (`void*`) with an `int` value produced C compiler warnings — codegen now auto-detects ptr/int mixed comparisons and emits `(intptr_t)` cast on the pointer side
- **`set_clear()` / `hashmap_clear()` left stale entries**: `hashmap_clear()` only set `occupied = false` per entry, leaving stale PSL, hash, key, and value data that could cause phantom entries on reinsertion — now uses `memset` to zero the entire entries array after freeing keys/values
- **`main_thread_sent` counter not reset between scheduler lifecycles**: The atomic `main_thread_sent` counter was never reset in `scheduler_init()` — stale sent count from a previous lifecycle caused `count_pending_messages()` to return permanently non-zero, spinning `scheduler_wait()` forever in C unit tests
- **Type inference not propagating parameter types through call chains**: Function parameters resolved from one call site were not propagated when the function was called from other sites — added constraint propagation pass that infers parameter types from all call sites

## [0.18.0]

### Fixed

- **Type checker did not validate message constructor field types**: Message constructors like `Greet { name: 42 }` were accepted even when the field was declared as `string` — now validates each field against the message definition's declared types
- **Missing `getpid()` header on Windows**: `<process.h>` is needed for `getpid()` on Windows, `<unistd.h>` on POSIX — added proper platform-guarded includes

## [0.17.0]

### Added

- **`exit()` builtin**: `exit(code)` terminates the program with the given exit code — no `extern` declaration needed; `exit()` with no argument defaults to 0
- **`--dump-ast` compiler flag**: `aetherc --dump-ast file.ae` prints the parsed AST tree and exits without generating C code — useful for debugging parser behavior and understanding program structure
- **`-g` debug symbols in dev builds**: `ae run` now compiles with `-g` flag, enabling `gdb`/`lldb` debugging on crashes without requiring manual gcc flags
- **`NO_COLOR` and `isatty()` support**: Error/warning output respects the `NO_COLOR` environment variable ([no-color.org](https://no-color.org/)) and automatically disables ANSI colors when stderr is not a terminal (e.g. piped to a file or CI log)
- **`null` keyword**: `null` is now a first-class literal typed as `ptr` — eliminates the need for C `null_ptr()` helpers; `x = null` and `if x == null` work as expected
- **Bitwise operators**: `&`, `|`, `^`, `~`, `<<`, `>>` with C-compatible precedence — enables bitmask flags, hash functions, and protocol parsing without C shim functions
- **Top-level constants**: `const NAME = value` at module scope — codegen emits `#define`; supports int, float, and string constant values
- **Compound assignment operators**: `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=` — emit directly to C compound assignments; work with both regular variables and actor state
- **Hex, octal, and binary numeric literals**: `0xFF`, `0o755`, `0b1010` with underscore separators (`0xFF_FF`, `0b1111_0000`) — converted to decimal at lex time so all downstream code (atoi, codegen) works unchanged
- **If-expressions**: `result = if cond { a } else { b }` produces a value — codegen emits C ternary `(cond) ? (a) : (b)`; works inline in function arguments and assignments
- **Range-based for loops**: `for i in 0..n { body }` — desugars to C-style `for (int i = start; i < end; i++)` at parse time; supports variable bounds and nests with other loop forms
- **Multi-statement arrow function bodies**: `f(x) -> { stmt1; stmt2; expr }` — the last expression is the implicit return value; supports intermediate variables, if/return, and loops inside arrow bodies

### Fixed

- **`state` keyword context-awareness**: `state` is now a regular identifier outside actor bodies — previously it was globally reserved, preventing common variable names like `state = 42` in non-actor code
- **int(0) to ptr type widening**: Variables initialized with `0` and later assigned a `ptr` value no longer cause type mismatch errors — `int` / `ptr` compatibility added for null-initialization patterns
- **Sibling if block variable scoping**: Reusing a variable name in sibling `if` blocks no longer causes "undeclared identifier" errors in generated C — each block now gets a fresh scope with `declared_var_count` properly restored after the entire if/else chain
- **String interpolation returns string pointer**: `"text ${expr}"` now produces a heap-allocated `char*` (via `snprintf` + `malloc`) instead of a `printf()` return value (`int`) — interpolated strings can now be passed to `extern` functions expecting `ptr` arguments; `print`/`println` retain the optimized `printf` path
- **`ae.c` buffer safety**: All `strncpy` calls into stack buffers now have explicit null termination; `get_exe_dir` buffer handling hardened against truncation
- **`ae.c` mixed-path separators on Windows**: `get_basename()` now handles both `/` and `\` path separators correctly, preventing incorrect toolchain discovery on Windows
- **`ae.c` source fallback completeness**: Added missing `std/io/aether_io.c` to both dev-mode and installed-mode source fallback lists; installed-mode include flags now cover all `std/` subdirectories and `share/aether/` fallback paths
- **`ae.c` temp directory consistency**: All temp file operations now use `get_temp_dir()` which checks `$TMPDIR` on POSIX and `%TEMP%` on Windows, instead of inconsistent inline checks
- **`install.sh` portability**: Changed shebang from `#!/bin/sh` to `#!/usr/bin/env bash` (script uses `local` keyword); added `cd "$(dirname "$0")"` so installer works when invoked from any directory
- **`install.sh` header completeness**: Added `std` and `std/io` directories to the header installation loop — `ae.c` generates `-I` flags for these paths
- **`install.sh` readline probe**: Uses the detected compiler (`$CC`) instead of hardcoded `gcc` for the readline compilation test
- **`install.sh` re-install handling**: Running installer again now updates the existing `AETHER_HOME` value via `sed` instead of skipping silently
- **Release archives missing headers**: Both Unix and Windows release packaging now include the `include/aether/` directory with all runtime and stdlib headers, matching the layout expected by `ae.c` in installed mode
- **Release pipeline command injection**: Commit message in the bump job is now passed via `env:` block instead of inline `${{ }}` expansion, preventing shell injection through crafted commit messages
- **`find` command quoting**: `ae test` discovery now quotes the search directory in `find` commands, preventing failures on paths with spaces
- **SPSCQueue codegen struct layout mismatch**: Codegen emitted `SPSCQueue spsc_queue` (3KB by-value) but runtime `ActorBase` uses `SPSCQueue* spsc_queue` (8-byte pointer) — caused struct layout corruption when the scheduler cast actor pointers; fixed codegen to emit pointer type
- **Codegen emits MSVC-incompatible GCC-isms**: Generated C contained bare `__thread`, `__attribute__((aligned(64)))`, `__attribute__((hot))`, and `__asm__` — replaced with portable `AETHER_TLS`, `AETHER_ALIGNED`, `AETHER_HOT`, and `AETHER_CPU_PAUSE()` macros from `aether_compiler.h`; generated code now includes `aether_compiler.h` for cross-platform builds
- **GCC statement expressions in generated C**: Three uses of `({ ... })` (clock_ns, string interpolation, ask operator) prevented MSVC compilation — each now guarded with `#if AETHER_GCC_COMPAT` with portable helper functions (`_aether_clock_ns`, `_aether_interp`, `_aether_ask_helper`) emitted in the generated preamble for the `#else` path
- **Computed goto dispatch in generated C**: Actor message dispatch used `&&label` / `goto *ptr` (GCC/Clang extension) — now guarded with `#if AETHER_GCC_COMPAT`; the `#else` path emits an equivalent `switch(_msg_id)` with `case N: goto handle_Msg;` entries; fast computed-goto path preserved for GCC/Clang
- **Message struct `__attribute__((aligned(64)))` unguarded**: Large message structs emitted bare GCC alignment attribute — now guarded with `__declspec(align(64))` for MSVC and `__attribute__((aligned(64)))` for GCC/Clang
- **`AETHER_GCC_COMPAT` macro override support**: Both the generated C preamble and `aether_compiler.h` now use `#ifndef AETHER_GCC_COMPAT` so users and tests can force a specific value via `-D` flag or early `#define`
