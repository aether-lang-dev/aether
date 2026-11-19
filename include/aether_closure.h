#ifndef AETHER_CLOSURE_H
#define AETHER_CLOSURE_H

/* Building an Aether closure from C (#3622).
 *
 * A C library or host that hands Aether code a callback makes it here. The
 * result is a closure in a `ptr`, the form `box_closure()` gives: the
 * Aether side opens it with `unbox_closure(p)`, calls it with `call(f, ...)`
 * and gives it back with `release_closure(p)`, exactly as for a closure
 * Aether made. The runtime owns the closure's layout; this header only ever
 * hands out opaque pointers, so a new release of Aether does not break a
 * caller compiled against an older one.
 *
 * The function. `fn` is called with one argument ahead of the Aether-side
 * parameters, as every Aether closure's function is: a handle on the
 * closure's environment. Read the `env` given to aether_closure_new back
 * from it with aether_closure_self_env():
 *
 *     static int add(AetherClosureSelf* self, int x) {
 *         Ctx* c = aether_closure_self_env(self);
 *         return c->base + x;
 *     }
 *     void* f = aether_closure_new((void (*)(void))add, ctx, ctx_free);
 *
 * Why a handle and not `env` itself: the first argument of a closure's
 * function is the environment the reference count lives in, which every
 * holder of the closure retains and releases through. That environment is
 * the runtime's, and it holds `env` and `release`; giving `fn` the caller's
 * pointer instead would take a trampoline per signature, which C cannot
 * write once for every signature, or a header the caller lays out itself,
 * which is the copy of the layout this API exists to remove.
 *
 * References. A closure from here takes part in the same reference count
 * as one Aether made. Each `void*` this header returns is one reference,
 * given back once, by aether_closure_release() in C or release_closure() in
 * Aether. A closure box has one owner, so a second reference is a second
 * box: aether_closure_retain() returns it, and both share the environment.
 * `release(env)` runs exactly once, when the last reference anywhere goes,
 * on whichever thread gives it back. An Aether closure that captured this
 * one, or a list that holds it, keeps a reference of its own, so `release`
 * waits for those too.
 *
 * All of this works the same on a closure Aether made and handed to C (a
 * `ptr` from box_closure, or an extern parameter declared `@retain ptr`),
 * except aether_closure_env() and aether_closure_self_env(), which answer
 * only for a closure made here. */

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define AETHER_CLOSURE_API __attribute__((visibility("default")))
#else
#  define AETHER_CLOSURE_API
#endif

/* What a closure's function receives first: never defined, only pointed at. */
typedef struct AetherClosureSelf AetherClosureSelf;

/* A closure that calls `fn` with its environment handle first, then the
 * Aether-side arguments. `env` is the caller's and is read back with
 * aether_closure_self_env(); `release`, which may be NULL, is called with it
 * once, when the last reference goes. `fn` must not be NULL. Returns one
 * reference, or NULL when `fn` is NULL or memory runs out; then nothing was
 * taken, and `env` is still the caller's to free. */
AETHER_CLOSURE_API void* aether_closure_new(void (*fn)(void), void* env,
                                            void (*release)(void* env));

/* A second reference to `closure`: a new box sharing its environment.
 * Release each, the original too, once. NULL when memory runs out. */
AETHER_CLOSURE_API void* aether_closure_retain(void* closure);

/* Gives one reference back and frees its box. On the last reference the
 * environment goes too, with the `release` it was made with. NULL is
 * ignored. */
AETHER_CLOSURE_API void aether_closure_release(void* closure);

/* The `env` a closure made by aether_closure_new() holds; NULL for any
 * other closure. Borrowed: it lives as long as the closure. */
AETHER_CLOSURE_API void* aether_closure_env(const void* closure);

/* The same, from inside the closure's function: `self` is its first
 * argument. NULL for anything aether_closure_new() did not make. */
AETHER_CLOSURE_API void* aether_closure_self_env(const AetherClosureSelf* self);

#ifdef __cplusplus
}
#endif

#endif
