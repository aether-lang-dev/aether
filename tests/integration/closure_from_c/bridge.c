/* C side of the closure_from_c test (#3622): closures built with the public
 * constructor in include/aether_closure.h, for probe.ae to call. Nothing
 * here knows how a closure is laid out. */
#include <stdlib.h>
#include "aether_closure.h"

#define CTX_MAGIC 0x5eed

typedef struct {
    int magic;
    int base;
} Ctx;

static int released_count = 0;
static int released_wrong = 0;

/* The `release` the closure was made with: counts its runs, and checks it
 * was handed the env given to aether_closure_new. */
static void ctx_release(void* env) {
    Ctx* c = (Ctx*)env;
    released_count++;
    if (!c || c->magic != CTX_MAGIC) {
        released_wrong++;
        return;
    }
    c->magic = 0;
    free(c);
}

/* The closure's function: its env handle first, then the Aether argument. */
static int add(AetherClosureSelf* self, int x) {
    Ctx* c = (Ctx*)aether_closure_self_env(self);
    if (!c || c->magic != CTX_MAGIC) return -1000;
    return c->base + x;
}

static int seven(AetherClosureSelf* self) {
    return aether_closure_self_env(self) == NULL ? 7 : -7;
}

void* make_adder(int base) {
    Ctx* c = (Ctx*)malloc(sizeof(Ctx));
    if (!c) return NULL;
    c->magic = CTX_MAGIC;
    c->base = base;
    void* f = aether_closure_new((void (*)(void))add, c, ctx_release);
    if (!f) free(c);
    return f;
}

/* No env and no release. */
void* make_seven(void) {
    return aether_closure_new((void (*)(void))seven, NULL, NULL);
}

void* share(void* f) { return aether_closure_retain(f); }
void drop(void* f) { aether_closure_release(f); }

int env_base(void* f) {
    Ctx* c = (Ctx*)aether_closure_env(f);
    return c ? c->base : -1;
}

/* 1 when `f` (a closure Aether boxed) has no env made here. */
int foreign_env_is_null(void* f) { return aether_closure_env(f) == NULL; }

int released(void) { return released_count; }
int released_wrongly(void) { return released_wrong; }
int new_refuses_null_fn(void) { return aether_closure_new(NULL, NULL, NULL) == NULL; }
