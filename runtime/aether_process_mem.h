/* aether_process_mem.h — how much memory the process holds (#2310).
 *
 * Two views, for a same-process leak check that runs on every platform
 * (an editor's bounded run, a game's test) and not only under a leak tool:
 *
 *   - aether_heap_in_use(): the bytes the C allocator has handed out and not
 *     taken back, from the allocator's own statistics. Every malloc counts:
 *     Aether's (heap.new, strings, closures, collections) and the ones C
 *     code reached through an extern makes. A loop that leaks 40 small
 *     blocks per round shows it. glibc, macOS and jemalloc count freed
 *     blocks parked in per-thread caches as in use, and on a churning
 *     workload those settle over many rounds, so there compare steady
 *     states (the growth between two later rounds), never a single
 *     before/after.
 *
 *   - aether_heap_in_use_exact(): 1 where the count is exactly the blocks
 *     the program holds (Windows' heap walk, not Wine's, which counts a
 *     low-fragmentation group whole; a sanitizer's allocator), so any
 *     growth between steady rounds is a leak; 0 elsewhere.
 *
 *   - aether_process_resident() / aether_process_private(): what the OS
 *     charges the process: resident set, and private (not shared) memory.
 *     Covers mappings the allocator does not see, at page granularity.
 *
 * Each returns -1 where the platform does not provide it:
 *   heap_in_use: glibc (mallinfo2 / mallinfo), macOS (malloc zones),
 *                Windows (HeapWalk over the process's heaps), FreeBSD
 *                (jemalloc stats.allocated), Emscripten (mallinfo), and a
 *                sanitizer's allocator. On glibc it is also -1 when glibc
 *                does not serve malloc: under valgrind, or with an
 *                allocator preloaded in front of it.
 *   resident:    Linux, macOS, Windows, FreeBSD.
 *   private:     Linux (resident minus shared), macOS (physical
 *                footprint), Windows (private usage).
 */
#ifndef AETHER_PROCESS_MEM_H
#define AETHER_PROCESS_MEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t aether_heap_in_use(void);
int aether_heap_in_use_exact(void);
int64_t aether_process_resident(void);
int64_t aether_process_private(void);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_PROCESS_MEM_H */
