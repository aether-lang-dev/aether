#include <stdio.h>
#include <string.h>

extern int add(int, int);

/* An Aether `-> string` returns a refcounted AetherString*, NOT a char*.
 * Declaring it char* and printing it reads the struct header instead of the
 * payload; aether_string_data unwraps either representation. See
 * docs/aether-string-abi.md. */
extern const void* greet(const char*);
extern const char* aether_string_data(const void*);
/* The string greet returns is the caller's: one reference, released here. */
extern void string_release(const void*);

int main(void) {
    if (add(2, 40) != 42) {
        printf("FAIL add=%d\n", add(2, 40));
        return 1;
    }
    const void* gs = greet("world");
    const char* g = aether_string_data(gs);
    if (!g || strcmp(g, "hello world") != 0) {
        printf("FAIL greet=%s\n", g ? g : "(null)");
        return 1;
    }
    string_release(gs);
    printf("ok\n");
    return 0;
}
