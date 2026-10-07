#include <stdint.h>

/* 1 when the actor struct does not sit on the 64-byte boundary its
 * generated type is declared with (#2485). */
int actor_misaligned(void* actor) {
    return ((uintptr_t)actor % 64) != 0;
}
