# contrib.host.perl — Embedded Perl

`import contrib.host.perl` lets an Aether program embed Perl
in-process: `aether_perl_run_sandboxed(perms, "<source>")` evaluates
Perl with permission-checked access controls.

## How it loads libperl

The bridge `dlopen`s libperl at the **deploy host's** runtime — there
is no `-lperl` on the link line. The produced binary works against
whatever Perl 5.x minor version the deploy host has (5.30 - 5.40
supported).

Discovery order at first `aether_perl_*` call (strict two-step):

1. `${AETHER_PERL_SONAME}` env var (orchestrator-supplied exact,
   e.g. `libperl.so.5.36`).
2. `libperl.so` (Debian-style unversioned symlink; present with
   libperl-dev or sometimes the runtime alone).

If both fail, the bridge prints a clear error naming the env var.
**There is no hardcoded version fallback list** — the bridge stays
distro-agnostic; the orchestrator owns the probe.

Hint command for orchestrators / users setting the env var manually:

```sh
AETHER_PERL_SONAME=$(perl -MConfig -e 'print $Config{libperl}')
```

## What `ae build` does for you automatically

When your program has `import contrib.host.perl`, `ae build`:
1. Links `libaether_host_perl.a` (the in-tree bridge) automatically.
2. Does NOT add any libperl link flags — the bridge dlopens libperl
   at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: build the image with
`aether-build --with=perl --rebuild-image` (containerised) or
`make contrib MODULES=perl && make install-contrib` (installed
toolchain).

## `aether.toml` — usually empty for perl

```toml
# nothing required for contrib.host.perl
[[bin]]
name = "myapp"
path = "myapp.ae"
```

If the deploy host's Perl isn't discoverable via the default order,
set `AETHER_PERL_SONAME` in the build environment:

```sh
AETHER_PERL_SONAME=libperl.so.5.36 ae build myapp.ae
```

## Usage

```aether
import contrib.host.perl

main() {
    aether_perl_run("$| = 1; print \"hello from perl\\n\"")
}
```

Function names are prefixed `aether_perl_` to avoid collision with
Perl's own `perl_run`/`perl_init` symbols.

## Implementation notes

- All Perl C-API access goes through a `dlsym` function-pointer
  table — see `aether_host_perl.c`. The bridge .a has NO unresolved
  Perl symbols (`nm -u` confirms).
- Perl's headers use the `pTHX_` context-passing convention; macros
  like `eval_pv`, `SvTRUE(ERRSV)`, `SvPV_nolen(ERRSV)` expand to
  `Perl_*` functions taking the interpreter pointer as first arg.
  The bridge dlsyms those `Perl_*` functions directly and passes
  `my_perl` explicitly. **No SV-struct-layout assumptions** — all
  field access goes through the layout-agnostic `Perl_*` accessor
  functions (`Perl_sv_true`, `Perl_sv_2pv_flags`).
- `SV_GMAGIC` constant (= 2) is baked. Stable across Perl 5.x —
  same shape as Ruby's `Qnil`-as-literal in the ruby bridge.
- `ERRSV` macro replaced with `Perl_get_sv(my_perl, "@", 0)` which
  fetches the `$@` SV via Perl's symbol-table lookup (no
  `PL_errgv`-struct-field access).

## Testing

Perl has **no dedicated fib-style end-to-end test** like
[contrib.host.factor](../factor/) or
[contrib.host.racket](../racket/). Its coverage is the cross-host
shared-map test plus a contrib-build smoke check:

- [`tests/sandbox/test_shared_map_all.sh`](../../../tests/sandbox/test_shared_map_all.sh)
  exercises `aether_perl_run_sandboxed_with_map` in the same
  shared-map round-trip it runs across **all** hosts: it hands Perl a
  map (`name`/`score`), the script reads it (`aether_map_get`), writes
  a key back (`aether_map_put('grade', 'A')`), and tries to tamper a
  frozen input key — the test asserts Perl read the values and that
  the frozen input stayed **untampered**. It **SKIPs** Perl (never
  fails) when `$PERL_AVAILABLE` is 0, i.e. when no Perl is detected.
- [`tests/scripts/contrib_build.sh`](../../../tests/scripts/contrib_build.sh)
  verifies `make contrib` builds the `host_perl` archive
  (`libaether_host_perl.a`). It SKIPs the perl module when `perl
  -MExtUtils::Embed` isn't usable, and hard-fails it only under
  `make contrib MODULES=perl` (the explicit `--with=perl` path).

## Testing — co-located embedding spec

[`test_host_perl.ae`](test_host_perl.ae) sits next to this bridge and drives it
through `import contrib.host.perl` — the path a real user takes, so it covers
module.ae's declarations and the contrib-link plumbing as well as the C shim.

It asserts that a script evaluates; that the interpreter **genuinely runs**
(Perl computes a value and a second eval asserts on it Perl-side, so a
bridge that loaded `libperl` but never evaluated would not pass); that runtime
errors *and* syntax errors propagate as failures rather than being swallowed;
and that `init` is idempotent while the VM is live.

Run by `make contrib-host-check`'s `[3/3]` phase, which resolves this
runtime's soname first. Without Perl installed the specs report **skipped**
(#1610's `it_when`) rather than failing — an absent runtime is a provisioning
gap, not a code defect.

Assertions are on **return codes and Perl-side state, never on the bridge's
stdout**: the embedded interpreter buffers its own output and it is not
reliably flushed when an Aether program exits.
