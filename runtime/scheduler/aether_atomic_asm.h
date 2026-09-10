// Inline Assembly Atomic Operations - x86_64 Optimized
// Hand-written x86_64 assembly for the critical atomic primitives,
// avoiding compiler-emitted LOCK-prefix sequences where the tighter
// form is measurable on the hot path.

#ifndef AETHER_ATOMIC_ASM_H
#define AETHER_ATOMIC_ASM_H

#include <stdint.h>
#include <stdatomic.h>

// Detect x86_64 platform
#if defined(__x86_64__) || defined(_M_X64)
#define HAS_X86_64_ASM 1
#else
#define HAS_X86_64_ASM 0
#endif

#if HAS_X86_64_ASM

// Optimized atomic load with relaxed ordering (no fence needed)
static inline int atomic_load_relaxed_asm(atomic_int* ptr) {
    int result;
    __asm__ volatile(
        "movl %1, %0"
        : "=r" (result)
        : "m" (*ptr)
        : "memory"
    );
    return result;
}

// Optimized atomic store with release ordering
static inline void atomic_store_release_asm(atomic_int* ptr, int value) {
    __asm__ volatile(
        "movl %1, %0"
        : "=m" (*ptr)
        : "r" (value)
        : "memory"
    );
}

// Optimized atomic compare-and-swap (CAS)
// Returns 1 if successful, 0 if failed
static inline int atomic_cas_asm(atomic_int* ptr, int* expected, int desired) {
    int old_value = *expected;
    int result;
    
    __asm__ volatile(
        "lock cmpxchgl %3, %1\n\t"
        "sete %b0"
        : "=q" (result), "+m" (*ptr), "+a" (old_value)
        : "r" (desired)
        : "cc", "memory"
    );
    
    *expected = old_value;
    return result;
}

// Optimized atomic fetch-and-add
static inline int atomic_fetch_add_asm(atomic_int* ptr, int value) {
    int result;
    __asm__ volatile(
        "lock xaddl %0, %1"
        : "=r" (result), "+m" (*ptr)
        : "0" (value)
        : "cc", "memory"
    );
    return result;
}

// Optimized spin-lock using PAUSE instruction
static inline void spin_pause(void) {
    __asm__ volatile("pause" ::: "memory");
}

// Custom optimized spinlock using inline assembly
typedef struct {
    atomic_int locked;
} FastSpinlock;

static inline void spinlock_init(FastSpinlock* lock) {
    atomic_store_explicit(&lock->locked, 0, memory_order_relaxed);
}

static inline void spinlock_lock(FastSpinlock* lock) {
    int expected = 0;
    while (1) {
        // Try to acquire with CAS
        if (atomic_cas_asm(&lock->locked, &expected, 1)) {
            return;  // Acquired
        }
        
        // Spin with PAUSE for power efficiency
        do {
            spin_pause();
            expected = atomic_load_relaxed_asm(&lock->locked);
        } while (expected != 0);
        
        expected = 0;  // Reset for next CAS attempt
    }
}

static inline void spinlock_unlock(FastSpinlock* lock) {
    atomic_store_release_asm(&lock->locked, 0);
}

// Optimized atomic flag test-and-set
static inline int atomic_test_and_set_asm(atomic_int* ptr) {
    int result = 1;
    __asm__ volatile(
        "xchgl %0, %1"
        : "+r" (result), "+m" (*ptr)
        :
        : "memory"
    );
    return result;
}

#else
// Fallback to standard C atomics on non-x86_64

static inline int atomic_load_relaxed_asm(atomic_int* ptr) {
    return atomic_load_explicit(ptr, memory_order_relaxed);
}

static inline void atomic_store_release_asm(atomic_int* ptr, int value) {
    atomic_store_explicit(ptr, value, memory_order_release);
}

static inline int atomic_cas_asm(atomic_int* ptr, int* expected, int desired) {
    return atomic_compare_exchange_strong_explicit(
        ptr, expected, desired,
        memory_order_acq_rel, memory_order_acquire
    );
}

static inline int atomic_fetch_add_asm(atomic_int* ptr, int value) {
    return atomic_fetch_add_explicit(ptr, value, memory_order_acq_rel);
}

static inline void spin_pause(void) {
#if defined(__aarch64__) || defined(__arm64__)
    // ARM64: yield gives up CPU time slice to other threads
    __asm__ volatile("yield" ::: "memory");
#elif defined(__arm__)
    // ARM32: yield instruction
    __asm__ volatile("yield" ::: "memory");
#else
    // Generic: use compiler barrier
    __asm__ volatile("" ::: "memory");
#endif
}

typedef struct {
    atomic_int locked;
} FastSpinlock;

static inline void spinlock_init(FastSpinlock* lock) {
    atomic_store_explicit(&lock->locked, 0, memory_order_relaxed);
}

static inline void spinlock_lock(FastSpinlock* lock) {
    int expected = 0;
    while (!atomic_compare_exchange_weak_explicit(
        &lock->locked, &expected, 1,
        memory_order_acquire, memory_order_relaxed)) {
        while (atomic_load_explicit(&lock->locked, memory_order_relaxed)) {
            // Spin
        }
        expected = 0;
    }
}

static inline void spinlock_unlock(FastSpinlock* lock) {
    atomic_store_explicit(&lock->locked, 0, memory_order_release);
}

#endif // HAS_X86_64_ASM

#endif // AETHER_ATOMIC_ASM_H
