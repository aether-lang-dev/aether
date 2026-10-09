#ifndef AETHER_ACTOR_THREAD_H
#define AETHER_ACTOR_THREAD_H

#include "../utils/aether_thread.h"

void* aether_actor_thread(void* actor);

#if AETHER_HAS_THREADS
// Starts an auto-process actor's own thread, running aether_actor_thread,
// and stores it in *thread. Returns 0, or the error pthread_create gave.
// Generated code calls this rather than pthread_create: a program's C sees
// the thread types only (AETHER_THREAD_TYPES_ONLY), not the calls behind
// them (#2673). Like its call site, it exists only where threads do.
int aether_actor_thread_start(pthread_t* thread, void* actor);
#endif

#endif
