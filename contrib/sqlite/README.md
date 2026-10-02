# contrib.sqlite — SQLite bindings for Aether

Thin veneer over SQLite: the pinned amalgamation compiled from source,
or the system `libsqlite3` when the amalgamation is not available (see
[Build](#build)). Lives in `contrib/` rather
than `std/` because the API surface is opinionated enough that
anchoring one shape in `std/` would force future contributors to work
around it. See [docs/stdlib-vs-contrib.md](../../docs/stdlib-vs-contrib.md)
for the rubric.

## v1 API

```aether
import contrib.sqlite

main() {
    db, err = sqlite.open(":memory:")
    if err != "" { return }
    defer sqlite.close(db)

    // DDL / INSERT / UPDATE — no rows back.
    sqlite.exec(db, "CREATE TABLE users (id INTEGER, name TEXT)")
    sqlite.exec(db, "INSERT INTO users VALUES (1, 'Alice')")
    sqlite.exec(db, "INSERT INTO users VALUES (2, 'Bob')")

    // SELECT — rows materialised into a ResultSet.
    rs, err2 = sqlite.query(db, "SELECT id, name FROM users ORDER BY id")
    if err2 != "" { return }
    defer sqlite.free(rs)

    n = sqlite.row_count(rs)
    r = 0
    while r < n {
        id   = sqlite.cell(rs, r, 0)
        name = sqlite.cell(rs, r, 1)
        println("${id}: ${name}")
        r = r + 1
    }
}
```

## v1 — convenience surface for trusted-input SQL

- `sqlite.open(path) -> (db, err)`
- `sqlite.close(db) -> err`
- `sqlite.exec(db, sql) -> err` — for DDL and no-row DML
- `sqlite.query(db, sql) -> (rs, err)` — for SELECT
- `sqlite.row_count(rs)`, `sqlite.col_count(rs)`, `sqlite.col_name(rs, i)`, `sqlite.cell(rs, row, col)`, `sqlite.free(rs)`

Every cell is returned as a `string`. Numeric columns are rendered
by SQLite's internal text conversion — callers parse them back with
`string.to_int` / `string.to_float` from `std.string` if they need
typed values.

## v2 — prepared statements + parameter binding

For SQL that takes user input (and so for *any* SQL where string
concatenation is a SQL-injection risk) use the prepared-statement
surface. Parameters are bound by 1-based index, matching SQLite's
`?N` syntax.

```aether
import contrib.sqlite

main() {
    db, _ = sqlite.open(":memory:")
    defer sqlite.close(db)

    sqlite.exec(db, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)")

    // INSERT with bound parameters.
    ins, _ = sqlite.prepare(db, "INSERT INTO users (id, name) VALUES (?, ?)")
    sqlite.bind_int (ins, 1, 1)
    sqlite.bind_text(ins, 2, "Alice")
    sqlite.step(ins, db)
    sqlite.finalize(ins)

    // SELECT with bound WHERE.
    sel, _ = sqlite.prepare(db, "SELECT name FROM users WHERE id = ?")
    sqlite.bind_int(sel, 1, 1)
    rc, _ = sqlite.step(sel, db)
    if rc == sqlite.SQLITE_ROW {
        name = sqlite.column_text(sel, 0)
        println("got: ${name}")
    }
    sqlite.finalize(sel)
}
```

**Key points:**

- `bind_text` uses `SQLITE_TRANSIENT` — SQLite copies the string immediately, so callers can drop their buffers right after the bind. Load-bearing inside loops with short-lived strings.
- `bind_blob(stmt, idx, data, len)` is binary-safe via the explicit length. AetherString-aware on the input side; embedded NULs survive.
- `bind_i64(stmt, idx, hi, lo)` splits 64-bit integers because Aether's `int` width isn't guaranteed 64-bit (it's 32-bit on MSVC). Reassemble with `((int64_t)hi << 32) | (uint32_t)lo`. Symmetric `column_i64(stmt, col) -> (hi, lo)` for the read side.
- `column_blob(stmt, col)` returns `(bytes, length, err)` — a length-aware AetherString plus byte count, same shape as `std.fs.read_binary`. Embedded NULs survive both directions.
- `step` returns `(rc, err)` where `rc` is one of the exported constants `SQLITE_ROW` (100 — row available, read columns), `SQLITE_DONE` (101 — no more rows / DML completed), or any other code (error; `err` carries `errmsg(db)` text).
- Streaming the row loop is a pure-Aether `while sqlite.step(stmt, db) == SQLITE_ROW { … }` on top of these primitives. No new C externs needed.
- `next_row(stmt, db) -> int` is cursor-iteration sugar over `step`. Returns `1` on row available, `0` on `SQLITE_DONE`, `-1` on error (call `sqlite.errmsg(db)` for text). Replaces the doubled-`step()` shape that's the most common bug in cursor APIs (forget to step at the end → infinite loop; forget to step at the start → skip row 0). Canonical use:
  ```aether
  stmt, _ = sqlite.prepare(db, "SELECT col FROM t WHERE x = ?")
  sqlite.bind_int(stmt, 1, 42)
  while sqlite.next_row(stmt, db) == 1 {
      v = sqlite.column_int(stmt, 0)
      ...
  }
  sqlite.finalize(stmt)
  ```
  `step` / `errmsg` / explicit rc compare remain available for callers that want to distinguish DONE from ROW with their own control flow.
- `finalize(stmt)` MUST be called before `close(db)`, otherwise close fails with "unable to close due to unfinalized statements".

## Still out of scope (v3 candidates)

- **`for_each_row(stmt) { … }` block-passing DSL sugar.** Needs Aether language-level support for closure-passing. The minor shape — `sqlite.next_row(stmt, db) -> int` — has shipped and removes the doubled-`step()` foot-gun, but a true block form is still future work.
- **Transactions as first-class.** `sqlite.exec(db, "BEGIN")` / `"COMMIT"` / `"ROLLBACK"` is idiomatic SQLite C API too.
- **Pragmas as named primitives.** `set_pragma(db, "journal_mode", "WAL")` is just `exec` underneath.
- **Migrations helper.** Generic enough to belong here, opinionated enough that real users (e.g. the subversion port's `wc/db_schema.ae` migration with PRAGMA introspection) hand-roll their own.
- **User-defined aggregate functions** via `sqlite3_create_function`. Niche; v4 candidate at most.

## Build

`import contrib.sqlite` is all a program needs. The module's `@link`
(`-laether_sqlite -lsqlite3 -lm`) puts the libraries on the link line, so
no `aether.toml` entry is required.

### Where SQLite comes from

SQLite is compiled from the **amalgamation** (`sqlite3.c` / `sqlite3.h`,
public domain), pinned by version and SHA-256 in
[`amalgamation.lock`](amalgamation.lock) (SQLite 3.53.4) (#1372). It is not
committed: [`scripts/fetch-sqlite-amalgamation.sh`](../../scripts/fetch-sqlite-amalgamation.sh)
downloads it, refuses it unless the checksum matches, and extracts it into
`contrib/sqlite/amalgamation/` (gitignored). Release archives ship the
extracted files, so an installed toolchain never fetches anything.

- **Native (`make contrib`).** The veneer builds into
  `build/contrib/libaether_sqlite.a`, and the amalgamation into
  `build/contrib/libsqlite3.a` beside it. `-lsqlite3` resolves to that
  archive because the `-L` for the contrib directory is searched before the
  system directories. The program carries SQLite, and the machine that runs
  it needs no `libsqlite3`. `make install-contrib` installs both archives.
  The amalgamation's object takes most of a minute to compile, so it is
  cached in `.ci-cache/sqlite/` (`SQLITE_OBJ_CACHE_DIR`), keyed by its
  source, the compiler and the flags.
- **Cross (`ae build --target=<triple>`).** The veneer and amalgamation are
  compiled for the target with zig cc, so for example
  `ae build --target=aarch64-linux-musl app.ae` links a working SQLite with
  nothing staged for the target. The amalgamation's object is cached per
  target in the ae cache (`~/.aether/cache`, or `AETHER_CACHE_DIR`). The
  first build for a target prints `Compiling SQLite for <triple>`, and later
  builds reuse the object. A `CROSSBUILD_SYSROOT` that stages
  `libaether_sqlite.a` and `libsqlite3.a` for the target still takes
  precedence.
- **System library (fallback).** When the amalgamation cannot be fetched
  (offline, no `curl`/`wget`) or `SQLITE_SYSTEM=1` is set, `make contrib`
  builds the veneer against the system `sqlite3.h` and `-lsqlite3` links the
  system library, as before. Cross builds have no such fallback: without the
  amalgamation or a staged sysroot they stop and say how to get one.

The feature flags (`SQLITE_THREADSAFE=1`, `SQLITE_ENABLE_FTS5`,
`SQLITE_ENABLE_MATH_FUNCTIONS`) live in the lock file's `SQLITE_CFLAGS` and
are part of both cache keys.

To move to a newer SQLite, change the four pinned values in
`amalgamation.lock` and run the fetch script. The `SQLITE_SHA3_256` that
sqlite.org lists on its download page is kept beside the SHA-256 for audit.

**Without the archives** (no `make contrib`): put the bridge in
`extra_sources` and link the system library.

```toml
[[bin]]
name = "myapp"
path = "src/main.ae"
extra_sources = ["contrib/sqlite/aether_sqlite.c"]

[build]
link_flags = "-lsqlite3"
```

## Installing libsqlite3 (system fallback only)

- **Debian / Ubuntu:** `apt install libsqlite3-dev`
- **Fedora / RHEL:** `dnf install sqlite-devel`
- **macOS (Homebrew):** `brew install sqlite` (already shipped; brew provides pkg-config metadata)
- **Windows (MSYS2):** `pacman -S mingw-w64-x86_64-sqlite3`
- **Alpine:** `apk add sqlite-dev`

## Test

```sh
ONLY="sqlite/" bash .github/scripts/contrib_check.sh   # the module spec, test_sqlite.ae
sh tests/integration/sqlite_vendored_native/test_sqlite_vendored_native.sh
sh tests/integration/sqlite_amalgamation_fetch/test_sqlite_amalgamation_fetch.sh
sh tests/integration/sqlite_vendored_cross/test_sqlite_vendored_cross.sh  # needs zig
```
