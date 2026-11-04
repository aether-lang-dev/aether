/* A C host of results.ae (#3388), the library linked in (one runtime). Each
 * call has its first allocation fail and ends at its return with
 * AETHER_OUT_OF_MEMORY: it must return its zero value, and what it would
 * have returned must be released by the library, since a host releases only
 * what an AETHER_OK call returned. Then the same calls with nothing failed.
 *
 *   string NULL outcome 5, released yes
 *   int 0 outcome 5
 *   struct 0 (null) outcome 5, released yes
 *   closure (null) outcome 5, released yes
 *   ok: hello7 42 1 p7 outcome 0
 */
#include <stdio.h>
#include <stdint.h>
#include "libaether.h"

typedef struct Pair { int32_t a; const void* s; } Pair;
typedef struct { void (*fn)(void); void* env; } Closure;

extern const void* aether_greet(int32_t n);
extern int32_t aether_answer(int32_t n);
extern Pair aether_pair(int32_t n);
extern Closure aether_greeter(int32_t n);
extern const char* aether_string_data(const void* s);
extern void aether_results_string_release(const void* s);
extern uint64_t aether_caps_used_bytes(void);
extern void aether_test_fail_alloc_after(int n);

int main(void) {
    uint64_t base = aether_caps_used_bytes();
    aether_test_fail_alloc_after(1);
    const void* s = aether_greet(7);
    printf("string %s outcome %d, released %s\n", s ? aether_string_data(s) : "NULL",
           aether_last_outcome(), aether_caps_used_bytes() == base ? "yes" : "no");

    aether_test_fail_alloc_after(1);
    int32_t n = aether_answer(7);
    printf("int %d outcome %d\n", n, aether_last_outcome());

    base = aether_caps_used_bytes();
    aether_test_fail_alloc_after(1);
    Pair p = aether_pair(7);
    printf("struct %d %s outcome %d, released %s\n", p.a, p.s ? aether_string_data(p.s) : "(null)",
           aether_last_outcome(), aether_caps_used_bytes() == base ? "yes" : "no");

    base = aether_caps_used_bytes();
    aether_test_fail_alloc_after(1);
    Closure c = aether_greeter(7);
    printf("closure %s outcome %d, released %s\n", c.fn ? "a function" : "(null)", aether_last_outcome(),
           aether_caps_used_bytes() == base ? "yes" : "no");

    const void* ok = aether_greet(7);
    int okn = aether_answer(7);
    Pair okp = aether_pair(7);
    printf("ok: %s %d %d %s outcome %d\n", aether_string_data(ok), okn, okp.a, aether_string_data(okp.s),
           aether_last_outcome());
    aether_results_string_release(ok);
    aether_results_string_release(okp.s);
    fflush(stdout);
    return 0;
}
