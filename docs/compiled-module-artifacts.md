# Compiled module artifacts (`.aea`)

An installed toolchain ships every Aether-authored std module twice: as
source under `share/aether/std/`, and already parsed, as a compiled module
artifact under `lib/aether/modules/`. When a program imports an installed
module, the compiler takes the module's AST from its artifact instead of
lexing and parsing the text again. Nothing about the import changes:

```aether,fragment
import std.cryptography.md2
```

means the same thing, resolves to the same module, and compiles to the same C
whether the module came from its artifact or from its source (issue #1746).

## Layout

The artifact mirrors the module path, under the same prefix as the source it
was built from:

```
$PREFIX/share/aether/std/cryptography/md2/module.ae   # source (always installed)
$PREFIX/lib/aether/modules/std/cryptography/md2.aea    # its artifact

$PREFIX/share/aether/std/jsonpath/parser.ae            # a package's own file
$PREFIX/lib/aether/modules/std/jsonpath/parser.aea
```

`make install`, `install.sh` and the release archives build them with
`scripts/build_module_artifacts.sh`, using the `aetherc` being installed.
`make modules` builds the same set into `build/modules/` for inspection.

## When an artifact is used

The resolver is unchanged: an import resolves to a source path exactly as it
always has, so local sources keep their precedence. Only when that path is an
installed module (it lies under a `share/aether/` root) does the compiler
look for an artifact beside it, and it uses one only when all of these hold:

| Check | Why |
|---|---|
| Same artifact format and compiler version | an older or newer toolchain may encode differently |
| Same front-end fingerprint | a hash of the lexer, parser and AST sources, generated at build time; any change to how text becomes an AST retires every artifact made before it |
| Same install-relative source path | the artifact belongs to this module and no other |
| Same source text (FNV-1a 64 hash) | a patched or locally edited install is parsed from its edited text |
| Same `defined(NAME)` answers | a `when defined(...)` region is decided while parsing, so a build with different `-D` symbols parses the source |
| Payload intact | a truncated or damaged file is ignored |

A missing, stale or damaged artifact is never an error: the module is parsed
from source, as it would have been without artifacts. A project's own
modules, `--lib` directories, and a development tree's `std/` are always
parsed from source.

The AST an artifact yields is stamped with the installed source path, so
diagnostics, `#line` directives, `@source`/`@c_include` resolution and a
package's relative imports all behave as they do from source. The artifact is
recorded as a build input in `--emit-deps`, so `ae`'s build cache notices when
one is installed, replaced or removed.

## Controls

| Variable | Effect |
|---|---|
| `AETHER_NO_AEA=1` | ignore artifacts; parse every installed module from source |
| `AETHER_AEA_TRACE=1` | say on stderr which modules came from an artifact, and why any other was not used |

`aetherc --emit=aea <std/x/module.ae> <out.aea>`, run from the directory that
installs as `share/aether/`, writes one artifact. It refuses a module that
does not parse cleanly (errors or warnings), because an importer of the
artifact would not see those diagnostics. Further `<module.ae> <out.aea>`
pairs write more artifacts from the same process, in order, stopping at the
first module it refuses; `scripts/build_module_artifacts.sh` hands it the
std tree that way, since a process start costs far more than a parse.

## What an artifact holds

A text header of `key value` lines (format, compiler version, front-end
fingerprint, module name, source path, source hash, each `defined(NAME)`
answer the parse depended on, payload hash and length) followed by the
module's parsed AST. The header can be read with `head`:

```
AEA 2
aether_version 0.741.0
frontend 1167604807-508798
module std.cryptography.md2
source std/cryptography/md2/module.ae
source_hash 67d50d75bd9e9a9f
payload_hash d447882a6ec574a9
payload 45485
```

## Scope, and what is not done yet

The artifact is the module's parse. Type checking and code generation still
run on the importing program, because both are whole-program today: imports
are merged into one program, unreachable functions are pruned, and a
module's namespace can depend on which other modules the program loads
(#2209). A module compiled on its own would not produce the same C as the
same module inside a program, so the proposal's typed IR and native object
caches remain open, as do `docs.json` for `ae help` (which still reads the
installed source) and artifacts for contrib modules.

For scale: with all 40 non-TLS `std.cryptography` modules imported, `aetherc`
took about 190 ms per compile parsing their sources and about 160 ms reading
their artifacts. The C compile of the pruned program is unaffected.
