# `ae fmt`, the source formatter

`ae fmt` rewrites Aether source into a canonical layout: consistent 4-space
indentation, normalized spacing around operators and punctuation, at most one
blank line between constructs, no trailing whitespace, and a single final
newline. Run it before committing, or wire it into an editor-save hook or CI
check.

## Usage

```bash
ae fmt                    # read stdin, write formatted source to stdout
ae fmt path/to/file.ae    # format one file in place
ae fmt src tests          # format every .ae file under these directories, in place
ae fmt --check src        # do not write; list files that would change, exit 1 if any do
```

- With **no path**, it reads standard input and writes the formatted result to
  standard output (useful for editor "format buffer" integrations).
- With **paths**, it formats each `.ae` file in place, recursing into
  directories and skipping hidden ones (`.git`, `.aether`, ...). It prints the
  path of every file it rewrites.
- `--check` (alias `-c`) writes nothing and exits non-zero if any file is not
  already formatted, listing those files. Use it in CI to enforce formatting.

Writes are atomic (a temp file is renamed over the original), so an interrupted
run never leaves a half-written file.

## What it does, and does not, change

The formatter is deliberately conservative: it changes **only whitespace**.

It normalizes:

- **Indentation**, by brace/bracket/paren nesting (4 spaces per level).
- **Inter-token spacing**: one space around binary operators and after `,`/`:`;
  none before `,`/`;`/`:`, none inside `(`/`[` at their edges, one space before
  a `{`. Struct and message literals read `Name { field: value }`.
- **Blank lines**: runs of blank lines collapse to at most one, and a blank line
  is never left immediately after `{` or before `}`.
- **Trailing whitespace** is stripped; the file ends in exactly one newline.

It preserves, byte for byte:

- **Your line breaks.** The formatter re-indents and re-spaces but does not
  reflow expressions onto different lines. Where you break a long call or list
  is left to you.
- **Comments**, including inline `//` and multi-line `/* ... */`.
- **String literals** and their `${...}` interpolation, **heredocs**
  (`<<MARKER ... MARKER`, whose body indentation is significant), and **backtick
  raw identifiers** (`` `reply` ``).

## Safety guarantee

`ae fmt` cannot change what your program does. It works on the token stream: the
sequence of significant tokens is never reordered, dropped, or fused, and string
and comment bytes are copied verbatim. Because the lexer already tokenizes `a-b`
and `a - b` identically, re-spacing between separated tokens cannot change how
the source parses; the one hazard, gluing two tokens that would re-lex as one
(`a b` becoming `ab`, or `<` `<` becoming `<<`), is explicitly prevented.

This is verified, not just asserted: formatting every program in `examples/` and
`tests/` produces byte-identical generated C (modulo `#line` directives), and
formatting is idempotent (`fmt(fmt(x)) == fmt(x)`) across the whole corpus.

Since #1302 both properties are enforced in CI, not just described:
`tests/integration/fmt_gate/test_fmt_gate.sh` fails the build if any
checked-in `.ae` file under `std/`, `examples/`, or `tests/` is not
canonically formatted (`ae fmt --check`), if formatting stops being
idempotent, or if a formatted file's generated C diverges from the
original's (modulo `#line`). Run `ae fmt std examples tests` before
committing to stay canonical.

## Not yet done

The formatter does not reflow long lines or align consecutive declarations. Those
are layout decisions that need an opinionated style pass; they can be added later
without changing the safety model above.
