// aether_sandbox.c — Global sandbox state
#include "aether_sandbox.h"

#ifdef AETHER_HAS_SANDBOX
#include <stdlib.h>
#include <string.h>
#include "aether_sandbox_path.h"

// Default: no sandbox checker installed = everything allowed
aether_sandbox_check_fn _aether_sandbox_checker = 0;

// The checker sees an fs path where it leads (aether_sandbox_path.h). A path
// that cannot be resolved is refused, not matched as written.
int aether_sandbox_check_slow(const char* category, const char* resource) {
    if (category && resource && category[0] == 'f' && category[1] == 's' &&
        (category[2] == '\0' || category[2] == '_')) {
        char resolved[PATH_MAX];
        if (!aether_sandbox_canon_path(resource, resolved, sizeof(resolved))) return 0;
        return _aether_sandbox_checker(category, resolved);
    }
    return _aether_sandbox_checker(category, resource);
}

// A grant's pattern in the form the checks compare against: an fs pattern's
// fixed directory resolved (std.sandbox's grant_fs_* call this), anything
// else copied. Returns a malloc'd string the grant list owns.
char* aether_sandbox_grant_pattern(const char* category, const char* pattern) {
    if (!pattern) return NULL;
    if (category && category[0] == 'f' && category[1] == 's') {
        char resolved[PATH_MAX];
        if (aether_sandbox_canon_pattern(pattern, resolved, sizeof(resolved))) return strdup(resolved);
    }
    return strdup(pattern);
}
#endif
