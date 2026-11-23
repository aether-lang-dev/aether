/**
 * Long paths on Windows (#3850): aether_win_path.h, the one helper std.fs
 * and the sandbox give a Win32 call a path through.
 *
 * - A path under the limit is given as it is; one at or past it gets its
 *   long form: absolute, its separators and "." and ".." resolved, prefixed
 *   "\\?\" ("\\?\UNC\" for a UNC path).
 * - The final path of a handle on a file past MAX_PATH comes back whole.
 *   Wine's GetFinalPathNameByHandleW reads the object name into a buffer of
 *   MAX_PATH units and fails past it, so aether_win_final_path makes the
 *   final DOS path from NtQueryObject's "\??\..." name as Wine would; that
 *   step is aether_win_nt_name_dos, checked here on the names Wine gives
 *   (Windows never gives one in "\??\", and its own call answers).
 */

#include "test_harness.h"

#ifdef _WIN32
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../../runtime/aether_win_path.h"

TEST(win_long_path_short_untouched_long_prefixed) {
    wchar_t out[1024];
    /* Short: as given, even relative and with "/" and "..". */
    ASSERT_TRUE(aether_win_path_into("a/../b", 0, out, 1024));
    ASSERT_TRUE(wcscmp(out, L"a/../b") == 0);
    /* Long: absolute, normalised, prefixed. */
    char longp[600];
    int n = snprintf(longp, sizeof longp, "C:/x/../y");
    for (int i = 0; i < 60; i++) n += snprintf(longp + n, sizeof longp - (size_t)n, "/dir%02d", i);
    ASSERT_TRUE(aether_win_path_into(longp, 0, out, 1024));
    ASSERT_TRUE(wcsncmp(out, L"\\\\?\\C:\\y\\dir00\\dir01", 20) == 0);
    ASSERT_TRUE(wcschr(out, L'/') == NULL);
    ASSERT_TRUE(wcsstr(out, L"..") == NULL);
    /* UNC: \\server\share becomes \\?\UNC\server\share. */
    wchar_t unc[600];
    int u = _snwprintf(unc, 600, L"\\\\srv\\share");
    for (int i = 0; i < 60; i++) u += _snwprintf(unc + u, 600 - (size_t)u, L"\\dir%02d", i);
    ASSERT_TRUE(aether_win_path_is_long(unc));
    ASSERT_TRUE(aether_win_long_form(unc, out, 1024) > 0);
    ASSERT_TRUE(wcsncmp(out, L"\\\\?\\UNC\\srv\\share\\dir00", 23) == 0);
    /* Already in the namespace: left alone. */
    wcscpy(unc, L"\\\\?\\C:\\");
    for (int i = 0; i < 60; i++) wcscat(unc, L"dir\\");
    ASSERT_TRUE(!aether_win_path_is_long(unc));
}

TEST(win_nt_name_dos_as_wine_makes_it) {
    wchar_t out[1024];
    /* A drive path: "\??\" becomes "\\?\". */
    const WCHAR* drive = L"\\??\\D:\\box\\a";
    DWORD n = aether_win_nt_name_dos(drive, (DWORD)wcslen(drive), out, 1024);
    ASSERT_EQ(12, (int)n);
    ASSERT_TRUE(wcscmp(out, L"\\\\?\\D:\\box\\a") == 0);
    /* A UNC path keeps its UNC part. */
    const WCHAR* unc = L"\\??\\UNC\\srv\\share\\f";
    n = aether_win_nt_name_dos(unc, (DWORD)wcslen(unc), out, 1024);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(wcscmp(out, L"\\\\?\\UNC\\srv\\share\\f") == 0);
    /* The length is the name's, not up to a NUL. */
    n = aether_win_nt_name_dos(drive, 10, out, 1024);
    ASSERT_TRUE(wcscmp(out, L"\\\\?\\D:\\box") == 0);
    /* Too small: the units it needs, NUL counted, nothing written. */
    out[0] = L'#';
    ASSERT_EQ(13, (int)aether_win_nt_name_dos(drive, (DWORD)wcslen(drive), out, 12));
    ASSERT_TRUE(out[0] == L'#');
    ASSERT_EQ(13, (int)aether_win_nt_name_dos(drive, (DWORD)wcslen(drive), NULL, 0));
    ASSERT_EQ(12, (int)aether_win_nt_name_dos(drive, (DWORD)wcslen(drive), out, 13));
    /* A name in no DOS namespace (Windows' "\Device\..."): none. */
    const WCHAR* dev = L"\\Device\\HarddiskVolume3\\box\\a";
    ASSERT_EQ(0, (int)aether_win_nt_name_dos(dev, (DWORD)wcslen(dev), out, 1024));
    /* Past MAX_PATH: whole. */
    WCHAR big[700];
    wcscpy(big, L"\\??\\C:");
    for (int i = 0; i < 100; i++) wcscat(big, L"\\abcde");
    n = aether_win_nt_name_dos(big, (DWORD)wcslen(big), out, 1024);
    ASSERT_EQ((int)wcslen(big), (int)n);
    ASSERT_TRUE(wcsncmp(out, L"\\\\?\\C:\\abcde", 12) == 0);
    ASSERT_TRUE(wcscmp(out + 4, big + 4) == 0);
}

TEST(win_final_path_of_a_long_directory) {
    /* A directory 60 levels below the temporary directory, made by its
     * long form, opened, and its final path asked: the whole path, in the
     * "\\?\" namespace. */
    wchar_t tmp[MAX_PATH + 1];
    DWORD tl = GetTempPathW(MAX_PATH + 1, tmp);
    ASSERT_TRUE(tl > 0 && tl <= MAX_PATH);
    wchar_t path[2048];
    _snwprintf(path, 2048, L"%lsae_win_long_%lu", tmp, (unsigned long)GetCurrentProcessId());
    wchar_t lp[2200];
    size_t base = wcslen(path);
    int levels = 0;
    for (; levels < 60; levels++) {
        if (levels > 0) wcscat(path, L"\\abcdef");
        DWORD ln = aether_win_path_is_long(path) ? aether_win_long_form(path, lp, 2200) : 0;
        if (!ln) wcscpy(lp, path);
        if (!CreateDirectoryW(lp, NULL)) break;
    }
    ASSERT_EQ(60, levels);
    ASSERT_TRUE(wcslen(path) > MAX_PATH);
    ASSERT_TRUE(aether_win_long_form(path, lp, 2200) > 0);
    HANDLE h = CreateFileW(lp, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    ASSERT_TRUE(h != INVALID_HANDLE_VALUE);
    wchar_t fin[2200];
    DWORD need = aether_win_final_path(h, NULL, 0);
    DWORD got = aether_win_final_path(h, fin, 2200);
    CloseHandle(h);
    ASSERT_TRUE(need > got && got > MAX_PATH);
    ASSERT_TRUE(wcsncmp(fin, L"\\\\?\\", 4) == 0);
    /* It ends in the names made below the temporary directory (which `tmp`
     * may spell in its short 8.3 form). */
    const wchar_t* made = path + tl;
    size_t ml = wcslen(made), fl = wcslen(fin);
    ASSERT_TRUE(fl > ml && _wcsicmp(fin + fl - ml, made) == 0 && fin[fl - ml - 1] == L'\\');
    /* Removed bottom-up through the long form. */
    for (int i = 59; i >= 0; i--) {
        DWORD ln = aether_win_path_is_long(path) ? aether_win_long_form(path, lp, 2200) : 0;
        if (!ln) wcscpy(lp, path);
        RemoveDirectoryW(lp);
        wchar_t* cut = wcsrchr(path, L'\\');
        if (!cut || (size_t)(cut - path) < base) break;
        *cut = L'\0';
    }
    ASSERT_TRUE(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
}
#endif
