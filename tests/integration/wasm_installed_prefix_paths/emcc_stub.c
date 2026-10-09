/* emcc_stub.c: stands in for Emscripten's emcc in
 * test_wasm_installed_prefix_paths.sh.
 *
 * It answers --version (ae's pre-flight), then checks that every .c file it
 * was handed is there, which is exactly the property the test is about, and
 * exits 1 because it compiled nothing.
 *
 * A compiled program rather than a #! script so that it runs wherever ae
 * does: on Windows ae starts emcc through _spawnvp, which runs an .exe or a
 * batch file but not a shell script.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        puts("emcc (stub) 0.0");
        return 0;
    }
    int total = 0, missing = 0;
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (n < 2 || strcmp(argv[i] + n - 2, ".c") != 0) continue;
        total++;
        struct stat st;
        if (stat(argv[i], &st) != 0 || !S_ISREG(st.st_mode)) {
            missing++;
            printf("MISSING: %s\n", argv[i]);
        }
    }
    printf("SOURCES: %d MISSING: %d\n", total, missing);
    return 1;
}
