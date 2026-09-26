/* Copyright (c) 2026 Aether Developers. */
/* Observable struct models (#2220): a side table from object address to the
 * closures that want to hear about field stores on it.
 *
 * Codegen calls aether_observe_notify(&obj) after every field store on a
 * value whose struct is declared `struct Name @observable { ... }`. Nothing
 * lives in the struct itself: the observer list is keyed by the object's
 * address in a runtime-owned hash table, so an unobserved program pays one
 * relaxed counter read per store and no memory per object.
 *
 * Notification is synchronous, on the storing thread, and re-entrancy is
 * guarded per object: a store performed by an observer on the very object
 * being notified does not start a nested pass. Cross-thread marshalling is
 * the caller's job (std.worker's poster is the tool for that). */
#ifndef AETHER_OBSERVE_H
#define AETHER_OBSERVE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Layout-compatible view of codegen's `_AeClosure` ({fn, env}). The
 * observer is invoked as `fn(env, obj)` with the observed address. */
typedef struct { void (*fn)(void); void* env; } AetherObserverClosure;

/* Register `cb` as an observer of `obj`. Returns a positive token that
 * identifies the registration, or 0 when `obj` is NULL, `cb.fn` is NULL, or
 * memory ran out. The closure environment is owned by the runtime from this
 * call on: released by aether_unobserve / aether_unobserve_all, or at once
 * when the registration is refused. */
long aether_observe(void* obj, AetherObserverClosure cb);

/* Remove the registration `token` made on `obj`. Returns 1 when it was
 * found and removed, 0 otherwise. Safe to call from inside the observer
 * being removed: the environment is released once the current pass ends. */
int aether_unobserve(void* obj, long token);

/* Remove every observer of `obj` (for an object about to be freed). Returns
 * the number removed. */
int aether_unobserve_all(void* obj);

/* Number of observers currently registered on `obj`. */
int aether_observer_count(void* obj);

/* Run every observer of `obj`, in registration order. Called by generated
 * code after a field store on an @observable struct. Cheap when nothing
 * is observed anywhere. */
void aether_observe_notify(void* obj);

/* True while a notification pass for `obj` is running on some thread. Lets
 * a test (or a guard in host code) see the re-entrancy state. */
int aether_observe_is_notifying(void* obj);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_OBSERVE_H */
