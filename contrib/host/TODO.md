# contrib/host TODO

## Both host modules (stunt, aether)
- [x] Bridges self-contained: each host pushes its `perms` as a grant level of the calling thread with `aether_sandbox_push` / `aether_sandbox_pop` (runtime/aether_sandbox.h), so a bridge called inside an `enforce` block is held to both (#2955; each used to keep a process-wide stack and swap the process-wide checker pointer, which dropped the enclosing block's grants and raced other threads). No longer pokes at the compiler-emitted preamble's `static` `_aether_ctx_stack`. The aether host is separate-process and unaffected.
- [x] Import path: `import contrib.host.<name>` (not `std.host.<name>`, which the compiler's stdlib resolver does not handle, since the modules live under `contrib/host/`).
- [x] IPv6-mapped addresses normalized in `pattern_match`: a grant for `10.0.0.1` matches a TCP resource reported as `::ffff:10.0.0.1` (and vice versa), in `runtime/aether_sandbox_match.h`, which the in-process checks and the LD_PRELOAD preload checker (`runtime/libaether_sandbox_preload.c`) share.
- [x] CI + `make contrib-host-check`: syntax-checks the bridges and runs their co-located specs (`contrib/host/*/test_*.ae`). Wired into a Linux CI job (`ci-contrib-host`), which needs no language package since the language bridges moved out.
- [ ] **Deferred**, tracked in the issue tracker rather than here:
  - Capture stdout/stderr from hosted code (pipe + shared map `_stdout`/`_stderr` keys, or pass-through)
  - `string:bytes` mode for shared map — binary data without base64

## stunt (the test double)
- [x] `contrib/host/stunt`: a scriptless engine in C that a bridge loads as it
  loads an interpreter (`libstunt`, dlopen'd), or that a host spawns with
  `sandbox.spawn_socket`, with a named catalogue of checks covering what a
  bridge must handle: grant checks, the preload, Landlock and seccomp rows,
  nested levels, typed callbacks, the shared map, the cached environment,
  the instruction limit and cancel, an oversize frame. No interpreter
  package. Tests in `tests/integration/contrib_host_stunt`, run on the Linux
  and macOS lanes (and Windows, for the in-process rows that are not POSIX
  calls). See its README.
- [ ] Run stunt on FreeBSD. Its FreeBSD branches (the preload in process,
  the uncontained child, `libc.so.7`/`libm.so.5`, `syscall()`) are written
  but have never run: untested there.

## The language bridges: moved out
- [x] All of them moved to
  [aether-guest-languages](https://github.com/aether-lang-dev/aether-guest-languages)
  (`ae add github.com/aether-lang-dev/aether-guest-languages`, then
  `import <lang>`) and were deleted from this tree with no transition
  period: Lua on 2026-11-04 (copied from `d3732a0ed`), Python on 2026-11-05
  (from `9673641e6`), and on 2026-11-05 the rest, copied from `2609ec841`:
  duktape (and its alias js), quickjs, perl, ruby, tcl, go, tinygo, java,
  factor, racket and rhombus. Their tests went with them (the duktape and
  tcl bidirectional tests, the racket, rhombus and tinygo integration
  tests, and the co-located `test_host_<lang>.ae` specs), as did the
  `04-interop/05-hosts` examples, the `--with=<lang>` layers of
  `tools/docker/aether-build` and the WITH_* toggles of
  `tools/docker/Dockerfile`, and `tests/sandbox/test_shared_map_all.sh`.
  Each language's `PROVENANCE.md` there records what it was copied from.
- [x] What they still need is tracked there, held to stunt's catalogue by
  name: the #904 bidirectional tier for Ruby, Perl, Factor and Racket
  (Lua, Python, Duktape and Tcl have it), and the Perl/Ruby shared-map
  write-back, which needs XS (Perl) or a C extension (Ruby).
- [x] The engine behind the QuickJS bridge, `contrib/quickjs`, stays in this
  tree; the bridge imports it.
