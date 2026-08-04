/* The C header that owns the layout the Aether overlay declares. Field order
 * here is the authority; @c_verify checks the overlay against it. */
#ifndef AETHER_TEST_SHAPE_H
#define AETHER_TEST_SHAPE_H
#include <stdint.h>

typedef struct shape {
    void*    rax;      /* offset 0  */
    uint64_t length;   /* offset 8  */
    uint32_t slen;     /* offset 16 */
} shape;

shape* shape_new(void);
#endif
