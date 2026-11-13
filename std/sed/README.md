# std.sed

FreeBSD's sed as a library: a file-by-file port of `usr.bin/sed` (see
`PROVENANCE.md`) on `std.posix.regex`, so scripts get POSIX Basic regular
expressions (Extended with `extended`), leftmost-longest matching and
FreeBSD's own behaviour, on every target. FreeBSD's sed test suites run
against it (`tests/integration/sed_parity/`, and `test_sed.ae` through the
API).

A compiled program is a value; every run keeps its state in its own stream,
so a program can be run any number of times, from any number of threads at
once. Errors come back as values. The module reads and writes no file of its
own accord: `r`, `w` and in-place editing go through closures the caller
grants, so it works in an `--emit=lib` build with no capabilities.

```aether,run
import std.sed

main() {
    prog = sed.compile("s/[0-9][0-9]*/<&>/g", null) or {
        println("script did not compile")
        return
    }
    out, err = sed.run(prog, "a1b22\nc333\n")
    print(out)
    sed.free(prog)

    // -n and -E are options; $!N;P;D is BSD sed's sliding window.
    o = sed.options()
    sed.quiet(o)
    sed.extended(o)
    p2 = sed.compile("/(x|xy)z/p", o) or { return }
    out2, _ = sed.run(p2, "xyz\nxz\nab\n")
    print(out2)
    sed.free(p2)
    sed.options_free(o)

    // A script error is a value, in sed's words.
    _bad, cerr = sed.compile("s/a/b", null)
    println(cerr)
}
```
```output
a<1>b<22>
c<333>
xyz
xz
1: "s/a/b
": unescaped newline inside substitute pattern
```

## Compiling

- `compile(script, opts) -> (prog, err)` compiles one script, as
  `sed 'script'` does (`opts` may be `null`).
- `compile_units(opts) -> (prog, err)` compiles the units added with
  `add_expression(o, text)` (-e) and `add_script(o, name, text)` (-f; the
  caller reads the file and passes its text), in order, as one script.
- `warnings(prog)` holds what sed would have written to standard error while
  compiling (an unused label, whitespace after a label), one per line.
- `free(prog)`.

## Options

`options()` makes a set, `options_free(o)` releases it, and a program keeps
its own copy of what it needs.

| | sed | |
|---|---|---|
| `quiet(o)` | `-n` | print only what the script prints |
| `extended(o)` | `-E`, `-r` | Extended regular expressions |
| `separate(o)` | (GNU's `-s`) | each file on its own: line numbers restart, `$` is each file's last line, the hold space and ranges reset |
| `in_place(o, suffix)` | `-i suffix` | edit `run_files`' files in place, one at a time, keeping `name + suffix` unless `suffix` is `""` |
| `in_place_span(o, suffix)` | `-I suffix` | the same, with line numbers, `$` and the hold space spanning the files |
| `delay_wfiles(o)` | `-a` | create `w` files when first written, not when the run starts |
| `line_wrap(o, n)` | `COLUMNS` | the line length `l` folds at (60 by default) |
| `grant_files(o, exists, read, write)` | | file access for `r`, `w`, `run_files` and in-place editing |

## Running

- `run(prog, input) -> (output, err)`: `sed script < input`.
- `run_files(prog, names) -> (output, err)`: `sed script file...`, with
  `names` a `*StringSeq`, read through the grant. A file that cannot be read
  is named in `err` and skipped, and the rest still run, as sed warns and
  exits 1. With `in_place` the output goes back into the files.
- Streaming, for input that arrives in pieces: `stream_new(prog)`, then
  `stream_write(st, chunk) -> (output, err)` as often as needed,
  `stream_close(st) -> (output, err)` at the end, `stream_free(st)`. A line
  runs once the byte after it has arrived, since `$` depends on it;
  `stream_done(st)` is 1 once `q`, the end of the input or an error has
  finished the run.

## Granting files

The three closures stand for the file system:

- `exists |path: string|` returns 1 when `path` can be read;
- `read |path: string|` returns its bytes;
- `write |path: string, data: string, truncate: int|` replaces the file
  with `data` (creating it) when `truncate` is 1, appends otherwise, and
  returns `""` or an error message.

They can map paths onto the real file system (`std.fs`), onto memory (a
`std.map`, as `test_sed.ae` does), or onto anything else. Without a grant
`r` reads nothing, as sed does for a file it cannot read, and a program with
a `w` command fails when its run starts.

## Behaviour

It is FreeBSD's sed, including where that differs from GNU sed: no
one-line `a text` (only `a\` and a newline), `N` on the last line quits
without printing, a label runs to the end of its line, `I` makes an address
or `s` case-insensitive, `\n`, `\t`, `\r` and `\xHH` work in patterns and
replacements. `PROVENANCE.md` lists what the library form changes.

## Tests

- `test_sed.ae`: FreeBSD's `regress.sh` and `sed2_test.sh` cases through the
  API with files in memory, byte-at-a-time streams against whole runs, and
  errors as values. Runs everywhere, Windows included.
- `tests/integration/sed_parity/`: FreeBSD's `multi_test.sh` (130 tests) and
  `regress.sh` (26) run unchanged with `aesed.ae`, a sed command line on
  this module, as `sed`; every test the host's own sed passes must pass.
  FreeBSD and macOS.

## Exports

`options`, `options_free`, `quiet`, `extended`, `separate`, `in_place`,
`in_place_span`, `delay_wfiles`, `line_wrap`, `grant_files`,
`add_expression`, `add_script`, `compile`, `compile_units`, `free`,
`warnings`, `run`, `run_files`, `stream_new`, `stream_write`,
`stream_close`, `stream_done`, `stream_free`.
