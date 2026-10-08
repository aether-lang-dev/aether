/* C side of the closure_extern_noescape regression (#2523).
 *
 * Two externs use the closure only during the call and are declared
 * `@noescape` in probe.ae: run_cb takes it by value (`fn`), run_box through
 * a `ptr` slot, whose box the caller builds on its own stack. Neither
 * stores or frees anything; the caller releases the environment once the
 * call returns.
 *
 * register_cb stores the closure for fire_cb to invoke later: the callback-
 * registry shape, left unannotated, so the caller must keep the environment
 * alive. The box layout mirrors the codegen prologue's `_AeClosure`
 * {fn, env} prefix. */

typedef struct {
    void (*fn)(void);
    void* env;
} AeClosure;

static AeClosure saved;

int run_cb(AeClosure boxed) {
    return ((int (*)(void*))boxed.fn)(boxed.env);
}

int run_box(void* box) {
    AeClosure clo = *(AeClosure*)box;
    return ((int (*)(void*))clo.fn)(clo.env);
}

void register_cb(AeClosure boxed) {
    saved = boxed;
}

int fire_cb(void) {
    if (!saved.fn) return -1;
    return ((int (*)(void*))saved.fn)(saved.env);
}
