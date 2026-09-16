/* Binds the @c_callback symbol by name (the C side of the bridge) and is linked
 * against BOTH generated TUs, each of which defines shared_cb_symbol. */
#include <stdio.h>
extern int shared_cb_symbol(int);
int main(void) {
    int v = shared_cb_symbol(7);
    printf("cb(7)=%d\n", v);
    return v == 21 ? 0 : 1;
}
