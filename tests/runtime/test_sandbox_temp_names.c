// #3817: fs.make_temp_file and fs.make_temp_dir inside an enforce block
// (aether_sandbox_temp_in) give each call a name of its own, even when the
// one before was deleted at once.
//
// On Windows the names came from the tick count alone, so every call in the
// same 15 ms tick started at the same name: a directory made and removed,
// then a file asked for under the same prefix, asked for that name again,
// and a delete still pending on it failed the call. The burst below makes
// and removes a file and a directory in turn, as std/sandbox's
// test_sandbox_checked_use does after its race, and every name must differ.
#include "test_harness.h"
#include "../../runtime/aether_sandbox.h"
#include "../../std/collections/aether_collections.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define ae_test_mkdir(p) _mkdir(p)
#define ae_test_rmdir(p) _rmdir(p)
#define ae_test_pid() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define ae_test_mkdir(p) mkdir((p), 0700)
#define ae_test_rmdir(p) rmdir(p)
#define ae_test_pid() getpid()
#endif

#define BURST 32

TEST_CATEGORY(sandbox_temp_names_differ_in_a_burst, TEST_CATEGORY_RUNTIME) {
    const char* tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    // The names are made in box/d under a grant on everything in box, as in
    // the sandbox test: the grant covers d, the directory they are made in.
    char box[512], dir[540];
    snprintf(box, sizeof box, "%s/ae_temp_names_%d", tmp, (int)ae_test_pid());
    snprintf(dir, sizeof dir, "%s/d", box);
    ASSERT_EQ(0, ae_test_mkdir(box));
    ASSERT_EQ(0, ae_test_mkdir(dir));

    char pattern[600];
    snprintf(pattern, sizeof pattern, "%s/*", box);
    ArrayList* grants = list_new();
    ASSERT_NOT_NULL(grants);
    list_add(grants, strdup("fs_read"));
    list_add(grants, aether_sandbox_grant_pattern("fs_read", pattern));
    list_add(grants, strdup("fs_write"));
    list_add(grants, aether_sandbox_grant_pattern("fs_write", pattern));

    static char names[BURST][600];
    int made = 0;
    aether_sandbox_push(grants);
    for (int i = 0; i < BURST; i++) {
        int is_dir = i % 2;
        if (aether_sandbox_temp_in(dir, "t", is_dir, names[i], sizeof names[i]) != 0) break;
        made++;
        if (is_dir) ae_test_rmdir(names[i]);
        else remove(names[i]);
    }
    aether_sandbox_pop();

    int repeats = 0;
    for (int i = 0; i < made; i++)
        for (int j = i + 1; j < made; j++)
            if (strcmp(names[i], names[j]) == 0) repeats++;

    for (int i = 0; i < list_size(grants); i++) free(list_get_raw(grants, i));
    list_free(grants);
    ae_test_rmdir(dir);
    ae_test_rmdir(box);

    ASSERT_EQ(BURST, made);
    ASSERT_EQ(0, repeats);
}
