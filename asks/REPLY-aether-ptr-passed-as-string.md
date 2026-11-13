# REPLY: a `ptr` is accepted where a `string` parameter is declared

**Re:** nib-code `asks/aether-ptr-passed-as-string.md` (branch
`aether-port`)

**RESOLVED** in PR #PRNUM (branch `batch/nov13-naeb-asks`; the first
release after 0.913.0 carries it).

The ask's repro is now:

```
error[E0202]: Argument 1 'text' of 'takes': expected string, got ptr; a `ptr`
given to a `string` parameter would be read as text whatever it points at
  --> hole.ae:5:22
   help: say it is a string with `string.from_ptr(p)`
```

E0202 rather than E0200: it is the rule a `ptr` bound to a `string` local
already followed (`s = map_get_raw(m, k)`), now held at the argument too, with
the same hint (`map.get_string` / `list.get_string` when the argument is that
read, `string.from_ptr` otherwise). What still takes a `ptr`: `null`, a
`string.*` producer typed `ptr`, and an `extern`'s `string` parameter (the C
boundary, as the ask says). A call whose `_ctx` a trailing block injects is
left alone, since which argument a parameter takes there is not settled by
position.

The tightening found 63 sites in this tree, every one a `list.at` / `map.get`
/ `map.lookup` result handed on as text: std.spec's TAP and aeocha reports,
std.clapae (its argv, and `get_string` / `get_int`), std.schema's `parse`,
contrib liquid, tinyweb, i18n.collate and vkgen, and some tests. They read
with the typed accessors now. Two behaviours follow: `schema.parse` reports
an input value that is not a string instead of reading it, and
`clapae.parse` copies the process's argv (C text) into strings before parsing.

Tests: `tests/integration/ptr_string_argument` (the ask's repro, a map value,
a std function's parameter, and the accepted forms run). Expect nib-code to
meet the same error wherever a `ptr` reaches a `string` parameter; the TUI's
`jsonx.long_of` call is the one the ask found.
