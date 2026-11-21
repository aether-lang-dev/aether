# REPLY: `ae build --emit=staticlib` omits module `@source` C files

**Re:** servirtium-vcr `asks/aether-staticlib-omits-source-files.md`

**RESOLVED** on the branch `batch/runtime-quickjs-staticlib-spawntree` (a
release that carries it). Once servirtium-vcr is on such a release,
`release/swift-bundle.sh` can drop its hand-compiled QuickJS object; the
`_embed_strdup.c` bridge can go too if it is passed with `--extra` (or named
by a module's `@source`).

**Cause.** `--emit=staticlib` is built on the cross path (`run_cross_build`
in `tools/ae_cross.c`). That path collects the `--extra` files and the
`// aether-source:` lines codegen writes for each module's `@source` into one
list, and the shared-library and executable links pass that list to the
compiler. The static-library branch stops before the link: it compiled the
program's own C, then archived that object with the runtime's objects, and
never looked at the list.

**Fix.** The static-library branch now compiles each source file in the list
for the target, with the same flags as the program's object (`-fPIC`, the
optimisation level, the feature defines, the include paths), and archives the
objects with the program's. Objects are numbered, so two modules that each
ship a `util.c` both get in. An `.o` named with `--extra` is archived as it
is; an `.a` cannot be a member of an archive, so the build warns and says to
link it beside the result.

**Checked.** The ask's repro (`--target x86_64-linux-musl`): `qjs_now_ns` and
the other 48 `qjs_*` functions are defined in the archive. servirtium-vcr's
`core/embed.ae` with `swift-bundle.sh`'s flags: 15 `qjs_*` symbols undefined
in the archive with 0.931.0, none with this change.
`tests/integration/cross_staticlib_sources` builds an archive for the host's
own triple from two modules that each ship a `util.c` plus an `--extra` file,
checks `nm` sees all three functions defined, and links and runs a C program
against the archive alone.
