#include "aether_tar.h"
#include "../../runtime/config/aether_optimization_config.h"
#include "../../runtime/utils/aether_compiler.h"
#include "../../runtime/aether_sandbox.h"
#include "../../runtime/aether_resource_caps.h"
#include "../string/aether_string.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>

#ifndef _WIN32
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#else
#include <windows.h>
#endif

int aether_tar_set_mtime_raw(const char* path, int64_t mtime, int is_symlink) {
    if (!path) return 0;
    // A symlink's own times are its own: the link is checked, not what it
    // leads to (#3751). The time is set on what the check saw: through a
    // handle verified after it was opened (Windows), from the anchor of
    // the resolved path (POSIX), never by the name a second time.

#ifdef _WIN32
    // FILE_FLAG_OPEN_REPARSE_POINT for a symlink, so the link itself is
    // opened and changed, not its target.
    HANDLE h = (HANDLE)aether_sandbox_win_open(path, FILE_WRITE_ATTRIBUTES, 0, is_symlink,
                                               AETHER_ACCESS_WRITE);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }

    // Convert Unix timestamp (seconds since 1970-01-01) to 100-nanosecond intervals since 1601-01-01.
    long long ll = mtime * 10000000ULL + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)ll;
    ft.dwHighDateTime = (DWORD)(ll >> 32);

    // Set modification time (and keep creation/access times unchanged)
    int ok = SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
    return ok ? 1 : 0;
#else
    // POSIX implementation using utimensat (which supports AT_SYMLINK_NOFOLLOW)
    struct timespec times[2];
    times[0].tv_sec = (time_t)mtime;
    times[0].tv_nsec = 0;
    times[1].tv_sec = (time_t)mtime;
    times[1].tv_nsec = 0;

#ifndef AT_SYMLINK_NOFOLLOW
    /* Never follow a symlink merely to preserve its timestamp. */
    if (is_symlink) return 0;
#endif

    // utimensat is standard POSIX.1-2008 and available on virtually all modern systems.
    return aether_sandbox_utimens(path, times, is_symlink) == 0 ? 1 : 0;
#endif
}

int aether_tar_get_mode_raw(const char* path) {
    if (!path) return 0;
    if (aether_sandbox_levels_here() > 0) {
        /* Looked up from the anchor of its directory, as checked (#3751). */
        AetherSandboxStat ls;
        return aether_sandbox_stat(path, 0, &ls) == 0 ? (int)ls.mode : 0;
    }
    if (!aether_sandbox_check("fs_read", path)) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (int)st.st_mode;
}

char* aether_tar_slice_string(const char* data, int offset, int len) {
    if (!data || len < 0) return NULL;
    const char* raw = data;
    if (is_aether_string(data)) {
        raw = ((const AetherString*)data)->data;
    }
    // Create a new AetherString with the sliced bytes
    AetherString* wrapped = string_new_with_length(raw + offset, (size_t)len);
    return (char*)wrapped;
}

void* aether_tar_join_name(const void* prefix, const void* name) {
    AetherString* prefix_slash = string_concat_wrapped(prefix, "/");
    if (!prefix_slash) return NULL;
    AetherString* joined = string_concat_wrapped(prefix_slash, name);
    string_release(prefix_slash);
    return joined;
}
