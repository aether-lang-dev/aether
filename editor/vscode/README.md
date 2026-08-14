# Aether Language Support

Visual Studio Code (and Cursor) support for the
[Aether programming language](https://github.com/aether-lang-dev/aether).

## What you get

- **Syntax highlighting** for `.ae` files, scoped specifically for actor
  definitions, message types, struct types, pattern arrows (`->`),
  cons-cell patterns (`[h | t]`), and the actor send / ask operators
  (`!`, `?`).
- **Snippets** — common constructs (`actor`, `struct`, `message`, `fn`,
  `main`, `receive`, `ask`, `reply`, `match`, `for`, `if`, `import`,
  `exports`, `extern`, `defer`). Type the prefix and press Tab.
- **Fixed, red-forward palette** applied to `.ae` files regardless of
  your active VS Code colour theme — identical on Windows, macOS and
  Linux. It's a deliberate red identity (Erlang-inspired): keywords,
  types, literals, the actor verbs and the structural landmarks all live
  in the red family, with gold reserved for function definitions (the one
  anchor the eye needs) and muted green/amber/sage for comments / strings
  / numbers so code stays legible rather than monochrome.

  | Token | Colour | Style |
  |---|---|---|
  | Comments (`//`, `/* */`) | `#6A9955` (green) | italic |
  | Strings | `#CE9178` (amber) | — |
  | Numbers | `#B5CEA8` (sage) | — |
  | `true` / `false` / `null` / `self` | `#E06C75` (red) | italic |
  | Keywords (`if`, `match`, `return`, …) | `#E06C75` (red) | italic |
  | Storage (`struct`, `actor`, `func`, …) | `#E06C75` (red) | italic |
  | Primitive types (`int`, `byte`, `longdouble`, …) | `#E59CA3` (salmon) | — |
  | Actor / message names | `#E06C75` (red) | **bold** |
  | Struct / type names | `#E59CA3` (salmon) | — |
  | Annotations (`@c_import`, `@packed`, …) | `#E59CA3` (salmon) | italic |
  | Function definitions | `#DCDCAA` (gold) | **bold** |
  | `main()` | `#DCDCAA` (gold) | **bold italic** |
  | Built-in / stdlib calls | `#DCDCAA` (gold) | — |
  | Variables | `#ABB2BF` (slate) | — |
  | Arrows / actor ops (`->`, `!`, `?`, `\|`) | `#E06C75` (red) | **bold** |
  | Other operators (incl. range `..`) | `#ABB2BF` (slate) | — |
  | Brackets, accessors (`.`, `[]`) | `#5C6370` (dim slate) | — |
  | String interpolation `${…}` | `#E06C75` (red) | — |
  | String escapes (`\n`, `\\`, …) | `#D7BA7D` (mustard) | — |

  The override is bounded to `.aether`-suffixed scopes only —
  anything else in your workspace (Markdown, JSON, JS, …) keeps
  your active theme's colours. Semantic highlighting is disabled for
  `.ae` so this palette is authoritative and identical across machines.
- **File icon** — a red "ae" badge for `.ae` files.
- **Language configuration** — auto-closing brackets, comment toggle,
  smart indent, and block-comment (`/* * */`) continuation on Enter.
- **Language Server (LSP) auto-wired** — when you open a `.ae` file the
  extension finds your `aether-lsp` binary and starts a language
  client against it. No manual configuration: it tries
  `aether.lsp.path`, then the workspace's `build/aether-lsp`, then
  `PATH`, then common install dirs (`~/.local/bin`, `~/.aether/bin`,
  `/usr/local/bin`, `/opt/homebrew/bin`). Set
  `aether.lsp.enable: false` for syntax-only mode. The server writes no
  log unless you ask for one: set `AETHER_LSP_LOG=/path/to/log` in the
  environment the editor launches it from.

## Activating the palette

The palette applies automatically the moment a `.ae` file opens —
no theme switch needed. The extension ships its colours via
`editor.tokenColorCustomizations.textMateRules` scoped to
`*.aether`, which VS Code applies on top of whatever theme is
already active.

To verify it's working: open any `.ae` file and check that comments
are green-italic, keywords (`func`, `if`, `match`) are red-italic,
and function names are gold bold. If you see your theme's default
colours instead, restart VS Code (extensions are loaded once per
session).

## Installation

### From source

```bash
./editor/vscode/install.sh
```

The script:

- Reads the version straight from `package.json` so the installed
  folder name tracks the manifest.
- Removes any prior `aether-language-*` directory before copying so
  stale assets from older releases can't shadow the new ones.
- Copies the full asset set (manifest, grammar, language config,
  theme, icon-theme, both icon files, README) into
  `~/.cursor/extensions/` (Cursor) or `~/.vscode/extensions/`
  (VS Code).
- Supports an explicit override target:
  `./install.sh /path/to/extensions`.

Restart your editor after installing.

### From a `.vsix`

Standard `vsce package` workflow if you've installed `vsce` (a
follow-up will publish official releases to the marketplace; for
now the install script is the supported path).

## Language Server Protocol

The extension auto-starts an LSP client on any `.ae` file open. The
language server is now embedded in `aetherc` itself (issue #327, the
`aetherc lsp` subcommand) so a single toolchain binary is enough —
the standalone `aether-lsp` is kept as a transitional alias for
editor configs that hardcode the older name.

The resolver tries each location in order, preferring `aetherc lsp`
over `aether-lsp` when both are present:

1. The `aether.lsp.path` setting if you've set it.
2. `<workspace>/build/aetherc lsp` (or `aether-lsp` as fallback) —
   the common case for working in the Aether repo itself; just
   `make compiler` and the extension finds it.
3. `aetherc lsp` (then `aether-lsp`) resolved through your shell
   `PATH`.
4. Common install directories: `~/.local/bin`, `~/.aether/bin`,
   `/usr/local/bin`, `/opt/homebrew/bin`.

If none of those find an executable, you'll see a one-time warning
with a link to the setting; the extension stays in syntax-only mode
until you provide a path or build the server. See
[`lsp/README.md`](../../lsp/README.md) for what the server currently
supports.

## Building the extension client from source (maintainers)

The bundled `out/extension.js` is committed so end users don't need
node. If you change `src/extension.ts`, regenerate it with:

```bash
cd editor/vscode
npm install
npm run build      # esbuild bundle to out/extension.js
npm run typecheck  # tsc --noEmit, sanity check
```

## Example

```aether
import std.string

message Tick { count: int }

actor Heartbeat {
    state ticks = 0

    receive {
        Tick(count) -> {
            ticks = ticks + 1
            if ticks % 10 == 0 {
                println("heartbeat ${ticks}")
            }
        }
    }
}

main() {
    h = spawn(Heartbeat())
    i = 0
    while i < 100 {
        h ! Tick { count: i }
        i = i + 1
    }
}
```

## Requirements

- Visual Studio Code 1.60.0+ (or Cursor on the same protocol level)

## Reporting issues

Open an issue at
[github.com/aether-lang-dev/aether/issues](https://github.com/aether-lang-dev/aether/issues).

## License

MIT — see the [LICENSE](../../LICENSE) at the repo root.
