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
    if (!aether_sandbox_check("fs_write", path)) return 0;

#ifdef _WIN32
    // Windows implementation with UTF-8 to UTF-16 path conversion
    int wpath_len = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wpath_len <= 0) return 0;

    // Allocate transient wide-char buffer
    size_t wpath_bytes = (size_t)wpath_len * sizeof(wchar_t);
    wchar_t* wpath = (wchar_t*)aether_caps_malloc(wpath_bytes);
    if (!wpath) return 0;

    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wpath_len);

    // Open handle to file/directory/symlink.
    // Use FILE_FLAG_BACKUP_SEMANTICS to allow opening directories,
    // and FILE_FLAG_OPEN_REPARSE_POINT to prevent following symbolic links/junctions.
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (is_symlink) {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }

    HANDLE h = CreateFileW(wpath, FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, flags, NULL);
    aether_caps_free(wpath, wpath_bytes);

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

    int flags = 0;
    if (is_symlink) {
#ifdef AT_SYMLINK_NOFOLLOW
        flags |= AT_SYMLINK_NOFOLLOW;
#else
        /* Never follow a symlink merely to preserve its timestamp. */
        return 0;
#endif
    }

    // utimensat is standard POSIX.1-2008 and available on virtually all modern systems.
    if (utimensat(AT_FDCWD, path, times, flags) == 0) {
        return 1;
    }

    return 0;
#endif
}

int aether_tar_get_mode_raw(const char* path) {
    if (!path) return 0;
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
