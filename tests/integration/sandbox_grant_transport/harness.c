#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int aether_spawn_sandboxed(void* grant_list, const char* program, const char* arg);
extern void* list_new(void);
extern int list_add_raw(void* list, void* item);
extern void list_free(void* list);

static int expect_result(const char* name, int got, int want) {
    if (got == want) return 0;
    fprintf(stderr, "FAIL %s: got %d, want %d\n", name, got, want);
    return 1;
}

static void* pair(const char* category, const char* pattern) {
    void* grants = list_new();
    if (!grants || !list_add_raw(grants, (void*)category) ||
        !list_add_raw(grants, (void*)pattern)) {
        fprintf(stderr, "FAIL cannot create grant pair\n");
        exit(2);
    }
    return grants;
}

int main(void) {
    int failures = 0;

    void* empty = list_new();
    if (!empty) return 2;
    failures += expect_result("empty deny-all list",
                              aether_spawn_sandboxed(empty, "/bin/true", NULL), 0);
    list_free(empty);

    failures += expect_result("null list",
                              aether_spawn_sandboxed(NULL, "/bin/true", NULL), -1);

    void* odd = list_new();
    if (!odd || !list_add_raw(odd, (void*)"exec")) return 2;
    failures += expect_result("odd list",
                              aether_spawn_sandboxed(odd, "/bin/true", NULL), -1);
    list_free(odd);

    void* null_item = pair(NULL, "*");
    failures += expect_result("null grant item",
                              aether_spawn_sandboxed(null_item, "/bin/true", NULL), -1);
    list_free(null_item);

    void* multiline = pair("fs_read", "/tmp/allowed\n*:*");
    failures += expect_result("multiline pattern",
                              aether_spawn_sandboxed(multiline, "/bin/true", NULL), -1);
    list_free(multiline);

    void* delimited_category = pair("fs_read:*", "*");
    failures += expect_result("category containing delimiter",
                              aether_spawn_sandboxed(delimited_category,
                                                     "/bin/true", NULL), -1);
    list_free(delimited_category);

    char* oversized_pattern = malloc(9001);
    if (!oversized_pattern) return 2;
    memset(oversized_pattern, 'a', 9000);
    oversized_pattern[9000] = '\0';
    void* oversized = pair("fs_read", oversized_pattern);
    failures += expect_result("oversized transport",
                              aether_spawn_sandboxed(oversized, "/bin/true", NULL), -1);
    list_free(oversized);
    free(oversized_pattern);

    void* allow = pair("*", "*");
    failures += expect_result("ordinary grant pair",
                              aether_spawn_sandboxed(allow, "/bin/true", NULL), 0);
    list_free(allow);

    if (failures) return 1;
    printf("PASS: sandbox grant transport validates complete input\n");
    return 0;
}
