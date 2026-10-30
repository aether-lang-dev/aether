#include <stdio.h>
#include <stdlib.h>

extern int aether_spawn_sandboxed(void* grant_list, const char* program, const char* arg);
extern void* list_new(void);
extern int list_add_raw(void* list, void* item);
extern void list_free(void* list);

static void add(void* l, const char* s) {
    if (!list_add_raw(l, (void*)s)) { fprintf(stderr, "cannot build grants\n"); exit(2); }
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const char* child = argv[1];
    const char* file = argv[2];
    int failures = 0;

    /* Empty list: the child starts, confined, with nothing allowed. */
    void* none = list_new();
    if (!none) return 2;
    int rc = aether_spawn_sandboxed(none, child, file);
    list_free(none);
    if (rc != 0) {
        fprintf(stderr, "FAIL empty grants: child status %d, want 0 "
                "(bit0 read, bit1 connect, 64 exec were not refused; -1 = not started)\n", rc);
        failures++;
    }

    /* Control: the same child with matching grants is allowed all three. */
    void* all = list_new();
    if (!all) return 2;
    add(all, "fs_read"); add(all, file);
    add(all, "tcp");     add(all, "127.0.0.1");
    add(all, "exec");    add(all, "/bin/sh");
    rc = aether_spawn_sandboxed(all, child, file);
    list_free(all);
    if (rc != (1 | 2 | 64)) {
        fprintf(stderr, "FAIL granted control: child status %d, want %d\n", rc, 1 | 2 | 64);
        failures++;
    }

    if (failures) return 1;
    printf("PASS: empty grant list confines the child; matching grants allow it\n");
    return 0;
}
