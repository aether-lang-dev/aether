/* A native stand-in for cosign and gh, as the release tools are on Windows:
 * a program of the C runtime, so it gets its arguments as the shell that
 * started it hands them over, after whatever rewriting that shell does.
 *
 * Built twice, as cosign and as gh (test_getsh_native_args.sh). Each call
 * appends one line per argument, "<tool> <arg>", to the file named by
 * NATIVE_TOOL_LOG, and answers as a real one would to what it was given:
 *   gh auth status                exits 0
 *   gh attestation verify FILE... exits 0 when FILE opens, else 1
 *   cosign verify-blob ... --bundle FILE ... BLOB   exits 0 when both open
 * A file that does not open as given is the sign of a path the shell
 * rewrote to one this program cannot see, or of one it never converted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int opens(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int main(int argc, char **argv) {
    const char *tool = strstr(argv[0], "cosign") ? "cosign" : "gh";
    const char *log = getenv("NATIVE_TOOL_LOG");
    int i, ok = 1;
    if (log) {
        FILE *f = fopen(log, "ab");
        if (f) {
            for (i = 1; i < argc; i++) fprintf(f, "%s %s\n", tool, argv[i]);
            fprintf(f, "%s --end--\n", tool);
            fclose(f);
        }
    }
    if (!strcmp(tool, "gh")) {
        if (argc > 2 && !strcmp(argv[1], "auth")) return 0;
        if (argc > 3 && !strcmp(argv[1], "attestation")) ok = opens(argv[3]);
    } else {
        for (i = 1; i < argc; i++)
            if (!strcmp(argv[i], "--bundle") && i + 1 < argc) ok = ok && opens(argv[i + 1]);
        ok = ok && opens(argv[argc - 1]);
    }
    return ok ? 0 : 1;
}
