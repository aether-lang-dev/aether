# std.sed provenance

A port of FreeBSD's sed, `usr.bin/sed` of <https://github.com/freebsd/freebsd-src>
at commit `9d2cdb3fd5741685622b0b9c0e17e22583c9ceff` (the commit
`std/posix/regex/freebsd/` is vendored from). BSD-3-Clause; the licence and
copyright lines are at the top of `module.ae` and in `THIRD_PARTY_LICENSES.md`.

## File by file

`module.ae` keeps FreeBSD's files as sections, in their order of use:

| FreeBSD | std.sed |
|---|---|
| `defs.h` | the `SedAddr`, `SedSubst`, `SedTr`, `SedCmd` structs and the `AT_*`, `A_*`, `AP_*` constants |
| `extern.h` | the globals, now fields of `SedParse` (compiling), `SedProgram` (the result) and `SedStream` (one run) |
| `main.c` | `cu_fgets` (script units), `mf_fgets`, `lastline`, `next_files_have_lines`; `main()` and its getopt are `tests/integration/sed_parity/aesed.ae`, the parity tests' command line, since a library has none |
| `compile.c` | `compile_stream`, `compile_addr`, `compile_delimited`, `compile_ccl`, `compile_re`, `compile_subst`, `compile_flags`, `compile_tr`, `compile_text`, `duptoeol`, `dohex`, the label functions |
| `process.c` | `exec` (the body of `process()`), `applies`, `substitute`, `regsub`, `do_tr`, `lputs`, `flush_appends`, `regexec_e`, `resetstate` |
| `misc.c` | `strregerror`, which std.posix.regex's error text replaces |

## What changed, and why

A library, not a program:

- **No exit() or err().** A compile error is `(null, err)` with sed's own
  message ("1: \"s/a\n\": unterminated substitute pattern"); a run error is
  the `err` of `(output, err)`. warnx() output (an unused label, whitespace
  after a label) is kept in `warnings(prog)`. An input file that cannot be
  read is skipped and named in `err`, the rest still run, as sed warns and
  exits 1.
- **Instance state only.** The C's globals (`prog`, `linenum`, `appends`,
  `match`, `defpreg`, the hold space, the range state in each command's
  `startline`) live in a compiled `SedProgram` and a per-run `SedStream`.
  A program is not changed by running it: the range state is in the stream,
  so one program can run any number of streams.
- **Files only through a grant.** `r`, `w`, the `w` flag of `s` and in-place
  editing call the `exists`/`read`/`write` closures `grant_files` gives.
  Without a grant, `r` reads nothing (sed ignores a file it cannot read) and
  a program with `w` fails when its run starts. sed opens (truncates) `w`
  files while compiling; std.sed does it when a run starts, unless
  `delay_wfiles` (-a). In-place editing writes the backup (name + suffix)
  and then the file through `write`, where sed links and renames.
- **Input in pieces.** `mf_fgets` reads from a buffer that `stream_write`
  extends; a line runs once the byte after it has arrived (or
  `stream_close` says there is none), since `$` depends on it, and a cycle
  waiting in `n` or `N` for a line resumes at that command. `run(prog, input)`
  is one write and a close; `run_files` reads its files through the grant.
- **The command line's flags are options**: `quiet` (-n), `extended`
  (-E/-r), `in_place` (-i), `in_place_span` (-I), `delay_wfiles` (-a),
  `add_expression` (-e) and `add_script` (-f, with the text the caller
  read), plus `separate` (-s, which FreeBSD's sed lacks: each file on its
  own, as -i treats them) and `line_wrap` for `l` (sed uses `COLUMNS` or the
  terminal's width, or 60 when output is not a terminal).

Behaviour kept on purpose, because it is FreeBSD's: `N` on the last line
quits without printing; `#n` at the start of any script unit means -n; a
label runs to the end of its line, `;` included; `\` before the delimiter in
an `s` pattern drops the backslash, so `s.\..X.` matches any character;
`n` and `N` on a file's last line read on into the next file, even under -i.

Other differences:

- Script lines have no length limit (the C reads them into
  `_POSIX2_LINE_MAX` buffers and splits longer ones).
- UTF-8 is the only encoding, as in std.posix.regex: `y` maps characters,
  and `l` prints valid multibyte characters as they are, with a display
  width of 2 for the East Asian wide blocks and 1 otherwise (wcwidth()
  approximated), and every byte that is not UTF-8 in octal, as the C locale
  does.
- `-l` and `-u` (stdout buffering) have no counterpart; output is returned
  as it is produced.
