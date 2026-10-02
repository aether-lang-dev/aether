/* contrib/sqlite — thin SQLite veneer for Aether.
 *
 * v1 surface (still present, unchanged):
 *   sqlite_open_raw(path)            -> sqlite3*
 *   sqlite_close_raw(db)             -> int (1 on success)
 *   sqlite_exec_raw(db, sql)         -> const char*  (NULL on success, error text on failure)
 *   sqlite_query_raw(db, sql)        -> ResultSet*   (NULL on failure)
 *   sqlite_rs_row_count(rs)          -> int
 *   sqlite_rs_col_count(rs)          -> int
 *   sqlite_rs_col_name(rs, i)        -> const char*
 *   sqlite_rs_cell(rs, row, col)     -> const char*  ("" if NULL in the DB)
 *   sqlite_rs_free(rs)               -> void
 *
 * v2 surface (additive — prepared statements + parameter binding):
 *   sqlite_prepare_raw(db, sql)              -> sqlite3_stmt*  (NULL on parse error)
 *   sqlite_bind_int_raw(stmt, idx, v)        -> int rc
 *   sqlite_bind_text_raw(stmt, idx, text)    -> int rc         (uses SQLITE_TRANSIENT)
 *   sqlite_bind_blob_raw(stmt, idx, data, n) -> int rc         (uses SQLITE_TRANSIENT)
 *   sqlite_bind_i64_raw(stmt, idx, hi, lo)   -> int rc         (split for Aether int width)
 *   sqlite_bind_null_raw(stmt, idx)          -> int rc
 *   sqlite_step_raw(stmt)                    -> int rc         (100=ROW, 101=DONE)
 *   sqlite_column_int_raw(stmt, col)         -> int
 *   sqlite_column_i64_hi_raw(stmt, col)      -> int            (top 32 bits, sign-preserving)
 *   sqlite_column_i64_lo_raw(stmt, col)      -> int            (bottom 32 bits, unsigned)
 *   sqlite_column_text_raw(stmt, col)        -> const char*    (always non-NULL; "" for SQL NULL)
 *   sqlite_try_column_blob(stmt, col)        -> int            (1 on success, 0 on NULL/OOB)
 *   sqlite_get_blob_bytes()                  -> const char*    (TLS slot from last try_)
 *   sqlite_get_blob_length()                 -> int
 *   sqlite_release_blob()                    -> void           (free early; otherwise next try_ frees)
 *   sqlite_reset_raw(stmt)                   -> int rc
 *   sqlite_finalize_raw(stmt)                -> int rc
 *   sqlite_changes_raw(db)                   -> int
 *   sqlite_errmsg_raw(db)                    -> const char*    (always non-NULL)
 *
 * SQLite itself comes from the amalgamation pinned in
 * amalgamation.lock (#1372): fetched on demand and checksum-verified,
 * never committed, so the repository stays the size docs/stdlib-vs-
 * contrib.md asks of contrib/. `make contrib` compiles it into
 * libsqlite3.a beside this veneer's archive, and `ae build --target`
 * compiles it for the target; the system libsqlite3 is the fallback
 * when it cannot be fetched. See README.md, "Build".
 *
 * Streaming row iteration is still out of scope for this version —
 * tracked as v3 in sqlite-improvement-plan.md. The v2 primitives
 * here (`prepare` + `step` + `column_*` + `finalize`) are sufficient
 * to write the streaming loop in pure Aether; a sugar helper for it
 * is additive future work.
 */

#include <sqlite3.h>
#include <stdint.h>   /* uint32_t / int32_t for sqlite_bind_i64_raw split */
#include <stdlib.h>
#include <string.h>

#include "../../std/string/aether_string.h"

/* -----------------------------------------------------------------
 * Helper: unwrap an Aether `string` param — may be an AetherString*
 * or a plain char*. Same dispatch the std/ modules added for
 * binary-safe I/O (v0.86.0 / v0.87.0 / v0.88.0).
 * ----------------------------------------------------------------- */
static inline const char* sq_str(const char* s) {
    if (!s) return NULL;
    if (is_aether_string(s)) return ((const AetherString*)s)->data;
    return s;
}

/* -----------------------------------------------------------------
 * Per-query result set. Materialised up-front: sqlite3_exec's
 * callback fills rows/cols into a flat row-major buffer, and the
 * Aether layer reads cells by (row, col). Rows are limited to
 * SQLITE_MAX_ROWS to keep a pathological SELECT from OOMing the
 * process — users who need more should switch to streaming (a
 * follow-on API).
 * ----------------------------------------------------------------- */
#define SQLITE_MAX_ROWS 100000

typedef struct ResultSet {
    int ncols;
    int nrows;
    int cap_rows;
    char** col_names;  /* [ncols] — NUL-terminated, owned */
    char** cells;      /* [nrows * ncols] — NUL-terminated, owned; may be empty "" */
    char*  err;        /* NULL on success; owned on failure path */
} ResultSet;

static int query_callback(void* user, int ncols, char** values, char** col_names) {
    ResultSet* rs = (ResultSet*)user;

    if (rs->ncols == 0) {
        rs->ncols = ncols;
        rs->col_names = (char**)calloc((size_t)ncols, sizeof(char*));
        if (!rs->col_names) return 1;
        for (int i = 0; i < ncols; i++) {
            const char* nm = col_names[i] ? col_names[i] : "";
            rs->col_names[i] = strdup(nm);
            if (!rs->col_names[i]) return 1;
        }
    } else if (rs->ncols != ncols) {
        /* Column count shifting between rows would be a sqlite bug,
         * but defensive. */
        return 1;
    }

    if (rs->nrows >= SQLITE_MAX_ROWS) return 1;

    /* Grow the cell buffer geometrically so N rows cost amortised O(N). */
    if (rs->nrows >= rs->cap_rows) {
        int new_cap = rs->cap_rows ? rs->cap_rows * 2 : 16;
        char** bigger = (char**)realloc(rs->cells, (size_t)new_cap * (size_t)ncols * sizeof(char*));
        if (!bigger) return 1;
        rs->cells = bigger;
        rs->cap_rows = new_cap;
    }

    char** row_base = rs->cells + (rs->nrows * ncols);
    for (int i = 0; i < ncols; i++) {
        const char* v = values[i] ? values[i] : "";
        row_base[i] = strdup(v);
        if (!row_base[i]) return 1;
    }
    rs->nrows++;
    return 0;
}

/* -----------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------- */

void* sqlite_open_raw(const char* path) {
    const char* p = sq_str(path);
    if (!p) return NULL;
    sqlite3* db = NULL;
    if (sqlite3_open(p, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    return (void*)db;
}

int sqlite_close_raw(void* db) {
    if (!db) return 0;
    return sqlite3_close((sqlite3*)db) == SQLITE_OK ? 1 : 0;
}

/* Returns NULL on success, a newly-allocated error string on
 * failure. Caller (the Aether wrapper) must free with free(). */
char* sqlite_exec_raw(void* db, const char* sql) {
    const char* s = sq_str(sql);
    if (!db || !s) return strdup("null db or sql");

    char* errmsg = NULL;
    int rc = sqlite3_exec((sqlite3*)db, s, NULL, NULL, &errmsg);
    if (rc == SQLITE_OK) {
        if (errmsg) sqlite3_free(errmsg);
        return NULL;
    }
    char* owned;
    if (errmsg) {
        owned = strdup(errmsg);
        sqlite3_free(errmsg);
    } else {
        owned = strdup("sqlite exec failed");
    }
    return owned;
}

void* sqlite_query_raw(void* db, const char* sql) {
    const char* s = sq_str(sql);
    if (!db || !s) return NULL;

    ResultSet* rs = (ResultSet*)calloc(1, sizeof(ResultSet));
    if (!rs) return NULL;

    char* errmsg = NULL;
    int rc = sqlite3_exec((sqlite3*)db, s, query_callback, rs, &errmsg);
    if (rc != SQLITE_OK) {
        /* Free whatever we accumulated before returning NULL so the
         * Aether wrapper can fall through to the error path. */
        if (rs->col_names) {
            for (int i = 0; i < rs->ncols; i++) free(rs->col_names[i]);
            free(rs->col_names);
        }
        if (rs->cells) {
            for (int i = 0; i < rs->nrows * rs->ncols; i++) free(rs->cells[i]);
            free(rs->cells);
        }
        free(rs);
        if (errmsg) sqlite3_free(errmsg);
        return NULL;
    }
    if (errmsg) sqlite3_free(errmsg);
    return (void*)rs;
}

int sqlite_rs_row_count(void* rs_opaque) {
    ResultSet* rs = (ResultSet*)rs_opaque;
    return rs ? rs->nrows : 0;
}

int sqlite_rs_col_count(void* rs_opaque) {
    ResultSet* rs = (ResultSet*)rs_opaque;
    return rs ? rs->ncols : 0;
}

const char* sqlite_rs_col_name(void* rs_opaque, int idx) {
    ResultSet* rs = (ResultSet*)rs_opaque;
    if (!rs || idx < 0 || idx >= rs->ncols) return "";
    return rs->col_names[idx] ? rs->col_names[idx] : "";
}

const char* sqlite_rs_cell(void* rs_opaque, int row, int col) {
    ResultSet* rs = (ResultSet*)rs_opaque;
    if (!rs || row < 0 || row >= rs->nrows) return "";
    if (col < 0 || col >= rs->ncols) return "";
    const char* v = rs->cells[row * rs->ncols + col];
    return v ? v : "";
}

void sqlite_rs_free(void* rs_opaque) {
    ResultSet* rs = (ResultSet*)rs_opaque;
    if (!rs) return;
    if (rs->col_names) {
        for (int i = 0; i < rs->ncols; i++) free(rs->col_names[i]);
        free(rs->col_names);
    }
    if (rs->cells) {
        int total = rs->nrows * rs->ncols;
        for (int i = 0; i < total; i++) free(rs->cells[i]);
        free(rs->cells);
    }
    free(rs);
}

/* =================================================================
 * v2: prepared statements + parameter binding
 * ================================================================= */

/* sqlite3_prepare_v2 returns NULL stmt on parse error. The caller's
 * Aether wrapper reads sqlite_errmsg_raw(db) for the message — the
 * error is recorded on the db handle, not the (NULL) statement. */
void* sqlite_prepare_raw(void* db, const char* sql) {
    const char* s = sq_str(sql);
    if (!db || !s) return NULL;
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2((sqlite3*)db, s, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        return NULL;
    }
    return (void*)stmt;
}

int sqlite_bind_int_raw(void* stmt, int idx, int v) {
    if (!stmt) return SQLITE_MISUSE;
    return sqlite3_bind_int((sqlite3_stmt*)stmt, idx, v);
}

/* SQLITE_TRANSIENT tells SQLite to copy the data immediately — the
 * caller can free its buffer right after the bind returns. The plan
 * doc calls this out explicitly: load-bearing for short-lived strings
 * inside loops. */
int sqlite_bind_text_raw(void* stmt, int idx, const char* text) {
    if (!stmt) return SQLITE_MISUSE;
    if (!text) return sqlite3_bind_null((sqlite3_stmt*)stmt, idx);
    const char* unwrapped;
    int len;
    if (is_aether_string(text)) {
        const AetherString* as = (const AetherString*)text;
        unwrapped = as->data;
        len = (int)as->length;
    } else {
        unwrapped = text;
        len = -1;  /* SQLite measures via strlen */
    }
    return sqlite3_bind_text((sqlite3_stmt*)stmt, idx, unwrapped, len, SQLITE_TRANSIENT);
}

/* Blob bind: explicit length (binary-safe, embedded NULs OK). data
 * may be an AetherString or a plain char*; the AetherString path
 * lets callers feed in fs.read_binary output directly. */
int sqlite_bind_blob_raw(void* stmt, int idx, const char* data, int len) {
    if (!stmt) return SQLITE_MISUSE;
    if (len < 0) return SQLITE_MISUSE;
    if (len == 0) {
        return sqlite3_bind_zeroblob((sqlite3_stmt*)stmt, idx, 0);
    }
    const char* bytes;
    if (is_aether_string(data)) {
        bytes = ((const AetherString*)data)->data;
    } else {
        bytes = data;
    }
    if (!bytes) return SQLITE_MISUSE;
    return sqlite3_bind_blob((sqlite3_stmt*)stmt, idx, bytes, len, SQLITE_TRANSIENT);
}

/* 64-bit int bind: split as (hi, lo) because Aether's `int` width
 * isn't guaranteed 64-bit (32-bit on MSVC; the std.string.from_long
 * pair already uses this idiom). hi is signed (carries the sign
 * bit), lo is treated as unsigned 32-bit when reassembling. */
int sqlite_bind_i64_raw(void* stmt, int idx, int hi, int lo) {
    if (!stmt) return SQLITE_MISUSE;
    sqlite3_int64 v = ((sqlite3_int64)(int32_t)hi << 32)
                    | ((sqlite3_int64)(uint32_t)lo);
    return sqlite3_bind_int64((sqlite3_stmt*)stmt, idx, v);
}

int sqlite_bind_null_raw(void* stmt, int idx) {
    if (!stmt) return SQLITE_MISUSE;
    return sqlite3_bind_null((sqlite3_stmt*)stmt, idx);
}

/* step returns the SQLite rc directly: 100=SQLITE_ROW, 101=SQLITE_DONE
 * are the success cases; anything else is a real error and the
 * caller's Aether wrapper reads sqlite_errmsg_raw(db) for detail. */
int sqlite_step_raw(void* stmt) {
    if (!stmt) return SQLITE_MISUSE;
    return sqlite3_step((sqlite3_stmt*)stmt);
}

/* Column accessors. Only valid after step() returned SQLITE_ROW.
 * SQLite's column functions are forgiving — out-of-range col returns
 * 0/NULL — so we mirror that with safe defaults rather than asserting. */

int sqlite_column_int_raw(void* stmt, int col) {
    if (!stmt) return 0;
    return sqlite3_column_int((sqlite3_stmt*)stmt, col);
}

/* 64-bit column: split into (hi, lo) to match the bind side. Two
 * separate externs because Aether can't return two ints from one
 * call — the alternative would be a TLS pair, which is overkill
 * for two small ints. */
int sqlite_column_i64_hi_raw(void* stmt, int col) {
    if (!stmt) return 0;
    sqlite3_int64 v = sqlite3_column_int64((sqlite3_stmt*)stmt, col);
    return (int)(int32_t)(v >> 32);
}

int sqlite_column_i64_lo_raw(void* stmt, int col) {
    if (!stmt) return 0;
    sqlite3_int64 v = sqlite3_column_int64((sqlite3_stmt*)stmt, col);
    return (int)(int32_t)(v & 0xFFFFFFFFu);
}

/* sqlite3_column_text returns NULL for NULL columns; we map that to
 * "" so callers don't have to NULL-check every cell read. The pointer
 * is valid until the next step()/reset()/finalize(); callers that
 * want to keep the value across those should string_concat-copy it
 * (matches the v1 cell pattern). */
const char* sqlite_column_text_raw(void* stmt, int col) {
    if (!stmt) return "";
    const unsigned char* t = sqlite3_column_text((sqlite3_stmt*)stmt, col);
    return t ? (const char*)t : "";
}

/* -- Blob column: split-accessor TLS pattern (mirrors std.fs and
 *    std.zlib). try_ copies the blob bytes into a TLS slot; get_*
 *    read it borrowed; release_ frees early.
 *
 *    We copy rather than expose sqlite3_column_blob's pointer
 *    directly because that pointer is invalidated by the next
 *    step()/reset()/finalize(), and the Aether wrapper's
 *    string_new_with_length copy may not happen before then in
 *    every code path. The copy ensures callers get a stable buffer
 *    until they explicitly reset or call try_ again. */
static _Thread_local unsigned char* tls_blob_buf = NULL;
static _Thread_local int            tls_blob_len = 0;

static void free_blob_tls(void) {
    if (tls_blob_buf) { free(tls_blob_buf); tls_blob_buf = NULL; }
    tls_blob_len = 0;
}

int sqlite_try_column_blob(void* stmt, int col) {
    free_blob_tls();
    if (!stmt) return 0;
    int n = sqlite3_column_bytes((sqlite3_stmt*)stmt, col);
    if (n < 0) return 0;
    /* SQL NULL blob — sqlite3_column_blob returns NULL; report 0
     * so callers can distinguish "no row" / "NULL cell" from a
     * legitimate empty blob. */
    const void* p = sqlite3_column_blob((sqlite3_stmt*)stmt, col);
    if (!p && n == 0) {
        /* Could be a real empty blob OR a SQL NULL — sqlite's
         * convention says NULL cells return 0 from column_bytes.
         * We treat both as "produce an empty TLS slot, return 1"
         * so callers see length 0 either way. NULL-vs-empty
         * disambiguation needs sqlite3_column_type, which can be
         * added later if a caller actually needs it. */
        tls_blob_buf = NULL;
        tls_blob_len = 0;
        return 1;
    }
    tls_blob_buf = (unsigned char*)malloc((size_t)n > 0 ? (size_t)n : 1);
    if (!tls_blob_buf) return 0;
    if (p && n > 0) memcpy(tls_blob_buf, p, (size_t)n);
    tls_blob_len = n;
    return 1;
}

const char* sqlite_get_blob_bytes(void) {
    return (const char*)(tls_blob_buf ? tls_blob_buf : (unsigned char*)"");
}

int sqlite_get_blob_length(void) { return tls_blob_len; }

void sqlite_release_blob(void) { free_blob_tls(); }

/* reset rebinds the statement to its initial state — bindings are
 * preserved (sqlite3_clear_bindings clears them, which we don't
 * call). Caller can rebind selectively and step again. */
int sqlite_reset_raw(void* stmt) {
    if (!stmt) return SQLITE_MISUSE;
    return sqlite3_reset((sqlite3_stmt*)stmt);
}

int sqlite_finalize_raw(void* stmt) {
    if (!stmt) return SQLITE_OK;
    return sqlite3_finalize((sqlite3_stmt*)stmt);
}

int sqlite_changes_raw(void* db) {
    if (!db) return 0;
    return sqlite3_changes((sqlite3*)db);
}

const char* sqlite_errmsg_raw(void* db) {
    if (!db) return "null db";
    const char* m = sqlite3_errmsg((sqlite3*)db);
    return m ? m : "";
}
