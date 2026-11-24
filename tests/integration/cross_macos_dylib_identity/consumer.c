/* Links against the cross-built dylib by -l, then calls through it. Run on a
 * macOS host only: it proves dyld finds the library by its install name. */
int aether_greet(void);
int main(void) { return aether_greet() == 42 ? 0 : 1; }
