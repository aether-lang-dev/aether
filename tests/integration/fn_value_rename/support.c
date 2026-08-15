#include <stdio.h>
typedef int (*cb_t)(int);
static cb_t g_cb;
void reg_cb(void* h) { g_cb = (cb_t)h; }
int run_cb(int v) { return g_cb ? g_cb(v) : -1; }
int collide_me(int v);
