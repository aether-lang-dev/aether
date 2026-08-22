# std.clapae

A command-line argument parser: a builder DSL, typed arguments, subcommands
and generated help.

Modelled on Rust's clap — the design, not the code: the builder API,
option and subcommand handling, and the help-text UX were studied and
reimplemented in idiomatic Aether. Pure Aether throughout, no C.

The shape is Aether's block-style builder: a trailing block whose setter calls
fill the object.

```aether
import std.clapae

main() {
    cmd = clapae.command("greet") {
        clapae.about("Greet someone")

        clapae.arg("name") {
            clapae.long_("name")
            clapae.string_arg()
            clapae.help("who to greet")
        }

        clapae.arg("loud") {
            clapae.long_("loud")
            clapae.flag()
        }
    }

    res, matches, err = clapae.parse(cmd)
    if err != "" {
        println("error: ${err}")
        return
    }

    println("parsed")
}
```

The example **compiles but is not run** in CI: `parse` reads the process's own
argv, so its output depends on how the example was invoked.

`long_` and `short` carry trailing underscores where the natural name is a
reserved word — the same reason `std.message` needs backticks to import.

Argument kinds are declared, not inferred: `string_arg()`, `int_arg()`,
`flag()`, `positional()`, `required()`. That is the point of declaring them —
the parser validates at the boundary, so a handler receives a value already
known to be the right type rather than a string it must re-check.

`parse` returns `(result, matches, err)`, where the result is one of
`RESULT_OK`, `RESULT_HELP` or `RESULT_ERROR`. `--help` is a *result*, not an
exit: the caller decides whether to print usage and stop. `parse_list` takes
an explicit argument list instead of reading argv, which is what makes a
parser testable.

Read values off the matches with `get_string`, `get_int` and `get_flag`, and
release with `free_matches` / `free_command`.

## Exports

`command`, `arg`, `subcommand`, `about`, `short`, `long_`, `help`,
`required`, `flag`, `string_arg`, `int_arg`, `positional`, `kind`; `parse`,
`parse_list`; `get_string`, `get_int`, `get_flag`, `subcommand_name`,
`subcommand_matches`; `print_help`, `free_command`, `free_matches`; the types
`Command`, `Arg`, `ArgMatches`; and the `KIND_*` and `RESULT_*` constants.
