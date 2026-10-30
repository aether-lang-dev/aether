// aether_host_quickjs.c — Embedded QuickJS Host Module C Bridge
#include "aether_host_quickjs.h"
#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AETHER_HAS_QUICKJS

typedef struct { void *fn; void *env; } AeQjsClosure;

extern void* qjs_new(int memory_kb, int stack_kb);
extern void qjs_free(void* q);
extern void qjs_set_time_limit(void* q, int ms);
extern int qjs_eval(void* q, const char* code, const char* filename, int flags);
extern const char* qjs_error(void* q);
extern void qjs_release(void* q, int h);
extern int qjs_function(void *qp, const char *name, int arity, AeQjsClosure cb);
extern void qjs_set_global(void *qp, const char *name, int v);
extern int qjs_string(void *qp, const char *s);
extern int qjs_undefined(void *qp);
extern int qjs_get_index(void *qp, int obj, int i);
extern const char *qjs_to_string(void *qp, int h);

static void qjs_set_global_c(void* q, const char* name, int v) {
    int g = qjs_global(q);
    qjs_set(q, g, name, v);
    qjs_release(q, g);
}

static const char* qjs_arg_string_c(void* q, int args, int i) {
    int h = qjs_get_index(q, args, i);
    const char* s = qjs_to_string(q, h);
    qjs_release(q, h);
    return s ? s : "";
}

int quickjs_init(void) {
    return 0;
}

void quickjs_finalize(void) {
}

int quickjs_run(const char* code) {
    if (!code) return -1;
    void* q = qjs_new(32768, 0);
    if (!q) return -1;
    qjs_set_time_limit(q, 5000);
    int h = qjs_eval(q, code, "<eval>", 0);
    if (h < 0) {
        fprintf(stderr, "[js] %s\n", qjs_error(q));
        qjs_free(q);
        return -1;
    }
    qjs_release(q, h);
    qjs_free(q);
    return 0;
}

int quickjs_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) return -1;
    aether_sandbox_push(perms);
    int res = quickjs_run(code);
    aether_sandbox_pop();
    return res;
}

static uint64_t quickjs_current_map_token = 0;

static int quickjs_map_get_cb(void* env, void* q, int this_h, int args) {
    (void)env; (void)this_h;
    const char* key = qjs_arg_string_c(q, args, 0);
    const char* val = aether_shared_map_get_by_token(quickjs_current_map_token, key);
    if (val) return qjs_string(q, val);
    return qjs_undefined(q);
}

static int quickjs_map_put_cb(void* env, void* q, int this_h, int args) {
    (void)env; (void)this_h;
    const char* key_tmp = qjs_arg_string_c(q, args, 0);
    char* key = strdup(key_tmp ? key_tmp : "");
    const char* val = qjs_arg_string_c(q, args, 1);
    aether_shared_map_put_by_token(quickjs_current_map_token, key, val);
    free(key);
    return qjs_undefined(q);
}

int quickjs_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    void* q = qjs_new(32768, 0);
    if (!q) return -1;
    qjs_set_time_limit(q, 5000);

    aether_shared_map_freeze_inputs_by_token(map_token);
    quickjs_current_map_token = map_token;

    AeQjsClosure get_c = { (void*)quickjs_map_get_cb, NULL };
    int get_fn = qjs_function(q, "aether_map_get", 1, get_c);
    qjs_set_global_c(q, "aether_map_get", get_fn);
    qjs_release(q, get_fn);

    AeQjsClosure put_c = { (void*)quickjs_map_put_cb, NULL };
    int put_fn = qjs_function(q, "aether_map_put", 2, put_c);
    qjs_set_global_c(q, "aether_map_put", put_fn);
    qjs_release(q, put_fn);

    if (aether_sandbox_depth() >= AETHER_SANDBOX_MAX_LEVELS) {
        qjs_free(q);
        quickjs_current_map_token = 0;
        return -1;
    }
    aether_sandbox_push(perms);
    int h = qjs_eval(q, code, "<eval>", 0);
    aether_sandbox_pop();
    quickjs_current_map_token = 0;

    if (h < 0) {
        fprintf(stderr, "[js] %s\n", qjs_error(q));
        qjs_free(q);
        return -1;
    }
    qjs_release(q, h);
    qjs_free(q);
    return 0;
}

#else
int quickjs_init(void) { return 0; }
void quickjs_finalize(void) {}
int quickjs_run(const char* code) { (void)code; return -1; }
int quickjs_run_sandboxed(void* perms, const char* code) { (void)perms; (void)code; return -1; }
int quickjs_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) { (void)perms; (void)code; (void)map_token; return -1; }
#endif
