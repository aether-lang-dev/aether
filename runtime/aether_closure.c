/* Copyright (c) 2026 Aether Developers. */
/* Closures built in C (#3622). See include/aether_closure.h for the
 * contract and runtime/aether_closure_abi.h for the layout.
 *
 * A closure made here is the box and environment generated code makes for
 * a closure literal, built from the same definitions: the box is an
 * _AeClosureBox carrying _AE_CLOSURE_TAG, and the environment opens with
 * _AE_ENV_HEAD_FIELDS, its count starting at 1 for the box. What follows
 * the head is what this closure captures: the caller's env and release.
 * Its destructor is a release in the generated form (one reference back,
 * the teardown on the last, run through aether_teardown), so every holder
 * the generated code knows (an unbox, a capturing closure, a list, a
 * worker) retains and releases it with no case of its own. */
#include "aether_closure.h"
#include "aether_closure_abi.h"
#include "actors/aether_panic.h"
#include "actors/aether_unwind.h"

#include <stdlib.h>

typedef struct {
    _AE_ENV_HEAD_FIELDS
    void* user_env;
    void (*user_release)(void*);
} AeCClosureEnv;

static void c_closure_env_teardown(void* p) {
    AeCClosureEnv* e = (AeCClosureEnv*)p;
    if (e->user_release) e->user_release(e->user_env);
    free(e);
}

/* The env's `_dtor` while it lives: one reference back. The last one swaps
 * in the teardown, which aether_teardown expects first in the object, as
 * the generated `_closure_env_<id>_free` does. */
static void c_closure_env_release(void* p) {
    if (!p) return;
    AeCClosureEnv* e = (AeCClosureEnv*)p;
    if (atomic_fetch_sub_explicit(&e->_refs, 1, memory_order_acq_rel) != 1) return;
    e->_dtor = c_closure_env_teardown;
    aether_teardown(p);
}

static _AeClosureBox* closure_box_checked(const void* closure, const char* where) {
    const _AeClosureBox* b = (const _AeClosureBox*)closure;
    if (b->tag != _AE_CLOSURE_TAG) aether_panic(where);
    return (_AeClosureBox*)closure;
}

void* aether_closure_new(void (*fn)(void), void* env, void (*release)(void* env)) {
    if (!fn) return NULL;
    AeCClosureEnv* e = (AeCClosureEnv*)malloc(sizeof(AeCClosureEnv));
    _AeClosureBox* b = (_AeClosureBox*)malloc(sizeof(_AeClosureBox));
    if (!e || !b) {
        free(e);
        free(b);
        return NULL;
    }
    e->_dtor = c_closure_env_release;
    atomic_init(&e->_refs, 1);
    e->user_env = env;
    e->user_release = release;
    b->fn = fn;
    b->env = e;
    b->tag = _AE_CLOSURE_TAG;
    return b;
}

void* aether_closure_retain(void* closure) {
    if (!closure) return NULL;
    const _AeClosureBox* b =
        closure_box_checked(closure, "aether_closure_retain() on a value that is not a closure box");
    _AeClosureBox* copy = (_AeClosureBox*)malloc(sizeof(_AeClosureBox));
    if (!copy) return NULL;
    *copy = *b;
    if (copy->env)
        atomic_fetch_add_explicit(&((_AeEnvHead*)copy->env)->_refs, 1, memory_order_relaxed);
    return copy;
}

void aether_closure_release(void* closure) {
    if (!closure) return;
    _AeClosureBox* b =
        closure_box_checked(closure, "aether_closure_release() on a value that is not a closure box");
    /* The order release_closure() keeps: the env's reference, then the box. */
    if (b->env) ((_AeEnvHead*)b->env)->_dtor(b->env);
    free(b);
}

void* aether_closure_self_env(const AetherClosureSelf* self) {
    const AeCClosureEnv* e = (const AeCClosureEnv*)self;
    if (!e || e->_dtor != c_closure_env_release) return NULL;
    return e->user_env;
}

void* aether_closure_env(const void* closure) {
    if (!closure) return NULL;
    const _AeClosureBox* b =
        closure_box_checked(closure, "aether_closure_env() on a value that is not a closure box");
    return aether_closure_self_env((const AetherClosureSelf*)b->env);
}
