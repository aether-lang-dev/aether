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
 * Handles are ints >= 1; -1 reports a JS exception, whose text qjs_error
 * gives. A runtime is used from one thread at a time.
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

/* Layout of the compiler's _AeClosure ({fn, env}); called as fn(env, args...). */
typedef struct { void *fn; void *env; } AeQjsClosure;
extern void aether_closure_env_free(void *env);

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
    int64_t limit_ms;       /* per entry; 0 = none */
    int64_t deadline_ms;    /* now + limit_ms at the current entry */
    char *err;              /* the last exception, as text */
    char *scratch;          /* the last string handed out */
} AeQjs;

static int64_t now_ms_(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static int interrupt_(JSRuntime *rt, void *opaque) {
    AeQjs *q = (AeQjs *)opaque;
    (void)rt;
    return q->deadline_ms != 0 && now_ms_() > q->deadline_ms;
}

/* Every entry into JS: a fresh deadline, and the stack top where we are now
 * (the host may call in from different depths). */
static void enter_(AeQjs *q) {
    q->deadline_ms = q->limit_ms > 0 ? now_ms_() + q->limit_ms : 0;
    JS_UpdateStackTop(q->rt);
}

static void set_err_(AeQjs *q, const char *s) {
    free(q->err);
    q->err = strdup(s ? s : "");
}

static const char *hand_out_(AeQjs *q, const char *s, size_t n) {
    free(q->scratch);
    q->scratch = (char *)malloc(n + 1);
    if (!q->scratch) return "";
    memcpy(q->scratch, s, n);
    q->scratch[n] = '\0';
    return q->scratch;
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
    const char *msg = JS_ToCString(q->ctx, e);
    char *text = strdup(msg ? msg : "exception");
    if (msg) JS_FreeCString(q->ctx, msg);
    if (JS_IsError(e)) {
        JSValue st = JS_GetPropertyStr(q->ctx, e, "stack");
        if (JS_IsString(st)) {
            const char *s = JS_ToCString(q->ctx, st);
            if (s && *s) {
                size_t n = strlen(text) + strlen(s) + 2;
                char *both = (char *)malloc(n);
                if (both) { snprintf(both, n, "%s\n%s", text, s); free(text); text = both; }
            }
            if (s) JS_FreeCString(q->ctx, s);
        }
        JS_FreeValue(q->ctx, st);
    }
    JS_FreeValue(q->ctx, e);
    free(q->err);
    q->err = text;
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

/* Milliseconds each entry into JS (an eval, a call, a run of the job queue)
 * may run before it is interrupted with "InternalError: interrupted"; 0 for
 * no limit. */
void qjs_set_time_limit(void *qp, int ms) { ((AeQjs *)qp)->limit_ms = ms > 0 ? ms : 0; }

/* The text of the last exception (empty if none). */
const char *qjs_error(void *qp) { AeQjs *q = (AeQjs *)qp; return q && q->err ? q->err : ""; }

/* Handles currently held: 0 once a host has released everything it took. */
int qjs_handles(void *qp) { return ((AeQjs *)qp)->live; }

/* Bytes the runtime has allocated. */
int64_t qjs_memory_used(void *qp) {
    JSMemoryUsage u;
    JS_ComputeMemoryUsage(((AeQjs *)qp)->rt, &u);
    return u.malloc_size;
}

/* ---- running code -------------------------------------------------------- */

/* flags: 1 = a module (its result is a promise), 2 = strict. */
int qjs_eval(void *qp, const char *code, const char *filename, int flags) {
    AeQjs *q = (AeQjs *)qp;
    int f = (flags & 1) ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
    if (flags & 2) f |= JS_EVAL_FLAG_STRICT;
    enter_(q);
    return result_(q, JS_Eval(q->ctx, code, strlen(code), filename, f));
}

/* Call fn with this and the elements of the array args (0: no arguments). */
int qjs_call(void *qp, int fn, int this_h, int args) {
    AeQjs *q = (AeQjs *)qp;
    JSValueConst f = get_(q, fn);
    if (!JS_IsFunction(q->ctx, f)) { set_err_(q, "TypeError: not a function"); return -1; }
    int64_t n = 0;
    JSValue *argv = NULL;
    if (args > 0) {
        JSValueConst a = get_(q, args);
        if (JS_GetLength(q->ctx, a, &n) < 0) return fail_(q);
        if (n > 65535) { set_err_(q, "RangeError: too many arguments"); return -1; }
        argv = (JSValue *)malloc(sizeof(JSValue) * (n ? n : 1));
        if (!argv) { set_err_(q, "InternalError: out of memory"); return -1; }
        for (int64_t i = 0; i < n; i++) argv[i] = JS_GetPropertyUint32(q->ctx, a, (uint32_t)i);
    }
    enter_(q);
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
int qjs_function(void *qp, const char *name, int arity, AeQjsClosure cb) {
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
    if (name && *name) {
        JS_DefinePropertyValueStr(q->ctx, f, "name", JS_NewString(q->ctx, name), JS_PROP_CONFIGURABLE);
    }
    return put_(q, f);
}

/* Throw from inside a host function; returns -1, which the function returns.
 * kind: "TypeError", "RangeError", "SyntaxError", "ReferenceError", else Error. */
int qjs_throw(void *qp, const char *kind, const char *msg) {
    AeQjs *q = (AeQjs *)qp;
    if (strcmp(kind, "TypeError") == 0) JS_ThrowTypeError(q->ctx, "%s", msg);
    else if (strcmp(kind, "RangeError") == 0) JS_ThrowRangeError(q->ctx, "%s", msg);
    else if (strcmp(kind, "SyntaxError") == 0) JS_ThrowSyntaxError(q->ctx, "%s", msg);
    else if (strcmp(kind, "ReferenceError") == 0) JS_ThrowReferenceError(q->ctx, "%s", msg);
    else {
        JSValue e = JS_NewError(q->ctx);
        JS_DefinePropertyValueStr(q->ctx, e, "message", JS_NewString(q->ctx, msg),
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        JS_Throw(q->ctx, e);
    }
    return -1;
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
int qjs_string(void *qp, const char *s) {
    AeQjs *q = (AeQjs *)qp;
    return result_(q, JS_NewString(q->ctx, s ? s : ""));
}
int qjs_object(void *qp) { AeQjs *q = (AeQjs *)qp; return result_(q, JS_NewObject(q->ctx)); }
int qjs_array(void *qp) { AeQjs *q = (AeQjs *)qp; return result_(q, JS_NewArray(q->ctx)); }

/* JSON.parse(text); -1 with qjs_error on bad JSON. */
int qjs_parse_json(void *qp, const char *text) {
    AeQjs *q = (AeQjs *)qp;
    enter_(q);
    return result_(q, JS_ParseJSON(q->ctx, text, strlen(text), "<json>"));
}

/* JSON.stringify(h), or "" (with qjs_error) if it cannot be. */
const char *qjs_to_json(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    enter_(q);
    JSValue s = JS_JSONStringify(q->ctx, get_(q, h), JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(s)) { fail_(q); return ""; }
    if (JS_IsUndefined(s)) return "";
    size_t n;
    const char *c = JS_ToCStringLen(q->ctx, &n, s);
    const char *out = c ? hand_out_(q, c, n) : "";
    if (c) JS_FreeCString(q->ctx, c);
    JS_FreeValue(q->ctx, s);
    return out;
}

/* String(h); valid until the next string this runtime hands out. */
const char *qjs_to_string(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    size_t n;
    const char *c = JS_ToCStringLen(q->ctx, &n, get_(q, h));
    if (!c) { fail_(q); return ""; }
    const char *out = hand_out_(q, c, n);
    JS_FreeCString(q->ctx, c);
    return out;
}

int qjs_to_int(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    int32_t n = 0;
    if (JS_ToInt32(q->ctx, &n, get_(q, h)) < 0) fail_(q);
    return n;
}

double qjs_to_float(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
    double d = 0;
    if (JS_ToFloat64(q->ctx, &d, get_(q, h)) < 0) fail_(q);
    return d;
}

int qjs_to_bool(void *qp, int h) {
    AeQjs *q = (AeQjs *)qp;
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

int qjs_get(void *qp, int obj, const char *name) {
    AeQjs *q = (AeQjs *)qp;
    enter_(q);
    return result_(q, JS_GetPropertyStr(q->ctx, get_(q, obj), name));
}

/* obj[name] = val (val stays held by its handle too); 1, or -1. */
int qjs_set(void *qp, int obj, const char *name, int val) {
    AeQjs *q = (AeQjs *)qp;
    enter_(q);
    if (JS_SetPropertyStr(q->ctx, get_(q, obj), name, JS_DupValue(q->ctx, get_(q, val))) < 0) return fail_(q);
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
    if (JS_GetLength(q->ctx, get_(q, obj), &n) < 0) return fail_(q);
    return (int)n;
}

/* An array of obj's own enumerable string keys. */
int qjs_keys(void *qp, int obj) {
    AeQjs *q = (AeQjs *)qp;
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
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
