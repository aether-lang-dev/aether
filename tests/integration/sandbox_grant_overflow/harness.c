#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern int aether_spawn_sandboxed(void* grant_list, const char* program, const char* arg);
extern void* list_new(void);
extern int list_add_raw(void* list, void* item);

// A grant entry that starts under the serialiser's 8000-byte cut-off but
// ends past its 8192-byte buffer must make the spawn fail (-1) without
// overrunning the stack buffer. Each "fs_read:<990 bytes>\n" entry is 999
// bytes, so eight end at 7992 and the ninth would end at 8991. (Entries that
// simply stop at the cut-off are dropped, which narrows the policy; that is
// not this case.)
int main(void) {
    static char pat[991];
    memset(pat, 'a', sizeof pat - 1);
    pat[0] = '/';
    void* grants = list_new();
    for (int i = 0; i < 20; i++) {
        if (!list_add_raw(grants, (void*)"fs_read") ||
            !list_add_raw(grants, (void*)pat)) {
            fprintf(stderr, "FAIL cannot build grant list\n");
            return 1;
        }
    }
    /* A program that exists (/usr/bin/true on FreeBSD), so a -1 means the
     * grant list was refused, not that the exec failed. */
    const char* true_bin = access("/bin/true", X_OK) == 0 ? "/bin/true" : "/usr/bin/true";
    int rc = aether_spawn_sandboxed(grants, true_bin, "");
    if (rc == -1) {
        printf("PASS: oversized grant list denied\n");
        return 0;
    }
    printf("FAIL: oversized grant list returned %d\n", rc);
    return 1;
}
