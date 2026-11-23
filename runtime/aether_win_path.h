// aether_win_path.h: the wide path a Win32 call is given, with no MAX_PATH
// limit (#3850).
//
// A Win32 call takes a path of fewer than MAX_PATH (260) characters, and
// CreateDirectoryW one of fewer than 248, unless the process is long-path
// aware (its manifest says so and the LongPathsEnabled policy is on) or the
// path is in the "\\?\" namespace. A tree made relative to directory handles
// (the sandbox's mkdir_p, an archive extracted on a long-path-aware system)
// can have longer paths, which every call by name then failed on: std.fs
// could not stat, list or remove what std.zip had extracted.
//
// So a path at or past the limit is given to a call the way Rust's std gives
// it (get_long_path, library/std/src/sys/path/windows.rs): made absolute by
// GetFullPathNameW, which also turns "/" into "\" and resolves "." and "..",
// as Win32 does before the file system sees a path, and then, when it is
// still that long, prefixed "\\?\" ("\\?\UNC\" for a UNC path), a form the
// file system takes literally. A path under the limit is given exactly as it
// was, and one already in the "\\?\" or "\??\" namespace too.
//
// Shared by std.fs (std/fs/aether_fs.c) and the sandbox's Windows calls
// (aether_sandbox.c, aether_sandbox_path.h), so every call by name in both
// takes a long path alike. Header-only.
#ifndef AETHER_WIN_PATH_H
#define AETHER_WIN_PATH_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// The length, in UTF-16 units with the terminating NUL, from which a path is
// given its long form: MAX_PATH less the 12 CreateDirectoryW keeps for an
// 8.3 name, as Rust's LEGACY_MAX_PATH.
#define AETHER_WIN_LONG_PATH (MAX_PATH - 12)

// 1 when `w` is to be given in its long form: at or past the limit, and not
// in the "\\?\" or "\??\" namespace already.
static inline int aether_win_path_is_long(const wchar_t* w) {
    if (wcslen(w) + 1 < AETHER_WIN_LONG_PATH) return 0;
    if (w[0] == L'\\' && (w[1] == L'\\' || w[1] == L'?') && w[2] == L'?' && w[3] == L'\\') return 0;
    return 1;
}

// The long form of `w` into `out` (`cap` units): its length, NUL not
// counted, or 0 when GetFullPathNameW fails or it does not fit. With `cap`
// 0 (and `out` NULL): the units to give `out` for it, or 0.
static inline DWORD aether_win_long_form(const wchar_t* w, wchar_t* out, DWORD cap) {
    // "\\?\UNC\" in place of a UNC path's "\\" is 6 units more than the
    // absolute path GetFullPathNameW gives; written 6 units in, the prefix
    // then goes before it.
    DWORD need = GetFullPathNameW(w, 0, NULL, NULL);   // its NUL included
    if (need == 0) return 0;
    if (cap == 0) return need + 6;
    if (cap <= 6) return 0;
    wchar_t* full = out + 6;
    DWORD n = GetFullPathNameW(w, cap - 6, full, NULL);
    if (n == 0 || n >= cap - 6) return 0;
    const wchar_t* pre = L"";
    DWORD skip = 0;
    if (n + 1 >= AETHER_WIN_LONG_PATH) {
        if (full[0] == L'\\' && (full[1] == L'\\' || full[1] == L'?') && full[2] == L'?' &&
            full[3] == L'\\') {
            // in the namespace already
        } else if (full[0] == L'\\' && full[1] == L'\\' && full[2] == L'.' && full[3] == L'\\') {
            pre = L"\\\\?\\";             // \\.\X -> \\?\X
            skip = 4;
        } else if (full[0] == L'\\' && full[1] == L'\\') {
            pre = L"\\\\?\\UNC\\";        // \\server\share -> \\?\UNC\server\share
            skip = 2;
        } else if (full[0] && full[1] == L':' && full[2] == L'\\') {
            pre = L"\\\\?\\";             // C:\x -> \\?\C:\x
        }
    }
    DWORD pl = (DWORD)wcslen(pre);
    memmove(out + pl, full + skip, (size_t)(n - skip + 1) * sizeof(wchar_t));
    memcpy(out, pre, (size_t)pl * sizeof(wchar_t));
    return pl + n - skip;
}

// UTF-8 `utf8` into `out` (`cap` units) as a Win32 call is to be given it:
// converted (MultiByteToWideChar with `flags`), then its long form when it
// is that long. 1, or 0 when it is not valid UTF-8 or does not fit.
static inline int aether_win_path_into(const char* utf8, DWORD flags, wchar_t* out, DWORD cap) {
    if (!MultiByteToWideChar(CP_UTF8, flags, utf8, -1, out, (int)cap)) return 0;
    if (!aether_win_path_is_long(out)) return 1;
    DWORD need = aether_win_long_form(out, NULL, 0);
    if (need == 0 || need > cap) return 0;
    wchar_t* tmp = (wchar_t*)malloc((size_t)need * sizeof(wchar_t));
    if (!tmp) return 0;
    DWORD n = aether_win_long_form(out, tmp, need);
    if (n) memcpy(out, tmp, ((size_t)n + 1) * sizeof(wchar_t));
    free(tmp);
    return n != 0;
}

// The final path of what `h` is open on, as GetFinalPathNameByHandleW gives
// it with FILE_NAME_NORMALIZED | VOLUME_NAME_DOS (in the "\\?\" namespace):
// its length, NUL not counted, in `out` (`cap` units); when `out` is too
// small (or `cap` is 0), the units it needs, NUL counted; 0 on failure, with
// the call's own error.
//
// Wine (10.0, dlls/kernelbase/file.c) asks NtQueryObject for the handle's
// name into a buffer of MAX_PATH units, so for a file whose path is longer
// GetFinalPathNameByHandleW fails (STATUS_BUFFER_OVERFLOW) where Windows
// answers, and a sandbox check that verifies a handle by its final path
// refused every long path there (#3850). Wine's NtQueryObject itself has no
// such cap: it names a file "\??\D:\..." (or "\??\UNC\..."), and Wine's
// final DOS path is that name with "\\?\" for "\??\". So when the call
// fails, the name NtQueryObject gives with room for any length is taken the
// way Wine takes it. Windows names a file "\Device\HarddiskVolumeN\...",
// which no DOS path is made from here: there the call's failure stands.
//
// aether_win_nt_name_dos is that step alone: the final DOS path Wine makes
// of an object name `name` (`units` long, no NUL needed), into `out` (`cap`
// units), with aether_win_final_path's return. 0 for a name in no DOS
// namespace (not "\??\...").
static inline DWORD aether_win_nt_name_dos(const WCHAR* name, DWORD units, wchar_t* out, DWORD cap) {
    if (!name || units < 4 || name[0] != L'\\' || name[1] != L'?' || name[2] != L'?' || name[3] != L'\\')
        return 0;
    DWORD rest = units - 4;
    if (!out || cap < rest + 5) return rest + 5;
    memcpy(out, L"\\\\?\\", 4 * sizeof(WCHAR));
    memcpy(out + 4, name + 4, (size_t)rest * sizeof(WCHAR));
    out[4 + rest] = L'\0';
    return rest + 4;
}

static inline DWORD aether_win_final_path(HANDLE h, wchar_t* out, DWORD cap) {
    DWORD n = GetFinalPathNameByHandleW(h, out, cap, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n != 0) return n;
    DWORD err = GetLastError();
    typedef LONG (NTAPI * query_object_fn)(HANDLE, int, PVOID, ULONG, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    FARPROC p = ntdll ? GetProcAddress(ntdll, "NtQueryObject") : NULL;
    query_object_fn query;
    memcpy(&query, &p, sizeof query);
    // OBJECT_NAME_INFORMATION: a UNICODE_STRING, the name after it.
    typedef struct { USHORT Length; USHORT MaximumLength; WCHAR* Buffer; } name_info;
    ULONG size = (ULONG)sizeof(name_info) + 65536;   // any UNICODE_STRING fits
    name_info* info = query ? (name_info*)malloc(size) : NULL;
    ULONG used = 0;
    DWORD result = 0;
    if (info && query(h, 1 /* ObjectNameInformation */, info, size, &used) >= 0)
        result = aether_win_nt_name_dos(info->Buffer, info->Length / sizeof(WCHAR), out, cap);
    free(info);
    if (result == 0) SetLastError(err);
    return result;
}

// For a narrow call (fopen, _stat, FindFirstFileA) that has always been given
// `utf8`: 1 with its long form in `out` (`cap` units) when it is at or past
// the limit, for the wide call to be given instead; 0 otherwise, and the
// narrow call takes it as it always has (a path under the limit, or one
// whose long form cannot be made here, which then fails as it did).
static inline int aether_win_long_into(const char* utf8, wchar_t* out, DWORD cap) {
    // UTF-16 never has more units than UTF-8 has bytes: a short string is
    // a short path.
    if (strlen(utf8) + 1 < AETHER_WIN_LONG_PATH) return 0;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, (int)cap)) return 0;
    if (!aether_win_path_is_long(out)) return 0;
    return aether_win_path_into(utf8, MB_ERR_INVALID_CHARS, out, cap);
}

#endif // _WIN32
#endif // AETHER_WIN_PATH_H
