/* A host that defines a function with the name of one of the library's own
 * (#2937). On ELF the library's call to its add1 went through its exported
 * name and bound to this one, the first definition in the process. */

#include <stdio.h>
#include <stdint.h>

int32_t aether_twice(int32_t n);

int add1(int n) {
    return n + 100;
}

int main(void) {
    int32_t r = aether_twice(1);
    if (r != 3) {
        printf("FAIL: aether_twice(1) = %d, the library called the host's add1\n", (int)r);
        return 1;
    }
    printf("OK %d %d\n", (int)r, add1(0));
    return 0;
}
