# toolchain: gcc 16 `-O2` miscompiles aetherc-emitted C (IPA bit-CP folds a live `int` parameter to 0)

> **Scope: not a selaenium bug.** Grid's `registry.ae` is correct. This is the
> toolchain: on gcc 16, `aeb`'s default `-O2` (lib/aether/module.ae:1645) silently
> produces a binary in which a function parameter that genuinely varies at runtime
> is replaced by the constant `0`. Any aether program is exposed, not just Grid.

## One-line reproducer (no linking, no running, no libaether)

    aetherc --with=net,os,fs --lib grid --lib selenium_core --lib selenium_core/drivermgr \
            grid/hub.ae /tmp/hub_exe.c
    gcc -E -fwrapv $(ae cflags --cflags) /tmp/hub_exe.c -o /tmp/hub_exe.i
    gcc -O2 -S /tmp/hub_exe.i -o /tmp/bad.s     # <- inspect registry__row

In `registry__row`, the second `string_from_int` call is fed a hard zero:

    	movl	%ecx, %edi
    	call	string_from_int@PLT     # from_int(max)      -- correct
    	xorl	%edi, %edi              # from_int(inuse)    -- WRONG: %r8d discarded
    	call	string_from_int@PLT

With `-fno-ipa-bit-cp` the same source compiles correctly:

    	movl	%ecx, %edi
    	call	string_from_int@PLT     # from_int(max)
    	movl	%r12d, %edi             # from_int(inuse)    -- the real 5th argument
    	call	string_from_int@PLT

`%r8d` is the 5th SysV integer argument — `inuse`. gcc 16 dropped the parameter
entirely; the good build spills it to `%r12d` in the prologue.

## Why the constant is wrong

`registry__row(id, address, browsers, max, inuse, now_ms)` has exactly two call
sites in the translation unit:

    registry__row(id, address, browsers, max, 0, registry__now_ms())   // register()
    registry__row(i, addr, br, max, inuse, last_ms)                    // _drop_and_add()

The second carries a genuine runtime value along an unbroken chain:

    register_node_handler:  inuse = _int_field(root, "inuse")   // parsed from the POST body
      -> registry_report_inuse(ud, id, inuse)
        -> registry__set_inuse(rin, id, n)
          -> registry__drop_and_add(..., inuse, ...)
            -> registry__row(..., inuse, ...)

Meeting a constant `0` with an unknown value must yield unknown. gcc 16 yields `0`.

## Flag matrix (gcc 16.2.1, assembly-level, same `.i` every row)

| flags | `registry__row` |
|---|---|
| `-O0`, `-O1`, `-Og` | ok |
| `-O1 -fipa-cp` | ok |
| `-O1 -fipa-bit-cp` | ok |
| **`-O1 -fipa-cp -fipa-bit-cp`** | **miscompiled** |
| **`-O2` / `-O3` / `-Os`** | **miscompiled** |
| `-O2 -fno-ipa-cp` | ok |
| `-O2 -fno-ipa-bit-cp` | ok |
| `-O2 -fno-wrapv` | miscompiled |
| `-O2 -fno-inline` | miscompiled |
| `-O2 -flto` | ok |

Necessary and sufficient trigger: **`-fipa-cp` and `-fipa-bit-cp` together.**
Confirmed in both directions — removing either from `-O2` fixes it, and adding
both to a clean `-O1` breaks it.

## Cross-compiler control

Same `hub.ae`, same aetherc, byte-identical 1.78 MB emitted C, built on a second
box with **gcc 12.2.0 / glibc 2.36**: correct at `-O0`, `-O1` *and* `-O2`, 5/5 runs.
Broken box is **gcc 16.2.1 / glibc 2.44**. So it is the compiler version, not the
distro, the libc, or the CPU. The window between 12 and 16 is unbisected here
(only gcc 16 is installed on the failing box).

## Why this hid for so long

* **ASAN and UBSAN are both silent.** There is no memory error and no UB — gcc
  simply emits wrong code. Every sanitiser run reported zero errors *while the
  program was misbehaving*.
* **Every probe "fixed" it.** Adding a debug call, adding a gate, adding a second
  caller with a different constant — each changes IPA-CP's lattice, so the
  Heisenbug pattern was the bug's signature, not noise.
* **It looks like data corruption.** The symptom was a registry row losing a
  field, which reads as a use-after-free or a lost write. It is neither.

## Which knob actually matters

`aeb` does **not** compile an `aether.program()` node itself — it shells out to
`ae build`, so the `-O2` that miscompiles the hub is **aether's**, not aeb's:

    execve("/home/paul/.aether/bin/ae", ["ae", "build", ".../grid/hub.ae", "-o", ...])

`ae`'s release C flags are `-O2 -pipe -Wformat -fwrapv` (from `strings` on the
`ae` binary). Overriding the compiler through `AE_CC` fixes the real build:

    $ aeb grid/.build.ae                                  # md5 ef804b04f091 -> "inuse":0
    $ AE_CC="gcc -fno-ipa-bit-cp" aeb grid/.build.ae       # md5 07d2ddc18aa0 -> "inuse":3

(both on a cold `~/.aether/cache` — see the caching note below.)

## What we would like

1. **aether** (primary) — add `-fno-ipa-bit-cp` to the release C flags used by
   `ae build` / `aetherc`, and to the flags used to build the toolchain itself.
   An `ae`/`aetherc` binary built by gcc 16 at `-O2` is exposed to this too, as
   are aeb's own `tools/` (they are built with `ae build`). `-fno-ipa-cp` also
   works but costs more; `-fno-ipa-bit-cp` is the minimal disable. Gating on
   gcc >= 16 is fine, but unconditional is safer — the upper bound is unknown.
2. **aeb** (secondary) — `lib/aether/module.ae`'s *manual* gcc path
   (`aether_link_cmd`, the `opt_flags = "-O2"` site) compiles aetherc-emitted C
   directly and has the same exposure. Patched here with the same flag;
   `tests/test_aether_cmd.ae` expectations updated, 151/151 aeb unit tests pass.
   Not exercised end-to-end by the Grid repro, which takes the `ae build` path.
3. **gcc** — worth an upstream report. The `.i` above is self-contained and the
   check is `gcc -O2 -S` plus one grep; no reduction was attempted here.

## Side finding: the build caches do not key on the C compiler

Neither `~/.aether/cache` (used by `ae build`) nor aeb's result cache includes
the compiler identity or its flags. `AE_CC=... aeb grid/.build.ae` returned a
byte-identical binary from cache until `~/.aether/cache` was moved aside, and
`rm -rf target/` alone never forced a recompile. That is the same hazard as
aeb's existing `asks/cache-key-omits-toolchain-identity.md`, one level down: a
gcc upgrade (or a flag change like this workaround) silently keeps serving
objects built by the old compiler. Anyone verifying this fix must clear
`~/.aether/cache`, or they will measure the cache instead of the compiler.

## Verification

Grid at `7fad6ee` (unmodified `hub.ae`), POST a node with `maxSessions:7, inuse:3`
then GET `/se/grid/status`:

    aeb-built (-O2)                 -> "inuse":0     WRONG
    hand-built -O2 -fno-ipa-bit-cp  -> "inuse":3     correct   (3/3)
    hand-built -O1                  -> "inuse":3     correct
    gcc 12, -O2                     -> "inuse":3     correct   (5/5, second box)

A write/read-side dump inside `report_inuse` on the failing build shows the inputs
are all intact — `n` is 3, the table read is correct, `_find_row` returns the right
row — and only the rebuilt row carries `0`. That is the folded parameter, nothing else.
