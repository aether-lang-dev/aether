# contrib.host.ruby — Embedded CRuby

`import contrib.host.ruby` lets an Aether program embed CRuby (MRI)
in-process: `ruby.run_sandboxed(perms, "<source>")` evaluates Ruby
inside the calling process, with permission-checked access controls.

## How it loads libruby

The bridge `dlopen`s libruby at the **deploy host's** runtime — there
is no `-lruby` on the link line. This means:

- The build environment doesn't need `ruby-dev` (or any libruby at
  all). Only the headers, which are pulled in via the
  `aether-build --with=ruby` image layer.
- The produced binary works against whatever Ruby minor version the
  deploy host has — no ABI lock-in.
- A binary built today on a 3.1 machine and run on a 3.2 machine
  works as long as the host has a usable libruby3.

Discovery order at first call to `ruby.run` (strict two-step):

1. `${AETHER_RUBY_SONAME}` env var (orchestrator-supplied exact
   soname, e.g. `libruby-3.1.so.3.1.2` or `libruby.so.3.4`).
2. `libruby.so` (unversioned symlink — typically only present with
   ruby-dev on Debian, sometimes runtime on Fedora-likes).

If both fail, `ruby.run*` returns -1 with a clear error naming the
env var. **There is no hardcoded version fallback list** — the
bridge stays distro-agnostic; the orchestrator owns the probe.

Hint command for orchestrators / users setting the env var manually:

```sh
AETHER_RUBY_SONAME=$(ruby -rrbconfig -e 'print RbConfig::CONFIG["LIBRUBY_SO"]')
```

## On the deploy host

You need a Ruby 3 runtime installed. Examples:

| Host | Package | Soname shipped |
|---|---|---|
| Bazzite / Fedora | `ruby` | `libruby.so` → `libruby.so.3.X` |
| Debian / Ubuntu | `libruby3.X` | `libruby-3.X.so` (Debian convention) |
| RHEL / Rocky | `ruby-libs` | `libruby.so.3.X` |

The bridge does NOT need `ruby-dev` on the deploy host. Headers are
only needed at build time (handled by `aether-build --with=ruby`).

## What `ae build` does for you automatically

When your program has `import contrib.host.ruby`, `ae build`:

1. Links `libaether_host_ruby.a` (the in-tree bridge) onto the
   resulting binary automatically. You do NOT need
   `-laether_host_ruby` in `link_flags` — the import is the trigger.
2. Does NOT add any libruby link flags — none are needed, since the
   bridge dlopens libruby at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: build the image with
`aether-build --with=ruby --rebuild-image` (containerised) or
`make contrib MODULES=ruby && make install-contrib` (installed
toolchain).

## `aether.toml` — usually empty for ruby

You typically don't need to set `cflags` or `link_flags` for
`contrib.host.ruby` at all:

```toml
# aether.toml — nothing required for contrib.host.ruby
[[bin]]
name = "myapp"
path = "myapp.ae"
```

If the deploy host's Ruby isn't discoverable via the default order,
set `AETHER_RUBY_SONAME` in the build environment:

```sh
AETHER_RUBY_SONAME=libruby-3.1.so ae build myapp.ae
```

## Usage

```aether
import contrib.host.ruby

main() {
    ruby.run("puts 'hello from ruby'")
}
```

Sandboxed:

```aether
import contrib.host.ruby
import std.list

main() {
    perms = list.new()
    list.add(perms, "fs_read")
    list.add(perms, "/etc/*")
    ruby.run_sandboxed(perms, "puts File.read('/etc/hostname')")
}
```

## Implementation notes

- All Ruby C-API access goes through a `dlsym` function-pointer
  table — see `aether_host_ruby.c`. The bridge .a has NO unresolved
  Ruby symbols (`nm -u` confirms).
- `RTLD_GLOBAL` at `dlopen` so Ruby C extensions loaded later
  resolve their Ruby refs against the same libruby instance.
- `RUBY_INIT_STACK` macro replaced with an explicit
  `g_rb.ruby_init_stack(&local_volatile_VALUE)` call on the main
  thread before `g_rb.ruby_init()`. Same effect, no macro.
- `Qnil` baked as `(VALUE)0x08` — the modern Ruby `USE_FLONUM=1`
  default constant (in effect since Ruby 2.0 / 2013). NIL_P
  comparisons use direct equality.
- `rb_intern` and `rb_funcall` are `#define`d as macros in
  `ruby/ruby.h` (rb_funcall expands to a GCC statement-expr block).
  The dlopen table fields are named `f_rb_intern` and `f_rb_funcall`
  to avoid the macro collision (same trap as `Py_None` in the python
  bridge).
- Ruby's `subst.h` `#define snprintf ruby_snprintf` is undef'd
  immediately after `#include <ruby.h>` — the bridge uses libc
  snprintf for plain string assembly, not Ruby's format extensions.

## Ruby stdout is buffered — `puts` output can vanish

libruby buffers `$stdout`, and **nothing flushes it when an Aether program
exits**. So this prints `done` and nothing else:

```aether
import contrib.host.ruby

main() {
    _ = ruby.ruby_init_host()
    _ = ruby.ruby_run("puts 'hello from ruby'")   // never appears!
    println("done")
}
```

The script *did* run — only the buffered output is lost. Confirmed by having
Ruby write a file instead: the file appears with the right contents while the
`puts` does not.

Two ways to get the output:

```aether
// 1. Finalize before exit — flushes as part of VM teardown.
ruby.ruby_finalize_host()

// 2. Or make Ruby's stdout unbuffered, if you need output as it happens.
_ = ruby.ruby_run("$stdout.sync = true")
```

This is why [`test_host_ruby.ae`](test_host_ruby.ae) asserts on **return
codes and Ruby-side state**, never on the bridge's stdout — a test that
grepped for `puts` output would be asserting on flush timing rather than on
the bridge.

## Lifecycle: finalize is terminal

`ruby_init_host()` is idempotent while the VM is live — call it defensively
before an eval and it is a no-op. But `ruby_finalize_host()` calls CRuby's
`ruby_finalize()`, which tears the VM down **permanently**: `ruby_init()`
cannot be called again in the same process.

The bridge does not currently enforce that. A post-finalize
`ruby_init_host()` returns **0**, and the next `ruby_run()` segfaults inside
libruby:

```
eval:1: [BUG] Segmentation fault at 0x0000000000000000
c:0003 p:---- s:0011 e:000010 CFUNC  :puts
```

So: **call `ruby_finalize_host()` once, at shutdown, or not at all.** A
long-running host that wants to reset script state should do it in Ruby
rather than by cycling the VM. `test_host_ruby.ae` pins the supported
lifecycle and deliberately does not exercise the unsupported one, leaving a
baseline for a future fix.

## Testing

Four tests exercise the Ruby host; all SKIP cleanly when Ruby isn't
installed, so they no-op on hosts without it.

- **Embedding lifecycle (co-located)** — [`test_host_ruby.ae`](test_host_ruby.ae),
  next to the bridge, written in Aether against `std.spec`. Drives the
  bridge through `import contrib.host.ruby` — the path a real user takes, so
  it covers the module declarations and contrib-link plumbing as well as the
  C shim. Covers evaluation, that the interpreter *really* runs (Ruby
  computes a value and a second eval asserts on it Ruby-side), exceptions and
  syntax errors propagating as failures rather than being swallowed, and init
  idempotence while the VM is live. Uses `spec.it_when` (#1610) so a box
  without Ruby reports **skipped**, not passed and not failed. Run by
  `make contrib-host-check`'s `[3/3]` phase, which performs the `LIBRUBY_SO`
  probe first.

- **Namespace / FFI SDK** —
  [`tests/integration/namespace_ruby/`](../../../tests/integration/namespace_ruby/).
  [`test_namespace_ruby.sh`](../../../tests/integration/namespace_ruby/test_namespace_ruby.sh)
  runs `ae build --namespace .` on
  [`calc.ae`](../../../tests/integration/namespace_ruby/calc.ae) (a tiny
  `double_it` / `label` / `is_positive` namespace that emits `Computed` /
  `Overflow` events), which emits both the `.so` and a
  `calc_generated_sdk.rb`. The generated SDK is loaded over Fiddle (ships
  with MRI 1.9.2+, no gem install) by
  [`check.rb`](../../../tests/integration/namespace_ruby/check.rb), which
  exercises the full surface — discovery (`describe`), input setters, event
  handlers, and function calls — asserts the round-trip values, and checks
  that the Aether script's `[ae]`-tagged stdout reaches the host.

- **Shared-map interop** —
  [`tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh).
  Its Ruby case calls `ruby_run_sandboxed_with_map` against a live
  Aether-owned shared map: the Ruby snippet reads input keys
  (`name`/`level`), writes a new `rank` key, and attempts to overwrite a
  frozen input. The test asserts the reads (`secret` is `nil`) and that the
  frozen input is untampered — the cross-host round-trip every bridge shares.

- **Build smoke** —
  [`tests/scripts/contrib_build.sh`](../../../tests/scripts/contrib_build.sh).
  `make contrib` (or `make contrib MODULES=ruby`) drives this; the `ruby`
  catalogue entry compiles `aether_host_ruby.c` and archives
  `libaether_host_ruby.a`. The `probe_ruby` step pkg-config-probes
  `ruby-3.2 / 3.1 / 3.0 / ruby` — a missing dev kit SKIPs in default mode
  but is a hard failure under explicit `MODULES=ruby`.
