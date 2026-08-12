# contrib/host TODO

## All host modules
- [x] Bridges self-contained: each host maintains its own permission stack and registers its own checker via the public `_aether_sandbox_checker`. No longer pokes at the compiler-emitted preamble's `static` `_aether_ctx_stack`. Applies to python, lua, perl, ruby, js, tcl; the aether host is separate-process and unaffected. Demonstrated end-to-end in `examples/host-tcl-demo.ae` (compiles, links, runs).
- [x] Import path drift fixed: all READMEs and module.ae headers now document the working form `import contrib.host.<lang>` (previously said `std.host.<lang>`, which the compiler's stdlib resolver doesn't handle since the modules live under `contrib/host/`, not `std/host/<lang>/`). `docs/containment-sandbox.md` already used the correct form.
- [x] IPv6-mapped addresses normalized in `pattern_match`: a grant for `10.0.0.1` now matches a TCP resource reported as `::ffff:10.0.0.1` (and vice versa). Applied to all 6 in-process bridges, the LD_PRELOAD preload checker (`runtime/libaether_sandbox_preload.c`), and the Java-side `AetherGrantChecker`.
- [x] CI + `make contrib-host-check` target added: syntax-checks every bridge in stub mode and runs per-language demos when the dev library is installed. Gracefully skips languages whose headers/libs aren't present. Wired into a Linux CI job (`ci-contrib-host`) that installs lua/python/ruby/perl/tcl/duktape/go dev packages.
- [ ] **Deferred**, tracked in the issue tracker rather than here (the
  `docs/next-steps.md` this used to link to is not in the tree):
  - Capture stdout/stderr from hosted code (pipe + shared map `_stdout`/`_stderr` keys, or pass-through)
  - Shared map `aether_map_get`/`aether_map_put` bindings for Perl and Ruby currently use eval-injected hashes — outputs stay in the hosted language. Need XS (Perl) or C extension (Ruby) to write outputs back to the C map.
  - `string:bytes` mode for shared map — binary data without base64

## Python
- [x] Dedicated `examples/host-python-demo.ae` written against the current stdlib + sandbox API (the `lazy-evaluation` recovery path no longer exists, confirmed via pickaxe search across all refs).
- [x] `os.environ` is cached at CPython startup — sandbox `getenv` interception only works via `ctypes.CDLL(None).getenv`, not `os.environ.get()`. Covered in `docs/containment-sandbox.md` → host module matrix (Env cache issue row) and the "Shared-interpreter behavior" section below it. The `examples/host-python-demo.ae` demonstrates the `ctypes` workaround inline.

## Lua
- [x] Tested and working well. Cleanest host module.
- [x] **Bidirectional embedding API (#904)** — persistent host-owned VM
  (`lua.vm_new`), host-callback registration (`lua.vm_register` — an Aether
  `@c_callback` callable from script, dotted names build the prefix table),
  typed args/returns inside callbacks (`lua.arg_*` / `lua.push_*` /
  `push_tagged_table` / `raise_error`), typed `eval` result
  (`lua.vm_result_type/_int/_str/_bool`), injected globals
  (`lua.vm_set_global_str/_int/_strlist`), and an instruction-count guard
  (`lua.vm_set_instruction_limit`). This is the **template** for the rollout
  plan below. PR #906.

## Bidirectional API rollout (which other hosts get the #904 treatment)

The #904 work added a second tier on top of the fire-and-forget surface every
host shares (`init`/`run`/`run_sandboxed`/`run_sandboxed_with_map`/`finalize`):
a **persistent typed VM + host callbacks + typed marshalling both ways**. Lua
is the only host with it today. Who should get it, and how hard:

**Tier-1 candidates — in-process dlopen hosts with a C value-stack to bridge
(highest value, the Lua pattern ports directly):**

- [ ] **Python** (`contrib/host/python`) — HIGHEST value after Lua. CPython's
  C-API has the exact shapes: `PyObject_CallObject` / `PyCFunction` for host
  callbacks, `PyArg_*` + `Py_BuildValue` for typed args/returns,
  `PyRun_String` returning a `PyObject*` for typed eval, `PyDict_SetItemString`
  on `__main__` for globals, and `PyEval_SetTrace`/sys.settrace for the
  instruction guard. Marshalling table: int/float/str/bool/None/list/dict ↔
  the same typed tags Lua uses. Effort: **medium-high** (CPython refcounting
  discipline is the wrinkle — every `PyObject*` the bridge holds needs
  correct `Py_INCREF`/`DECREF`, unlike Lua's stack-GC).

- [ ] **Duktape / JS** (`contrib/host/duktape`, `js` is its alias) — HIGH
  value, LOWEST effort of the JS-family. Duktape's stack API is almost
  identical in shape to Lua's (`duk_push_*` / `duk_get_*` / `duk_get_top` /
  `duk_push_c_function` with a magic/hidden-symbol upvalue for the host fn
  ptr; `duk_pcompile`+`duk_pcall` for typed eval; `duk_put_global_string` for
  globals; `duk_set_global_object` + a custom exec-timeout via the
  `DUK_USE_EXEC_TIMEOUT_CHECK` hook). The Lua trampoline pattern (host fn ptr
  through a hidden property) maps over almost line-for-line. **Best
  second-after-Lua pick on effort/value.**

- [ ] **Tcl** (`contrib/host/tcl`) — MEDIUM value, LOW-MEDIUM effort. Tcl's C
  API is command-oriented, not stack-oriented: host callbacks register as
  `Tcl_CreateObjCommand` (the host fn reads `objv[]` Tcl_Obj args and sets the
  interp result), typed values via `Tcl_GetIntFromObj`/`Tcl_NewStringObj`/etc,
  `Tcl_Eval` + `Tcl_GetObjResult` for typed eval, `Tcl_SetVar` for globals,
  `Tcl_LimitSetCommands`/`Tcl_LimitTypeSet` for the guard. Different idiom from
  Lua but a clean 1:1 mapping; everything's already a `Tcl_Obj`.

- [ ] **Ruby** (`contrib/host/ruby`) — MEDIUM value, HIGH effort. MRI's
  embedding C-API can do it (`rb_define_global_function` with a
  `VALUE(*)(int,VALUE*,VALUE)` callback, `rb_funcall`, `rb_eval_string_protect`
  returning a `VALUE`, `rb_gv_set` for globals, `rb_add_event_hook` for the
  guard), but the GVL + `rb_protect` exception-safety + `VALUE` GC-marking make
  it the fiddliest of the in-process set. Do after Python proves the
  refcounted-host pattern.

- [ ] **Perl** (`contrib/host/perl`) — MEDIUM value, HIGH effort. Doable via
  XS-style C (`newXS` for callbacks reading the Perl stack `SP`/`ST(i)`,
  `eval_pv` returning an `SV*`, `set_sv`/`get_sv` for globals), but Perl's
  stack-macro C-API (`dSP`/`PUSHMARK`/`SPAGAIN`) is the most error-prone to
  hand-write. The TODO already notes Perl/Ruby shared-map outputs need XS/C-ext
  to write back — same underlying gap; this would close it properly.

**Tier-2 candidates — in-process but a partial head start:**

- [ ] **Factor** (`contrib/host/factor`) — already has `factor_eval -> string`,
  `factor_set`/`factor_get` over a persistent VM (the precedent #904 cited).
  The gaps vs Lua: (a) **host-callback registration** (Factor would register a
  host word that re-enters Aether), and (b) **typed** eval result instead of
  the stringified `factor_eval`. Concatenative stack model actually suits the
  push/pop callback API well. MEDIUM effort; mostly additive.

- [ ] **Racket / Rhombus** (`contrib/host/racket`, `rhombus`) — already
  `racket_eval`/`set`/`get` over the shared Chez VM. Adding callbacks means
  exposing a host procedure into the Racket namespace (`scheme_make_prim_w_arity`
  / the FFI) and a typed result reader off `Scheme_Object*`. The
  static-link + Chez-value-macro constraints (per the racket README caveats)
  make this MEDIUM-HIGH effort and lower priority — no Redis-class consumer
  asking yet.

**Out of scope — separate-process hosts (no in-process VM to bridge):**

- Java (`contrib/host/java`) — JVM is separate-process via Panama; the
  callback story is the existing shared-memory + Panama FFI, not a C value
  stack. A bidirectional surface here is a *different* design (Panama upcalls),
  not the #904 pattern. Note separately if a consumer needs it.
- Go / TinyGo (`go`, `tinygo`) — `go` is separate-subprocess; `tinygo` is
  c-shared but exposes fixed exported funcs, not a callback-registration VM.
- aether (`contrib/host/aether`) — separate-process Aether-in-Aether.

### Suggested order

1. **Duktape/JS** — lowest effort, the Lua trampoline pattern ports nearly
   verbatim; proves the rollout on a second stack-API host.
2. **Python** — highest downstream value; establishes the refcounted-host
   pattern (the template for Ruby/Perl).
3. **Tcl** — clean command-oriented mapping; low risk.
4. **Factor** — additive on its existing two-way surface.
5. **Ruby**, then **Perl** — highest effort; do last, reusing Python's
   refcount/exception-safety lessons. Closes the long-standing Perl/Ruby
   shared-map-writeback gap noted under "All host modules" above.

Each should mirror Lua's shape exactly — same `vm_*` / `arg_*` / `push_*`
naming and the same `LUA_T*`-style typed-value tag constants (rename per host,
e.g. `PY_T*`) — so a consumer that learns one host's bidirectional API knows
them all. Each needs a `tests/integration/host_<lang>_bidirectional/` test
modelled on `host_lua_bidirectional/` (callback + typed result + global inject
+ guard), SKIPping without the dev library.

## JS (Duktape)
- [x] Purest containment — no LD_PRELOAD needed; only explicitly-exposed functions are callable. Bindings: `print`, `env`, `readFile`, `fileExists`, `writeFile`, `exec`. All sandbox-checked via `check_sandbox` against the active grant list.

## Perl
- [x] Function names prefixed `aether_perl_` to avoid conflict with Perl's own `perl_run`/`perl_init`. Documented in `contrib/host/perl/README.md` Notes section.
- [x] `%ENV` scrubbed at sandbox entry. Shared interpreter means unsandboxed `run()` after sandboxed `run_sandboxed()` sees scrubbed ENV. Covered in `docs/containment-sandbox.md` → "Shared-interpreter behavior" section.
- [x] Stub-mode typo fixed: `perl_init()` → `aether_perl_init()` (the stub was calling the real libperl symbol which isn't linked when AETHER_HAS_PERL isn't defined).

## Ruby
- [x] Same `ENV` scrub issue as Perl. Covered in `docs/containment-sandbox.md` → "Shared-interpreter behavior".
- [x] `Fiddle.dlopen("libc.so.6")` succeeds but calls are still intercepted — not a real escape but looks alarming in tests. Covered in `docs/containment-sandbox.md` → "Shared-interpreter behavior".

## Java
- [x] Separate process via JVM — uses Panama FFI for shared memory, not in-process embedding. Documented in the README.
- [x] `grant_jvm_runtime()` convenience helper shipped in `contrib/host/java/module.ae`: bundles the ~29 grants the JVM needs before any application code runs (linker paths, trust stores, locale, `JAVA_*` env vars). Callers import `contrib.host.java` and invoke `java.grant_jvm_runtime()` inside a sandbox block.
- [x] IPv6-mapped address normalization — applied in `AetherGrantChecker.patternMatch` (Java side) and `libaether_sandbox_preload.c` `pattern_match` (LD_PRELOAD C side). A grant for `10.0.0.1` matches `::ffff:10.0.0.1` and vice versa.

## Go
- [x] Added as the eighth host. Separate subprocess under LD_PRELOAD sandbox interception (same pattern as `contrib/host/aether`). Two modes: `go.run_script_sandboxed(perms, path)` shells out to `go run`, `go.run_sandboxed(perms, binary)` runs a pre-built binary under tight grants. End-to-end demo: `examples/host-go-demo.ae`.

## Tcl
- [x] Added as the seventh host, mirroring the lua template. `::env` caveat (shared interpreter, same as Perl/Ruby) covered in `docs/containment-sandbox.md` → "Shared-interpreter behavior". End-to-end verified: `examples/host-tcl-demo.ae` compiles, links, and runs on macOS with system Tcl 8.5.
