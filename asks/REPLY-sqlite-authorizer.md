# REPLY: contrib.sqlite has the authorizer, limits, extension control and typed rows

**Re:** sae `asks/sqlite-authorizer.md`

**RESOLVED** in branch `fix/win-main-closure-drain-contrib-hooks` (the
first release after 0.799.0 carries it).

Shaped as the ask proposed, Go-style wrappers over raw externs:

- `set_authorizer(db, cb: fn(ptr, int, string, string, string, string) -> int, ud: ptr) -> string`,
  plus `clear_authorizer(db)`. `SQLITE_DENY` 1 and `SQLITE_IGNORE` 2, and
  every action code (`SQLITE_COPY` 0 .. `SQLITE_RECURSIVE` 33) exported. No
  named function-pointer type: the cast you already write
  (`authorize_ as fn(ptr, int, string, string, string, string) -> int`) is
  the argument.
- `limit(db, id, val) -> int` with `SQLITE_LIMIT_LENGTH` 0 ..
  `SQLITE_LIMIT_WORKER_THREADS` 11; `SQLITE_LIMIT_ATTACHED` is 7, and a spec
  checks it against SQLite's behaviour (ATTACH fails with it at 0).
- `enable_load_extension(db, on) -> string`.
- `column_count`, `column_name`, `column_type` (`SQLITE_INTEGER` 1 ..
  `SQLITE_NULL` 5), `column_double -> float`, `column_int64 -> long`,
  `bind_double`, `bind_int64(stmt, idx, v: long)`, `bind_parameter_count`,
  `bind_parameter_index`, `last_insert_rowid -> long`. The `(hi, lo)` pair
  stays.

One difference from declaring the calls yourself: macOS's system
libsqlite3 has no `sqlite3_enable_load_extension` (it is built without
extension loading), so a veneer naming it would fail every link against
the system library. The veneer calls it when compiled against the pinned
amalgamation (`make contrib` and `ae build --target` define
`AETHER_SQLITE_VENDORED`) and on every other platform; against Apple's
library it switches the connection's own setting and reports "off" as
done, since nothing can be loaded there anyway.

Tests: five new specs in `contrib/sqlite/test_sqlite.ae` (authorizer
consulted, ATTACH and `load_extension()` refused and allowed again after
`clear_authorizer`; the limit; extension loading off; column metadata and
types, a negative 64-bit value and one past 2^53, a double, `$` `:` `@`
named parameters, `last_insert_rowid`), green against the amalgamation and
against macOS's system library.

## What sae can drop

The thirteen externs and the SQLite number constants at the top of
`services/sqlite/module.ae`, once it pins a release with this:
`sqlite.set_authorizer(db, authorize_ as fn(...) -> int, null)`,
`sqlite.limit(db, sqlite.SQLITE_LIMIT_ATTACHED, 0)`,
`sqlite.enable_load_extension(db, 0)`, and the `sqlite.column_*` /
`sqlite.bind_*` / `sqlite.last_insert_rowid` wrappers. Note the wrappers
return `""`/error strings where the raw calls returned rcs.
