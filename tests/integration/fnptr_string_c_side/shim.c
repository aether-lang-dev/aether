/* #2586: the C side of fnptr_string_c_side. */
#include <string.h>

/* A C function returning a string C owns (a literal, as strerror returns
 * a static buffer). */
static const char* fixed_name(int n) {
    return n > 0 ? "positive" : "other";
}

void* fixed_name_ptr(void) {
    return (void*)fixed_name;
}

/* Calls an Aether function handed over as a plain C function pointer and
 * reads its result as C text. */
int c_len_of(void* fn) {
    const char* (*f)(int) = (const char* (*)(int))fn;
    return (int)strlen(f(0));
}
