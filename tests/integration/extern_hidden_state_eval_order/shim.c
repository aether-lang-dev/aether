/* A C extern with state of its own and no handle parameter (#2524): the
 * declaration shows nothing the call writes, yet `peek` after `tick` must
 * see the step. */
static int n;

int tick(void) { return ++n; }
int peek(void) { return n; }
