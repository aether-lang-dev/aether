- **contrib.sqlite: the authorizer, limits, extension control and typed
  rows.** `set_authorizer` / `clear_authorizer` (SQLite's authorizer, with
  the action codes and `SQLITE_DENY` / `SQLITE_IGNORE` exported), `limit`
  with the `SQLITE_LIMIT_*` constants (`SQLITE_LIMIT_ATTACHED` is 7),
  `enable_load_extension`, `column_count` / `column_name` / `column_type`
  (`SQLITE_INTEGER` .. `SQLITE_NULL`), `column_double`, `column_int64 ->
  long`, `bind_double`, `bind_int64(stmt, idx, v: long)`,
  `bind_parameter_count`, `bind_parameter_index` and `last_insert_rowid ->
  long`: what a host running someone else's SQL needs to hold a connection
  to its one file, and to return rows with their types. sae declared the
  thirteen SQLite calls itself until now (sae asks/sqlite-authorizer.md).
