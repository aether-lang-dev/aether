/* contrib/quickjs — QuickJS (quickjs-ng) embedded in Aether.
 *
 * The engine is quickjs-ng's own single-file amalgamation, compiled into this
 * translation unit (amalgamation.lock pins it; scripts/fetch-quickjs-
 * amalgamation.sh fetches it). quickjs-libc, which would give scripts their
 * own file and OS access, sits behind QJS_BUILD_LIBC and is never defined:
 * a script reaches only what the host gives it.
 *
 * Two things make QuickJS awkward to call from Aether directly, and this file
 * exists to absorb both:
 *
 *   - On 64-bit targets a JSValue is a 16-byte struct, returned in two
 *     registers on arm64 and x86-64 SysV and through a hidden pointer on
 *     Windows x64. Aether code never sees one: it holds integer HANDLES into
 *     a per-runtime table, and every function here takes and returns those.
 *   - QuickJS counts references. The table owns one reference per handle;
 *     qjs_release drops it. Nothing else needs JS_DupValue/JS_FreeValue.
 *
 * Host functions are Aether closures. They are all created with
 * JS_NewCFunctionData carrying an index into the runtime's closure table, and
 * one C function (dispatch_) calls the closure at that index, so there is no
 * C to write or generate per host function. A closure is called as
 *     int cb(env, q, this_handle, args_handle)
 * where args is a JS array of the arguments. It returns a handle the call
 * then owns (0 for undefined), or -1 after qjs_throw.
 *
 * Handles are ints >= 1; -1 reports a JS exception, whose text
 * qjs_error_data / qjs_error_len give. A runtime is used from one thread at
 * a time.
 *
 * Text crosses by its length, both ways. Every string Aether hands in (a
 * script, a filename, a property name, a message, JSON) is an `@aether
 * string`: the C side reads its bytes and length (aether_string_data,
 * aether_string_length), so a NUL inside it is a character, not the end. A
 * filename is the one exception the engine forces (JS_Eval takes it as a C
 * string): one holding a NUL is refused, never cut. Every string handed out
 * (to_string, to_json, an error's text) is a byte count beside the bytes,
 * NULs kept, and is valid UTF-8: the engine writes a lone surrogate as its
 * WTF-8 bytes (ED A0..BF xx), and own_utf8_ turns each into U+FFFD (EF BF
 * BD, the same three bytes' room). JSON is the lossless route for one
 * (JSON.stringify writes it as a \uXXXX escape). Bytes going in that are
 * not UTF-8 become U+FFFD, the engine's rule (JS_NewStringLen).
 *
 * Interrupts are the host's: each runtime holds an absolute deadline on the
 * monotonic clock (qjs_now_ns), which the host sets and moves itself and
 * which no entry resets, a budget of interrupt polls (work, not time), and up
 * to two std.sync cells, cancel and stop, that any thread may set. QuickJS polls interrupt_ while JavaScript runs, on the
 * runtime's own thread; it reads only this runtime's fields, the cells with
 * an acquire load. There is no process-wide state.
 */
#include "amalgamation/quickjs-amalgam.c"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* An `@aether string` parameter: the string's bytes and length (std.string). */
extern const char *aether_string_data(const void *s);
extern size_t aether_string_length(const void *s);

/* Layout of the compiler's _AeClosure ({fn, env}); called as fn(env, args...). */
typedef struct { void *fn; void *env; } AeQjsClosure;
extern void aether_closure_env_free(void *env);
/* std.sync's acquire load of a cell (std/sync/aether_sync.c). */
extern int64_t aether_sync_atomic_load(void *cell);

/* qjs_interrupted's answers: why JavaScript was stopped. */
enum { QJS_INTERRUPT_NONE = 0, QJS_INTERRUPT_DEADLINE = 1, QJS_INTERRUPT_CANCEL = 2, QJS_INTERRUPT_STOP = 3,
       QJS_INTERRUPT_BUDGET = 4 };

typedef struct AeQjs {
    JSRuntime *rt;
    JSContext *ctx;
    JSValue *vals;          /* handle h lives at vals[h] (h >= 1) */
    unsigned char *used;
    int cap;
    int *free_list;
    int nfree;
    int live;               /* handles held, for leak checks */
    AeQjsClosure *fns;      /* host-function closures, by index */
    int nfns, capfns;
    int64_t deadline_ns;    /* absolute, on qjs_now_ns's clock; 0 = none */
    void *cancel_cell;      /* std.sync cell, the host's; nonzero stops JS */
    void *stop_cell;        /* likewise, reported apart from a cancel */
    int64_t budget;         /* interrupt polls left; -1 = no budget */
    int interrupted;        /* the first QJS_INTERRUPT_* since cleared */
    char *err;              /* the last exception, as text (NUL-terminated, may hold NULs) */
    size_t err_len;         /* ... and its byte count */
    JSValue exc;            /* ... and as the value itself, for qjs_rethrow */
    int has_exc;
    char *scratch;          /* the last string handed out */
} AeQjs;

/* Monotonic nanoseconds: the clock deadlines are on. The same sources as
 * std.os's now_monotonic_ns (CLOCK_MONOTONIC; QueryPerformanceCounter on
 * Windows), so the two agree, without this module needing std.os. */
int64_t qjs_now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0 || !QueryPerformanceCounter(&c)) return 0;
    /* Split, so ticks * 1e9 cannot overflow on a long uptime. */
    int64_t freq = (int64_t)f.QuadPart, ticks = (int64_t)c.QuadPart;
    return (ticks / freq) * 1000000000 + ((ticks % freq) * 1000000000) / freq;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
#endif
}

/* QuickJS's interrupt poll, on the thread running the JavaScript. quickjs-ng
 * (0.18.0) calls it when its per-context counter, JS_INTERRUPT_COUNTER_INIT
 * (10000), runs out: the counter is decremented at every JavaScript function
 * call, every jump the interpreter takes or tests (a loop's back edge, an
 * `if`), every element step of the built-ins that iterate (Array.prototype's
 * find, map, sort, ...) and every step of a proxy chain; the regexp engine
 * polls on its own counter of 10000 backtracking steps. Each call here spends
 * one of the poll budget's polls. A cancel is reported over a passed deadline,
 * a deadline over a spent budget, and all three over a stop; the first reason
 * sticks until qjs_clear_interrupted. */
static int interrupt_(JSContext *ctx, void *opaque) {
    AeQjs *q = (AeQjs *)opaque;
    int why = QJS_INTERRUPT_NONE;
    int spent = 0;
    (void)ctx;
    if (q->budget >= 0) {
        if (q->budget > 0) q->budget--;
        spent = q->budget == 0;
    }
    if (q->cancel_cell && aether_sync_atomic_load(q->cancel_cell) != 0) why = QJS_INTERRUPT_CANCEL;
    else if (q->deadline_ns != 0 && qjs_now_ns() >= q->deadline_ns) why = QJS_INTERRUPT_DEADLINE;
    else if (spent) why = QJS_INTERRUPT_BUDGET;
    else if (q->stop_cell && aether_sync_atomic_load(q->stop_cell) != 0) why = QJS_INTERRUPT_STOP;
    if (why == QJS_INTERRUPT_NONE) return 0;
    if (q->interrupted == QJS_INTERRUPT_NONE) q->interrupted = why;
    return 1;
}

/* Every entry into JS: the stack top where we are now (the host may call in
 * from different depths). The interrupt needs nothing per entry: the
 * deadline and the cells are the host's, and hold across entries. */
static void enter_(AeQjs *q) {
    JS_UpdateStackTop(q->rt);
}

static void drop_exc_(AeQjs *q) {
    if (q->has_exc) { JS_FreeValue(q->ctx, q->exc); q->has_exc = 0; }
}

/* An error with no exception value behind it (a host-side refusal). */
static void set_err_(AeQjs *q, const char *s) {
    drop_exc_(q);
    free(q->err);
    q->err = strdup(s ? s : "");
    q->err_len = q->err ? strlen(q->err) : 0;
}

/* The engine writes a lone surrogate as its three WTF-8 bytes, ED A0..BF
 * 80..BF, which no valid UTF-8 holds; each becomes U+FFFD, EF BF BD, in
 * place. A surrogate pair the engine has already joined into one four-byte
 * character. */
static void lone_surrogates_to_fffd_(char *b, size_t n) {
    unsigned char *u = (unsigned char *)b;
    for (size_t i = 0; i + 2 < n; i++) {
        if (u[i] == 0xED && u[i + 1] >= 0xA0 && u[i + 1] <= 0xBF && (u[i + 2] & 0xC0) == 0x80) {
            u[i] = 0xEF; u[i + 1] = 0xBF; u[i + 2] = 0xBD;
            i += 2;
        }
    }
}

/* String(v) as UTF-8 in a buffer of our own (malloc'd, NUL-terminated, the
 * byte count in *n, NULs kept, lone surrogates as U+FFFD); NULL with the
 * exception pending when the conversion threw, or with none pending when
 * there was no memory for the copy. */
static char *own_utf8_(AeQjs *q, JSValueConst v, size_t *n) {
    size_t len = 0;
    const char *c = JS_ToCStringLen(q->ctx, &len, v);
    *n = 0;
    if (!c) return NULL;
    char *b = (char *)malloc(len + 1);
    if (b) {
        memcpy(b, c, len);
        b[len] = '\0';
        lone_surrogates_to_fffd_(b, len);
        *n = len;
    }
    JS_FreeCString(q->ctx, c);
    return b;
}

/* Hand out a buffer from own_utf8_ as the runtime's scratch (freeing the last). */
static void hand_out_(AeQjs *q, char *b) {
    free(q->scratch);
    q->scratch = b;
}

/* An `@aether string`'s bytes and length. */
static const char *bytes_(const void *s, size_t *n) {
    *n = s ? aether_string_length(s) : 0;
    const char *d = s ? aether_string_data(s) : NULL;
    if (!d) { *n = 0; return ""; }
    return d;
}

/* A property key from a name's bytes, NULs and all; JS_ATOM_NULL (with an
 * exception pending) when there is no memory for it. */
static JSAtom atom_(AeQjs *q, const void *name) {
    size_t n;
    const char *d = bytes_(name, &n);
    return JS_NewAtomLen(q->ctx, d, n);
}

/* ---- the handle table ---------------------------------------------------- */

/* Take ownership of v; its handle. */
static int put_(AeQjs *q, JSValue v) {
    if (q->nfree == 0) {
        /* Grow: every new slot goes on the free list (slot 0 never does,
         * so 0 is never a handle). */
        int ncap = q->cap ? q->cap * 2 : 64;
        JSValue *nv = (JSValue *)realloc(q->vals, sizeof(JSValue) * ncap);
        if (nv) q->vals = nv;
        unsigned char *nu = nv ? (unsigned char *)realloc(q->used, ncap) : NULL;
        if (nu) q->used = nu;
        int *nf = nu ? (int *)realloc(q->free_list, sizeof(int) * ncap) : NULL;
        if (nf) q->free_list = nf;
        if (!nv || !nu || !nf) { JS_FreeValue(q->ctx, v); set_err_(q, "InternalError: out of memory"); return -1; }
        memset(q->used + q->cap, 0, ncap - q->cap);
        for (int i = ncap - 1; i >= (q->cap ? q->cap : 1); i--) q->free_list[q->nfree++] = i;
        q->cap = ncap;
    }
    int h = q->free_list[--q->nfree];
    q->vals[h] = v;
    q->used[h] = 1;
    q->live++;
    return h;
}

static int valid_(AeQjs *q, int h) { return q && h > 0 && h < q->cap && q->used[h]; }

static JSValueConst get_(AeQjs *q, int h) { return valid_(q, h) ? q->vals[h] : JS_UNDEFINED; }

/* Remove h from the table without freeing: the caller owns the value now. */
static JSValue take_(AeQjs *q, int h) {
    if (!valid_(q, h)) return JS_UNDEFINED;
    JSValue v = q->vals[h];
    q->used[h] = 0;
    q->free_list[q->nfree++] = h;
    q->live--;
    return v;
}

/* The pending exception as text ("TypeError: x\n    at f (page.js:3)"),
 * recorded for qjs_error; -1. */
static int fail_(AeQjs *q) {
    JSValue e = JS_GetException(q->ctx);
    /* With the heap at its cap QuickJS cannot allocate the "out of memory"
     * error itself (JS_ThrowOutOfMemory), so the exception is left null. */
    if (JS_IsNull(e) || JS_IsUninitialized(e)) {
        set_err_(q, "InternalError: out of memory (the runtime's memory limit)");
        return -1;
    }
    /* String(e), then a newline and e.stack when e is an Error with one: by
     * their lengths, so a NUL in the message is kept. A thrown object whose
     * toString throws in turn is "exception" (that second exception is
     * dropped: the value recorded is the first). */
    size_t tn = 0;
    char *text = own_utf8_(q, e, &tn);
    if (!text) {
        if (JS_HasException(q->ctx)) JS_FreeValue(q->ctx, JS_GetException(q->ctx));
        text = strdup("exception");
        tn = text ? strlen(text) : 0;
    }
    if (text && JS_IsError(e)) {
        JSValue st = JS_GetPropertyStr(q->ctx, e, "stack");
        if (JS_IsString(st)) {
            size_t sn = 0;
            char *s = own_utf8_(q, st, &sn);
            if (s && sn > 0) {
                char *both = (char *)malloc(tn + 1 + sn + 1);
                if (both) {
                    memcpy(both, text, tn);
                    both[tn] = '\n';
                    memcpy(both + tn + 1, s, sn);
                    both[tn + 1 + sn] = '\0';
                    free(text);
                    text = both;
                    tn = tn + 1 + sn;
                }
            }
            free(s);
        } else if (JS_IsException(st)) {
            JS_FreeValue(q->ctx, JS_GetException(q->ctx));
        }
        JS_FreeValue(q->ctx, st);
    }
    /* Kept, so a host function can throw this very value on (qjs_rethrow). */
    drop_exc_(q);
    q->exc = e;
    q->has_exc = 1;
    free(q->err);
    q->err = text;
    q->err_len = text ? tn : 0;
    return -1;
}

static int result_(AeQjs *q, JSValue v) {
    if (JS_IsException(v)) return fail_(q);
    return put_(q, v);
}

/* ---- runtimes ------------------------------------------------------------ */

/* A runtime and its one context. memory_kb and stack_kb of 0 mean QuickJS's
 * defaults (no memory limit; its stack limit). NULL if it cannot be made. */
void *qjs_new(int memory_kb, int stack_kb) {
    AeQjs *q = (AeQjs *)calloc(1, sizeof(AeQjs));
    if (!q) return NULL;
    q->rt = JS_NewRuntime();
    if (!q->rt) { free(q); return NULL; }
    if (memory_kb > 0) JS_SetMemoryLimit(q->rt, (size_t)memory_kb * 1024);
    if (stack_kb > 0) JS_SetMaxStackSize(q->rt, (size_t)stack_kb * 1024);
    JS_SetInterruptHandler(q->rt, interrupt_, q);
    q->ctx = JS_NewContext(q->rt);
    if (!q->ctx) { JS_FreeRuntime(q->rt); free(q); return NULL; }
    JS_SetContextOpaque(q->ctx, q);
    q->budget = -1;
    q->err = strdup("");
    return q;
}

/* Frees every handle still held, the context, the runtime and the host
 * closures' captures. */
void qjs_free(void *qp) {
    AeQjs *q = (AeQjs *)qp;
    if (!q) return;
    for (int h = 1; h < q->cap; h++) {
        if (q->used[h]) { JS_FreeValue(q->ctx, q->vals[h]); q->used[h] = 0; }
    }
    drop_exc_(q);
    JS_FreeContext(q->ctx);
    JS_FreeRuntime(q->rt);
    for (int i = 0; i < q->nfns; i++) {
        if (q->fns[i].env) aether_closure_env_free(q->fns[i].env);
    }
    free(q->fns);
    free(q->vals); free(q->used); free(q->free_list);
    free(q->err); free(q->scratch);
    free(q);
}

/* JavaScript running at or after ns (qjs_now_ns's clock) is stopped with
 * "InternalError: interrupted", in this entry and every later one until the
 * host moves or clears it; 0 for no deadline. Set by the runtime's thread. */
void qjs_set_deadline_ns(void *qp, int64_t ns) { ((AeQjs *)qp)->deadline_ns = ns; }
int64_t qjs_deadline_ns(void *qp) { return ((AeQjs *)qp)->deadline_ns; }

/* A std.sync cell whose nonzero value stops JavaScript (NULL for none). The
 * cell is the host's: it must outlive the runtime or be unset first. */
void qjs_set_cancel_cell(void *qp, void *cell) { ((AeQjs *)qp)->cancel_cell = cell; }
void qjs_set_stop_cell(void *qp, void *cell) { ((AeQjs *)qp)->stop_cell = cell; }

/* A budget of interrupt polls: each poll spends one, and the poll that leaves
 * none stops JavaScript (QJS_INTERRUPT_BUDGET), in this entry and every later
 * one, until the host sets it again or clears it (-1). A budget of n stops
 * JavaScript at its nth poll from now: after between (n - 1) * 10000 and
 * n * 10000 of the counted steps interrupt_ describes (a fresh runtime polls
 * at its first step, so exactly (n - 1) * 10000 then). 0 is spent: the next
 * poll stops. Below 0, no budget. Set by the runtime's thread. */
void qjs_set_poll_budget(void *qp, int64_t polls) { ((AeQjs *)qp)->budget = polls < 0 ? -1 : polls; }
int64_t qjs_poll_budget(void *qp) { return ((AeQjs *)qp)->budget; }

/* Why JavaScript was first interrupted since the last clear: a QJS_INTERRUPT_*. */
int qjs_interrupted(void *qp) { return ((AeQjs *)qp)->interrupted; }
void qjs_clear_interrupted(void *qp) { ((AeQjs *)qp)->interrupted = QJS_INTERRUPT_NONE; }

/* The text of the last exception (empty if none): its bytes, which may hold
 * NULs, and their count. Valid until the next failure on this runtime. */
const void *qjs_error_data(void *qp) { AeQjs *q = (AeQjs *)qp; return q && q->err ? q->err : ""; }
int qjs_error_len(void *qp) {
    AeQjs *q = (AeQjs *)qp;
    if (!q || !q->err) return 0;
    return q->err_len > (size_t)0x7fffffff ? 0x7fffffff : (int)q->err_len;
}

/* The value the last failure threw (an eval, call, run_jobs or conversion),
 * as a NEW handle the caller releases; it stays valid after later failures,
 * which only change what the next call returns. 0 when the last failure had
 * no value behind it: a host-side refusal (set_err_), or a heap too full to
 * make the error. Runs no JavaScript. */
int qjs_last_exception(void *qp) {
    AeQjs *q = (AeQjs *)qp;
    if (!q || !q->has_exc) return 0;
    return put_(q, JS_DupValue(q->ctx, q->exc));
}

/* Handles currently held: 0 once a host has released everything it took. */
int qjs_handles(void *qp) { return ((AeQjs *)qp)->live; }

/* Bytes the runtime has allocated. */
int64_t qjs_memory_used(void *qp) {
    JSMemoryUsage u;
    JS_ComputeMemoryUsage(((AeQjs *)qp)->rt, &u);
    return u.malloc_size;
}

/* ---- running code -------------------------------------------------------- */

/* flags: 1 = a module (its result is a promise), 2 = strict, 4 = a global
 * script with top-level await (JS_EVAL_FLAG_ASYNC): its result is a promise
 * that settles to { value: <the script's completion value> }. */
int qjs_eval(void *qp, const void *code_s, const void *filename_s, int flags) {
    AeQjs *q = (AeQjs *)qp;
    size_t n, fn;
    const char *code = bytes_(code_s, &n);
    const char *filename = bytes_(filename_s, &fn);
    /* JS_Eval takes the filename as a C string: one holding a NUL would be
     * a different, shorter name in every stack, so it is refused. */
    if (memchr(filename, 0, fn)) { set_err_(q, "TypeError: eval: the filename holds a NUL"); return -1; }
    int f = (flags & 1) ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
    if (flags & 2) f |= JS_EVAL_FLAG_STRICT;
    if ((flags & 4) && !(flags & 1)) f |= JS_EVAL_FLAG_ASYNC;
    /* The script is all n bytes, NULs too (the tokenizer reads a NUL before
     * the end as a character). JS_Eval wants code[n] to be a NUL: a string
     * always has one there, but a view into a longer one may not, so that
     * case is copied. */
    char *copy = NULL;
    if (code[n] != '\0') {
        copy = (char *)malloc(n + 1);
        if (!copy) { set_err_(q, "InternalError: out of memory"); return -1; }
        memcpy(copy, code, n);
        copy[n] = '\0';
        code = copy;
    }
    /* filename has a NUL at filename[fn], by the same rule, or came unbroken
     * from a C string: either way it ends where the check above looked. */
    char *fcopy = NULL;
    if (filename[fn] != '\0') {
        fcopy = (char *)malloc(fn + 1);
        if (!fcopy) { free(copy); set_err_(q, "InternalError: out of memory"); return -1; }
        memcpy(fcopy, filename, fn);
        fcopy[fn] = '\0';
        filename = fcopy;
    }
    enter_(q);
    int r = result_(q, JS_Eval(q->ctx, code, n, filename, f));
    free(copy);
    free(fcopy);
    return r;
}

/* Call fn with this and the elements of the array args (0: no arguments). */
int qjs_call(void *qp, int fn, int this_h, int args) {
    AeQjs *q = (AeQjs *)qp;
    JSValueConst f = get_(q, fn);
    if (!JS_IsFunction(q->ctx, f)) { set_err_(q, "TypeError: not a function"); return -1; }
    int64_t n = 0;
    JSValue *argv = NULL;
    enter_(q);   /* reading args can run getters */
    if (args > 0) {
        JSValueConst a = get_(q, args);
        if (JS_GetLength(q->ctx, a, &n) < 0) return fail_(q);
        if (n > 65535) { set_err_(q, "RangeError: too many arguments"); return -1; }
        argv = (JSValue *)malloc(sizeof(JSValue) * (n ? n : 1));
        if (!argv) { set_err_(q, "InternalError: out of memory"); return -1; }
        for (int64_t i = 0; i < n; i++) argv[i] = JS_GetPropertyUint32(q->ctx, a, (uint32_t)i);
    }
    JSValue r = JS_Call(q->ctx, f, get_(q, this_h), (int)n, (JSValueConst *)argv);
    for (int64_t i = 0; i < n; i++) JS_FreeValue(q->ctx, argv[i]);
    free(argv);
    return result_(q, r);
}

/* Run the pending promise jobs (what `await` and .then() queued) until none
 * are left: the number run, or -1 if one threw (qjs_error says what). */
int qjs_run_jobs(void *qp) {
    AeQjs *q = (AeQjs *)qp;
    int ran = 0;
    enter_(q);
    for (;;) {
        JSContext *c;
        int r = JS_ExecutePendingJob(q->rt, &c);
        if (r == 0) return ran;
        if (r < 0) {
            /* The exception is on the job's context, which is ours. */
            return fail_(q);
        }
        ran++;
    }
}

/* ---- host functions ------------------------------------------------------ */

static JSValue dispatch_(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv, int magic, JSValueConst *data) {
    AeQjs *q = (AeQjs *)JS_GetContextOpaque(ctx);
    int32_t idx = -1;
    (void)magic;
    JS_ToInt32(ctx, &idx, data[0]);
    if (!q || idx < 0 || idx >= q->nfns)
        return JS_ThrowInternalError(ctx, "contrib.quickjs: no such host function");
    AeQjsClosure c = q->fns[idx];
    int this_h = put_(q, JS_DupValue(ctx, this_val));
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < argc; i++) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_DupValue(ctx, argv[i]));
    int args_h = put_(q, arr);
    int r = ((int (*)(void *, void *, int, int))c.fn)(c.env, q, this_h, args_h);
    JSValue ret;
    if (r == -1) {
        if (!JS_HasException(ctx)) JS_ThrowInternalError(ctx, "host function failed without throwing");
        ret = JS_EXCEPTION;
    } else if (r == 0) {
        ret = JS_UNDEFINED;
    } else if (r == this_h || r == args_h) {
        ret = JS_DupValue(ctx, get_(q, r));
    } else if (valid_(q, r)) {
        ret = take_(q, r);
    } else {
        ret = JS_ThrowInternalError(ctx, "host function returned an unknown handle");
    }
    JS_FreeValue(ctx, take_(q, this_h));
    JS_FreeValue(ctx, take_(q, args_h));
    return ret;
}

/* A JS function named `name` that calls the Aether closure cb (the runtime
 * keeps the closure until qjs_free). */
int qjs_function(void *qp, const void *name_s, int arity, AeQjsClosure cb) {
    AeQjs *q = (AeQjs *)qp;
    if (q->nfns == q->capfns) {
        int ncap = q->capfns ? q->capfns * 2 : 16;
        AeQjsClosure *nf = (AeQjsClosure *)realloc(q->fns, sizeof(AeQjsClosure) * ncap);
        if (!nf) { set_err_(q, "InternalError: out of memory"); return -1; }
        q->fns = nf; q->capfns = ncap;
    }
    int idx = q->nfns++;
    q->fns[idx] = cb;
    JSValue data = JS_NewInt32(q->ctx, idx);
    JSValue f = JS_NewCFunctionData(q->ctx, dispatch_, arity, 0, 1, &data);
    JS_FreeValue(q->ctx, data);
    if (JS_IsException(f)) return fail_(q);
    size_t nn;
    const char *name = bytes_(name_s, &nn);
    if (nn > 0) {
        JS_DefinePropertyValueStr(q->ctx, f, "name", JS_NewStringLen(q->ctx, name, nn), JS_PROP_CONFIGURABLE);
    }
    return put_(q, f);
}

static int is_kind_(const char *kind, size_t kn, const char *want) {
    return strlen(want) == kn && memcmp(kind, want, kn) == 0;
}

/* Throw an error of `kind` whose message is the n bytes at msg, NULs kept:
 * the error is made with an empty message (so its stack is captured here, as
 * JS_Throw*Error would) and the message set by its length. -1. */
static int throw_kind_(AeQjs *q, const char *kind, size_t kn, const char *msg, size_t n) {
    JSValue e;
    if (is_kind_(kind, kn, "TypeError")) e = JS_NewTypeError(q->ctx, "%s", "");
    else if (is_kind_(kind, kn, "RangeError")) e = JS_NewRangeError(q->ctx, "%s", "");
    else if (is_kind_(kind, kn, "SyntaxError")) e = JS_NewSyntaxError(q->ctx, "%s", "");
    else if (is_kind_(kind, kn, "ReferenceError")) e = JS_NewReferenceError(q->ctx, "%s", "");
    else e = JS_NewError(q->ctx);
    if (JS_IsException(e)) return -1;   /* no memory: that exception is pending */
    JS_DefinePropertyValueStr(q->ctx, e, "message", JS_NewStringLen(q->ctx, msg, n),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_Throw(q->ctx, e);
    return -1;
}

/* Throw from inside a host function; returns -1, which the function returns.
 * kind: "TypeError", "RangeError", "SyntaxError", "ReferenceError", else Error.
 * The message goes in by its length. */
int qjs_throw(void *qp, const void *kind_s, const void *msg_s) {
    AeQjs *q = (AeQjs *)qp;
    size_t kn, n;
    const char *kind = bytes_(kind_s, &kn);
    const char *msg = bytes_(msg_s, &n);
    return throw_kind_(q, kind, kn, msg, n);
}

/* Throw the last exception again, the same value (its type, message and
 * stack), from inside a host function whose nested qjs_call or qjs_eval
 * failed; returns -1, which the host function returns. With no value kept,
 * the error was the host's ("TypeError: not a function") or the heap was
 * full ("InternalError: out of memory ..."): the error its text names, with
 * the rest as the message, or an Error with the whole text. Since
 * quickjs-ng 0.18 an InternalError cannot be caught by the script, which
 * is what a full heap should be; a host refusal stays catchable. */
int qjs_rethrow(void *qp) {
    AeQjs *q = (AeQjs *)qp;
    if (q->has_exc) {
        JS_Throw(q->ctx, JS_DupValue(q->ctx, q->exc));
        return -1;
    }
    /* A host refusal's text: set_err_'s, a C string. */
    const char *text = q->err && *q->err ? q->err : "Error: error";
    size_t tn = strlen(text);
    static const char *const kinds[] = { "TypeError", "RangeError", "SyntaxError", "ReferenceError" };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        size_t n = strlen(kinds[i]);
        if (strncmp(text, kinds[i], n) == 0 && text[n] == ':' && text[n + 1] == ' ')
            return throw_kind_(q, kinds[i], n, text + n + 2, tn - n - 2);
    }
    if (strncmp(text, "InternalError: ", 15) == 0) {
        JS_ThrowInternalError(q->ctx, "%s", text + 15);
        return -1;
    }
    return throw_kind_(q, "Error", 5, text, tn);
}

/* ---- values -------------------------------------------------------------- */

void qjs_release(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    if (valid_(q, h)) JS_FreeValue(q->ctx, take_(q, h));
}

/* A second handle to the same value (released separately). */
int qjs_dup(void *qp, int h) { AeQjs *q = (AeQjs *)qp; return put_(q, JS_DupValue(q->ctx, get_(q, h))); }

int qjs_global(void *qp) { AeQjs *q = (AeQjs *)qp; return put_(q, JS_GetGlobalObject(q->ctx)); }
int qjs_undefined(void *qp) { return put_((AeQjs *)qp, JS_UNDEFINED); }
int qjs_null(void *qp) { return put_((AeQjs *)qp, JS_NULL); }
int qjs_bool(void *qp, int b) { return put_((AeQjs *)qp, JS_NewBool(((AeQjs *)qp)->ctx, b != 0)); }
int qjs_int(void *qp, int n) { return put_((AeQjs *)qp, JS_NewInt32(((AeQjs *)qp)->ctx, n)); }
int qjs_float(void *qp, double d) { return put_((AeQjs *)qp, JS_NewFloat64(((AeQjs *)qp)->ctx, d)); }
/* A JavaScript string of all of s's bytes, NULs kept (UTF-8; a byte that is
 * not is U+FFFD). */
int qjs_string(void *qp, const void *s) {
    AeQjs *q = (AeQjs *)qp;
    size_t n;
    const char *d = bytes_(s, &n);
    return result_(q, JS_NewStringLen(q->ctx, d, n));
}
int qjs_object(void *qp) { AeQjs *q = (AeQjs *)qp; return result_(q, JS_NewObject(q->ctx)); }
int qjs_array(void *qp) { AeQjs *q = (AeQjs *)qp; return result_(q, JS_NewArray(q->ctx)); }

/* JSON.parse of all of text's bytes (a NUL in it is a parse error, as any
 * stray character is); -1 with the error text on bad JSON. */
int qjs_parse_json(void *qp, const void *text_s) {
    AeQjs *q = (AeQjs *)qp;
    size_t n;
    const char *text = bytes_(text_s, &n);
    /* JS_ParseJSON, like JS_Eval, wants text[n] to be a NUL. */
    char *copy = NULL;
    if (text[n] != '\0') {
        copy = (char *)malloc(n + 1);
        if (!copy) { set_err_(q, "InternalError: out of memory"); return -1; }
        memcpy(copy, text, n);
        copy[n] = '\0';
        text = copy;
    }
    enter_(q);
    int r = result_(q, JS_ParseJSON(q->ctx, text, n, "<json>"));
    free(copy);
    return r;
}

/* JSON.stringify(h) in the runtime's scratch (qjs_scratch): its byte count,
 * or -1 when h has no JSON form (undefined, a function) or stringifying
 * threw (then with the error text). */
int qjs_to_json(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    enter_(q);
    JSValue s = JS_JSONStringify(q->ctx, get_(q, h), JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(s)) return fail_(q);
    if (JS_IsUndefined(s)) return -1;
    size_t n;
    char *b = own_utf8_(q, s, &n);
    JS_FreeValue(q->ctx, s);
    if (!b) {
        if (JS_HasException(q->ctx)) return fail_(q);
        set_err_(q, "InternalError: out of memory");
        return -1;
    }
    if (n > (size_t)0x7fffffff) { free(b); set_err_(q, "RangeError: string too long"); return -1; }
    hand_out_(q, b);
    return (int)n;
}

/* String(h), as valid UTF-8 (lone surrogates as U+FFFD) in the runtime's scratch (qjs_scratch, valid until the
 * next string this runtime hands out): its byte count, which may be 0 and may
 * count NUL bytes, or -1 with qjs_error when the conversion threw (a Symbol,
 * a throwing or interrupted toString) or there was no memory for it. */
int qjs_to_string(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    enter_(q);
    size_t n;
    char *b = own_utf8_(q, get_(q, h), &n);
    if (!b) {
        if (JS_HasException(q->ctx)) return fail_(q);
        set_err_(q, "InternalError: out of memory");
        return -1;
    }
    if (n > (size_t)0x7fffffff) { free(b); set_err_(q, "RangeError: string too long"); return -1; }
    hand_out_(q, b);
    return (int)n;
}

/* The bytes qjs_to_string last handed out. */
const void *qjs_scratch(void *qp) { return ((AeQjs *)qp)->scratch; }

int qjs_to_int(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    int32_t n = 0;
    enter_(q);
    if (JS_ToInt32(q->ctx, &n, get_(q, h)) < 0) fail_(q);
    return n;
}

double qjs_to_float(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    double d = 0;
    enter_(q);
    if (JS_ToFloat64(q->ctx, &d, get_(q, h)) < 0) fail_(q);
    return d;
}

int qjs_to_bool(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    enter_(q);
    int b = JS_ToBool(q->ctx, get_(q, h));
    if (b < 0) { fail_(q); return 0; }
    return b;
}

/* "undefined", "null", "boolean", "number", "bigint", "string", "symbol",
 * "function", "array", "error", "promise" or "object". */
const char *qjs_type(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    JSValueConst v = get_(q, h);
    if (JS_IsUndefined(v)) return "undefined";
    if (JS_IsNull(v)) return "null";
    if (JS_IsBool(v)) return "boolean";
    if (JS_IsNumber(v)) return "number";
    if (JS_IsBigInt(v)) return "bigint";
    if (JS_IsString(v)) return "string";
    if (JS_IsSymbol(v)) return "symbol";
    if (JS_IsFunction(q->ctx, v)) return "function";
    if (JS_IsArray(v)) return "array";
    if (JS_IsError(v)) return "error";
    if (JS_IsPromise(v)) return "promise";
    return "object";
}

/* ---- properties ---------------------------------------------------------- */

/* obj[name], the name by its length (a NUL in it is part of the key). */
int qjs_get(void *qp, int obj, const void *name) {
    AeQjs *q = (AeQjs *)qp;
    enter_(q);
    JSAtom a = atom_(q, name);
    if (a == JS_ATOM_NULL) return fail_(q);
    JSValue v = JS_GetProperty(q->ctx, get_(q, obj), a);
    JS_FreeAtom(q->ctx, a);
    return result_(q, v);
}

/* obj[name] = val (val stays held by its handle too), the name by its
 * length; 1, or -1. */
int qjs_set(void *qp, int obj, const void *name, int val) {
    AeQjs *q = (AeQjs *)qp;
    enter_(q);
    JSAtom a = atom_(q, name);
    if (a == JS_ATOM_NULL) return fail_(q);
    int r = JS_SetProperty(q->ctx, get_(q, obj), a, JS_DupValue(q->ctx, get_(q, val)));
    JS_FreeAtom(q->ctx, a);
    if (r < 0) return fail_(q);
    return 1;
}

int qjs_get_index(void *qp, int obj, int i) {
    AeQjs *q = (AeQjs *)qp;
    enter_(q);
    return result_(q, JS_GetPropertyUint32(q->ctx, get_(q, obj), (uint32_t)i));
}

int qjs_set_index(void *qp, int obj, int i, int val) {
    AeQjs *q = (AeQjs *)qp;
    enter_(q);
    if (JS_SetPropertyUint32(q->ctx, get_(q, obj), (uint32_t)i, JS_DupValue(q->ctx, get_(q, val))) < 0) return fail_(q);
    return 1;
}

/* obj.length, or -1. */
int qjs_length(void *qp, int obj) {
    AeQjs *q = (AeQjs *)qp;
    int64_t n = 0;
    enter_(q);
    if (JS_GetLength(q->ctx, get_(q, obj), &n) < 0) return fail_(q);
    return (int)n;
}

/* An array of obj's own enumerable string keys. */
int qjs_keys(void *qp, int obj) {
    AeQjs *q = (AeQjs *)qp;
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    enter_(q);
    if (JS_GetOwnPropertyNames(q->ctx, &tab, &len, get_(q, obj), JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return fail_(q);
    JSValue arr = JS_NewArray(q->ctx);
    for (uint32_t i = 0; i < len; i++)
        JS_SetPropertyUint32(q->ctx, arr, i, JS_AtomToString(q->ctx, tab[i].atom));
    JS_FreePropertyEnum(q->ctx, tab, len);
    return put_(q, arr);
}

/* ---- promises ------------------------------------------------------------ */

/* A new promise, as an object { promise, resolve, reject }: hand `promise`
 * to the script, and call `resolve` or `reject` (qjs_call) when the answer
 * comes. Run the job queue after, so the script's awaits continue. */
int qjs_new_promise(void *qp) {
    AeQjs *q = (AeQjs *)qp;
    JSValue funcs[2];
    JSValue p = JS_NewPromiseCapability(q->ctx, funcs);
    if (JS_IsException(p)) return fail_(q);
    JSValue o = JS_NewObject(q->ctx);
    JS_SetPropertyStr(q->ctx, o, "promise", p);
    JS_SetPropertyStr(q->ctx, o, "resolve", funcs[0]);
    JS_SetPropertyStr(q->ctx, o, "reject", funcs[1]);
    return put_(q, o);
}

/* -1 when h is not a promise, else JS_PROMISE_PENDING (0), FULFILLED (1) or
 * REJECTED (2). Runs no JavaScript. */
int qjs_promise_state(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    if (!valid_(q, h)) return -1;
    return (int)JS_PromiseState(q->ctx, q->vals[h]);
}

/* The value a settled promise holds (its fulfilment value, or the reason it
 * was rejected with), as a new handle; -1 with qjs_error when h is not a
 * promise or is still pending. Runs no JavaScript. */
int qjs_promise_result(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    int st = valid_(q, h) ? (int)JS_PromiseState(q->ctx, q->vals[h]) : -1;
    if (st < 0) { set_err_(q, "TypeError: not a promise"); return -1; }
    if (st == JS_PROMISE_PENDING) { set_err_(q, "TypeError: the promise is still pending"); return -1; }
    return put_(q, JS_PromiseResult(q->ctx, q->vals[h]));
}

/* ---- bytes ---------------------------------------------------------------- */

/* The bytes of the Uint8Array / Uint8ClampedArray v (a view's own window of
 * its buffer), their count in *n; NULL when v is not one, or its buffer is
 * detached or the view out of bounds. Other typed arrays are refused: their
 * byte order and element width are not what a byte reader expects. Leaves
 * no exception pending. An empty array is a valid answer with no bytes,
 * returned as a non-NULL pointer so it is told apart from "not one". */
static const uint8_t *u8_(AeQjs *q, JSValueConst v, size_t *n) {
    *n = 0;
    int t = JS_GetTypedArrayType(v);
    if (t != JS_TYPED_ARRAY_UINT8 && t != JS_TYPED_ARRAY_UINT8C) return NULL;
    size_t len = 0;
    uint8_t *b = JS_GetUint8Array(q->ctx, &len, v);
    if (!b) {
        if (JS_HasException(q->ctx)) {
            JS_FreeValue(q->ctx, JS_GetException(q->ctx));
            return NULL;
        }
        static const uint8_t empty_ = 0;
        return &empty_;   /* no buffer storage behind an empty view */
    }
    if (len > (size_t)0x7fffffff) return NULL;
    *n = len;
    return b;
}

/* The byte count of the Uint8Array at h, or -1 when it is not one. */
int qjs_u8_length(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    if (!valid_(q, h)) return -1;
    size_t n;
    return u8_(q, q->vals[h], &n) ? (int)n : -1;
}

/* A pointer to those bytes, inside the engine's buffer: valid until JS next
 * runs (which can resize or detach it) or h's value is collected; NULL when
 * h is not one. */
const void *qjs_u8_data(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    if (!valid_(q, h)) return NULL;
    size_t n;
    return u8_(q, q->vals[h], &n);
}

/* A new Uint8Array holding a copy of the n bytes at data; -1 (with
 * qjs_error) for a negative n, a NULL data with n > 0, or no memory. */
int qjs_new_uint8array(void *qp, const void *data, int n) {
    AeQjs *q = (AeQjs *)qp;
    if (n < 0 || (n > 0 && !data)) { set_err_(q, "RangeError: invalid byte count or data"); return -1; }
    static const uint8_t none_ = 0;
    return result_(q, JS_NewUint8ArrayCopy(q->ctx, n > 0 ? (const uint8_t *)data : &none_, (size_t)n));
}
