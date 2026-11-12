/* A main() for a libFuzzer harness built without libFuzzer: every file named
 * on the command line, or found directly inside a directory named there, is
 * read whole and handed to LLVMFuzzerTestOneInput once. It replays a corpus
 * or a reproducer with any C compiler, where clang has no libFuzzer (MinGW,
 * MSVC) or none is installed, so a harness and its property checks can be
 * run there on the seeds. It finds nothing new: that takes libFuzzer.
 *
 * Exits 0 when every input ran (a property failure abort()s first), 2 when
 * an input could not be read. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static int run_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "replay: cannot open %s\n", path);
        return -1;
    }
    size_t cap = 4096, len = 0;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) { fclose(f); return -1; }
    for (;;) {
        if (len == cap) {
            uint8_t* grown = (uint8_t*)realloc(buf, cap * 2);
            if (!grown) { free(buf); fclose(f); return -1; }
            buf = grown;
            cap *= 2;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        if (n == 0) break;
        len += n;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) {
        free(buf);
        fprintf(stderr, "replay: cannot read %s\n", path);
        return -1;
    }
    /* An exact-size copy, so a read past the input's end is one past an
     * allocation, where a sanitizer or a guard page can see it. */
    uint8_t* exact = (uint8_t*)malloc(len ? len : 1);
    if (!exact) { free(buf); return -1; }
    memcpy(exact, buf, len);
    free(buf);
    if (getenv("FUZZ_REPLAY_VERBOSE")) fprintf(stderr, "replay: %s\n", path);
    LLVMFuzzerTestOneInput(exact, len);
    free(exact);
    return 0;
}

int main(int argc, char** argv) {
    long ran = 0;
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            fprintf(stderr, "replay: no such file or directory: %s\n", argv[i]);
            failed = 1;
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            if (run_file(argv[i]) == 0) ran++; else failed = 1;
            continue;
        }
        DIR* d = opendir(argv[i]);
        if (!d) { fprintf(stderr, "replay: cannot list %s\n", argv[i]); failed = 1; continue; }
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            size_t n = strlen(argv[i]) + strlen(e->d_name) + 2;
            char* path = (char*)malloc(n);
            if (!path) { failed = 1; break; }
            snprintf(path, n, "%s/%s", argv[i], e->d_name);
            struct stat fs;
            if (stat(path, &fs) == 0 && !S_ISDIR(fs.st_mode)) {
                if (run_file(path) == 0) ran++; else failed = 1;
            }
            free(path);
        }
        closedir(d);
    }
    if (failed) fprintf(stderr, "replay: %ld inputs ran; some could not be read\n", ran);
    else fprintf(stderr, "replay: %ld inputs ran, every property held\n", ran);
    return failed ? 2 : 0;
}
