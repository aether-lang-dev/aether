/* Calls the panicking export twice and the plain path once, printing what
 * each call returned and its outcome. */
#include <stdio.h>
extern int aether_boom(int);
extern int aether_last_outcome(void);

int main(void) {
    int a = aether_boom(1);
    int oa = aether_last_outcome();
    int b = aether_boom(2);
    int ob = aether_last_outcome();
    int c = aether_boom(0);
    int oc = aether_last_outcome();
    printf("boom %d outcome %d; boom %d outcome %d; plain %d outcome %d\n", a, oa, b, ob, c, oc);
    return 0;
}
