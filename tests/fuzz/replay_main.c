/* A main() for a libFuzzer harness built without libFuzzer: every file named
 * on the command line, or found directly inside a directory named there, is
 * read whole and handed to LLVMFuzzerTestOneInput once. It replays a corpus
 * or a reproducer with any C compiler, where clang has no libFuzzer (MinGW,
 * MSVC) or none is installed, so a harness and its property checks can be
 * run there on the seeds. It finds nothing new: that takes libFuzzer.
 *
 * -dict=FILE reads a dictionary as libFuzzer does (ParseDictionaryFile and
 * ParseOneDictionaryEntry, FuzzerUtil.cpp): one entry a line, name="value"
 * or "value", blank lines and # comments skipped, every byte inside the
 * quotes printable or a space, and the only escapes \\, \" and \xNN. A line
 * it cannot read fails the run, naming the line, as libFuzzer refuses to
 * start on it; each entry is then run as an input too. -check_dict=FILE
 * reads one without running its entries (another target's dictionary). A
 * dictionary that only libFuzzer read failed on CI alone (#3543: fuzz_json
 * never started).
 *
 * Exits 0 when every input ran (a property failure abort()s first), 1 when a
 * dictionary does not parse, 2 when an input could not be read. */
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

/* The whole of `path` in a malloc'd buffer of exactly *len bytes (at least
 * one allocated), or NULL. */
static uint8_t* read_file(const char* path, size_t* len_out) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "replay: cannot open %s\n", path);
        return NULL;
    }
    size_t cap = 4096, len = 0;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    for (;;) {
        if (len == cap) {
            uint8_t* grown = (uint8_t*)realloc(buf, cap * 2);
            if (!grown) { free(buf); fclose(f); return NULL; }
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
        return NULL;
    }
    /* An exact-size copy, so a read past the input's end is one past an
     * allocation, where a sanitizer or a guard page can see it. */
    uint8_t* exact = (uint8_t*)malloc(len ? len : 1);
    if (exact) memcpy(exact, buf, len);
    free(buf);
    *len_out = len;
    return exact;
}

static void run_bytes(const uint8_t* data, size_t len) {
    uint8_t* exact = (uint8_t*)malloc(len ? len : 1);
    if (!exact) return;
    memcpy(exact, data, len);
    LLVMFuzzerTestOneInput(exact, len);
    free(exact);
}

static int run_file(const char* path) {
    size_t len = 0;
    uint8_t* data = read_file(path, &len);
    if (!data) return -1;
    if (getenv("FUZZ_REPLAY_VERBOSE")) fprintf(stderr, "replay: %s\n", path);
    LLVMFuzzerTestOneInput(data, len);
    free(data);
    return 0;
}

/* One dictionary line [s, s + n), as libFuzzer's ParseOneDictionaryEntry
 * reads it: the value into `out` (at least n bytes), its length in *out_len.
 * 1, or 0 for a line libFuzzer refuses. */
static int dict_entry(const char* s, size_t n, uint8_t* out, size_t* out_len) {
    *out_len = 0;
    if (n == 0) return 0;
    size_t l = 0, r = n - 1;
    while (l < r && isspace((unsigned char)s[l])) l++;
    while (r > l && isspace((unsigned char)s[r])) r--;
    if (r - l < 2) return 0;
    if (s[r] != '"') return 0;
    r--;
    while (l < r && s[l] != '"') l++;
    if (l >= r) return 0;
    l++;
    for (size_t p = l; p <= r; p++) {
        unsigned char v = (unsigned char)s[p];
        if (!isprint(v) && !isspace(v)) return 0;
        if (v == '\\') {
            if (p + 1 <= r && (s[p + 1] == '\\' || s[p + 1] == '"')) {
                out[(*out_len)++] = (uint8_t)s[p + 1];
                p++;
                continue;
            }
            if (p + 3 <= r && s[p + 1] == 'x' &&
                isxdigit((unsigned char)s[p + 2]) && isxdigit((unsigned char)s[p + 3])) {
                char hex[3] = { s[p + 2], s[p + 3], 0 };
                out[(*out_len)++] = (uint8_t)strtol(hex, NULL, 16);
                p += 3;
                continue;
            }
            return 0;   /* any other escape */
        }
        out[(*out_len)++] = v;
    }
    return 1;
}

/* Every entry of the dictionary at `path`, each run as an input when `run`.
 * The number of entries, or -1 with the line libFuzzer would stop at named. */
static long run_dict(const char* path, int run) {
    size_t len = 0;
    uint8_t* text = read_file(path, &len);
    if (!text) return -1;
    if (len == 0) {
        fprintf(stderr, "replay: ParseDictionaryFile: %s is empty\n", path);
        free(text);
        return -1;
    }
    uint8_t* value = (uint8_t*)malloc(len);
    long entries = 0;
    int line_no = 0;
    size_t at = 0;
    while (value && at < len) {
        const char* line = (const char*)text + at;
        const char* nl = (const char*)memchr(line, '\n', len - at);
        size_t n = nl ? (size_t)(nl - line) : len - at;
        at += n + 1;
        line_no++;
        size_t p = 0;
        while (p < n && isspace((unsigned char)line[p])) p++;
        if (p == n || line[p] == '#') continue;
        size_t vl = 0;
        if (!dict_entry(line, n, value, &vl)) {
            fprintf(stderr, "replay: ParseDictionaryFile: error in line %d of %s\n\t\t%.*s\n",
                    line_no, path, (int)n, line);
            free(value);
            free(text);
            return -1;
        }
        if (run) run_bytes(value, vl);
        entries++;
    }
    free(value);
    free(text);
    return entries;
}

int main(int argc, char** argv) {
    long ran = 0;
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        int check_only = strncmp(argv[i], "-check_dict=", 12) == 0;
        if (check_only || strncmp(argv[i], "-dict=", 6) == 0) {
            const char* path = argv[i] + (check_only ? 12 : 6);
            long entries = run_dict(path, !check_only);
            if (entries < 0) return 1;
            fprintf(stderr, "replay: %ld dictionary entries in %s%s\n", entries, path,
                    check_only ? " (read, not run)" : "");
            if (!check_only) ran += entries;
            continue;
        }
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
