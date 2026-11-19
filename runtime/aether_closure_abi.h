#ifndef AETHER_CLOSURE_ABI_H
#define AETHER_CLOSURE_ABI_H

/* The closure layout generated code and the runtime share (#3622).
 *
 * PRIVATE. This header is the one definition of how a closure is laid out
 * in memory: every generated program includes it from its prologue, and
 * the runtime files that build or take apart a closure include it too, so
 * the two cannot disagree. Code outside the toolchain does not include it.
 * A C library or host that needs a closure uses the public constructor in
 * include/aether_closure.h, which only ever hands out opaque pointers, so
 * this layout can change in any release without breaking it.
 *
 * Three shapes:
 *
 *  - _AeClosure, a closure value: the function and its environment. A call
 *    through it passes `env` as the function's first argument, ahead of the
 *    declared parameters: `((R (*)(void*, A...))c.fn)(c.env, a...)`.
 *
 *  - _AeEnvHead, the fields every environment starts with: its destructor
 *    (#1398) and its reference count (#2494). Each holder of the env holds
 *    one reference; a holder gives it back by calling `_dtor(env)`, which
 *    is a release: it lowers `_refs` and tears the env down on the last
 *    one. The count is atomic because a closure can be released on another
 *    thread than the one that made it. A closure that captures nothing has
 *    no environment (env is NULL). Generated env structs open with
 *    _AE_ENV_HEAD_FIELDS rather than spelling the fields again.
 *
 *  - _AeClosureBox, a closure kept behind a `ptr` (box_closure, a closure
 *    stored into a `ptr` slot or a list): the value, then a tag that tells a
 *    real box from a raw code address (#1439). The {fn, env} prefix comes
 *    first so a reader that knows only _AeClosure reads the box the same
 *    way. A box is held by one owner and holds one reference to its env;
 *    release_closure() gives that reference back and frees the box. */

#include <stdatomic.h>

#define _AE_ENV_HEAD_FIELDS \
    void (*_dtor)(void*);   \
    atomic_long _refs;

typedef struct { void (*fn)(void); void* env; } _AeClosure;
typedef struct { _AE_ENV_HEAD_FIELDS } _AeEnvHead;
typedef struct { void (*fn)(void); void* env; unsigned long long tag; } _AeClosureBox;

#define _AE_CLOSURE_TAG 0xAEC105EDB0CEDULL

#endif
