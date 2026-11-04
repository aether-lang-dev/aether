/* holder <path> <marker> <ms>: what a virus scanner does to a file it has
 * just seen written, on Windows: open <path> for reading without sharing it
 * for writing, say so by creating <marker>, keep it <ms> milliseconds, and
 * let it go. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    HANDLE h = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "holder: cannot open %s (Windows error %lu)\n", argv[1],
                (unsigned long)GetLastError());
        return 1;
    }
    FILE* m = fopen(argv[2], "w");
    if (m) fclose(m);
    Sleep((DWORD)atoi(argv[3]));
    CloseHandle(h);
    return 0;
}
