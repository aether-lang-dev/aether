# REPLY: `AETHER_LIB_DIR` does not find a binary library that `--lib` finds

**Re:** aether-guest-languages `asks/ae-lib-dir-binary-library-by-file-name.md`

**RESOLVED** in branch `batch/fix-compiler` (the first release after 0.934.0
carries it). `AETHER_LIB_DIR` and `--lib` now find a binary library by the
same rule, its file name (`lib<name>.so`, `.dylib`, `.dll`, or `<name>.*`),
whatever source it was built from, so `import rexxae` finds `librexxae`
built from `src/engine.ae` either way, and the `LANG_LIB` hook can go.

What was wrong was not a lookup by source name. The `ae` driver probes its
search chain (`tc.lib_dirs`) for a binary library before the compile, in the
binary-import prepass, and `AETHER_LIB_DIR` joined that chain only later,
when the cache key was computed, so the prepass never looked in it. The
library seemed to be found when it was named after its source because the
source file `tiny.ae` sat in the directory the build ran from, and the
import resolved to that source, not to the library. A second effect of the
same ordering: in a project with `[dependencies]`, the dependency
directories were in the chain first, the variable's directories were taken
for overridden by a typed `--lib`, and the variable was ignored altogether,
for source modules too, although `ae lib-path` listed it.

The variable is now read in one place, `tc_lib_dirs_seed_from_env` in
`tools/ae.c`, once per command, when the dependencies are resolved and
before anything else joins the chain: if no `--lib` was typed, its list
takes the place a `--lib` would have. The binary-import probe, the source
probe, the cache key, `ae lib-path` and the flags the compiler is given all
read that one chain. The rule is unchanged otherwise: a typed `--lib` still
wins over the variable, and the project's `lib/` comes first.

Test: `tests/integration/binary_import_lib_dir/` builds `libother` from
`tiny.ae` with no source of either beside the program, and builds, runs and
checks `import other` with `--lib out`, `AETHER_LIB_DIR=out` and an absolute
`AETHER_LIB_DIR`, and from a project with a `[dependencies]` entry; it fails
on 0.934.0.
