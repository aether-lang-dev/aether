/* ae_repl.c — the interactive REPL (#1221 split).
 *
 * Moved verbatim out of ae.c; cmd_repl is the only entry point and is
 * declared in ae_internal.h, the eval/persist/line helpers stay static.
 */

#include "ae_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <process.h>
#  ifndef getpid
#    define getpid _getpid
#  endif
#else
#  include <unistd.h>
#endif

#define REPL_MAX_LINES 256
#define REPL_LINE_LEN  1024

/* EXE_EXT mirrors ae.c's platform block; AE_VERSION comes from the build
 * (-DAETHER_VERSION) exactly as ae.c derives it. */
#ifdef _WIN32
#  define EXE_EXT ".exe"
#else
#  define EXE_EXT ""
#endif
#ifndef AE_VERSION
#  ifdef AETHER_VERSION
#    define AE_VERSION AETHER_VERSION
#  else
#    define AE_VERSION "dev"
#  endif
#endif


// Compile and run the REPL input. Returns 1 on success, 0 on failure.
static int repl_eval(const char* ae_file, const char* c_file,
                     const char* exe_file, char** history,
                     int history_count, const char* input) {
    FILE* f = fopen(ae_file, "w");
    if (!f) return 0;
    fprintf(f, "main() {\n");
    for (int i = 0; i < history_count; i++)
        fprintf(f, "    %s\n", history[i]);
    const char* rest = input;
    const char* nl;
    while ((nl = strchr(rest, '\n')) != NULL) {
        fprintf(f, "    %.*s\n", (int)(nl - rest), rest);
        rest = nl + 1;
    }
    if (*rest) fprintf(f, "    %s\n", rest);
    fprintf(f, "}\n");
    fclose(f);

    char cmd[16384];
    build_aetherc_cmd(cmd, sizeof(cmd), ae_file, c_file);
    if (run_cmd_quiet(cmd) != 0) {
        run_cmd(cmd);
        remove(c_file);
        return 0;
    }
    build_gcc_cmd(cmd, sizeof(cmd), c_file, exe_file, false, NULL);
    if (run_cmd_quiet(cmd) != 0) {
        build_gcc_cmd(cmd, sizeof(cmd), c_file, exe_file, false, NULL);
        run_cmd(cmd);
        remove(c_file);
        remove(exe_file);
        return 0;
    }
    snprintf(cmd, sizeof(cmd), "\"%s\"", exe_file);
    /* Force any pending parent-side stdio to drain BEFORE the child
     * inherits the fd. Without this, prompts that the parent printed
     * but hadn't yet flushed sit in the userspace FILE* buffer; the
     * child writes its output directly to the kernel-side fd; then
     * when the parent eventually flushes, its delayed prompt
     * overwrites or interleaves with the child's output in the pipe
     * stream. Under parallel CI load on Linux, this race becomes
     * deterministic and the child's output appears lost.
     *
     * The fflush-before pattern is well-established stdio hygiene
     * around any fork/exec/posix_spawn that shares stdout with the
     * parent's libc-buffered stream. Same hygiene applied after the
     * child runs so subsequent parent prompts arrive in the right
     * order without a second eval. */
    fflush(stdout);
    fflush(stderr);
    run_cmd(cmd);
    fflush(stdout);
    fflush(stderr);
    remove(c_file);
    remove(exe_file);
    return 1;
}

// Persist an assignment or const into session history, replacing
// previous assignments to the same variable name.
static void repl_persist(char** history, int* history_count, const char* input) {
    char* eq = strchr(input, '=');
    int has_assign = (eq && (eq == input ||
        (eq[-1] != '=' && eq[-1] != '!' && eq[-1] != '<' && eq[-1] != '>'))
        && eq[1] != '=');
    int has_const = (strncmp(input, "const ", 6) == 0);
    if (!has_assign && !has_const) return;

    int replaced = 0;
    if (has_assign && eq) {
        int name_len = (int)(eq - input);
        while (name_len > 0 && input[name_len - 1] == ' ') name_len--;
        for (int i = 0; i < *history_count; i++) {
            char* heq = strchr(history[i], '=');
            if (heq) {
                int hlen = (int)(heq - history[i]);
                while (hlen > 0 && history[i][hlen - 1] == ' ') hlen--;
                if (hlen == name_len && strncmp(input, history[i], name_len) == 0) {
                    free(history[i]);
                    history[i] = strdup(input);
                    replaced = 1;
                    break;
                }
            }
        }
    }
    if (!replaced && *history_count < REPL_MAX_LINES)
        history[(*history_count)++] = strdup(input);
}

// Check if a single line is a complete statement (no open braces).
// Single-line statements execute immediately without waiting for blank line.
static int repl_is_complete_line(const char* line) {
    int depth = 0;
    for (const char* p = line; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
    }
    return depth == 0;
}

int cmd_repl(void) {
    /* When the REPL's stdin is a pipe (CI tests, scripted invocations),
     * glibc defaults stdout to FULLY-buffered. Prompts that the user
     * needs to see immediately get held in the userspace buffer until
     * the next fflush or program exit — and any child process the
     * REPL spawns (`repl_eval` invokes aetherc + gcc + the compiled
     * binary) writes DIRECTLY to the kernel fd, bypassing the
     * parent's buffer. The result is interleaved or lost output in
     * the pipe stream. Switching to line buffering makes every
     * newline-terminated write go straight to the fd, which is what
     * an interactive shell user gets on a TTY anyway. Same fix for
     * stderr in case error messages arrive while the parent has
     * pending stdout. No-op on a real TTY (already line-buffered). */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    setvbuf(stderr, NULL, _IOLBF, BUFSIZ);

    printf("\n");
    // Dynamic box: "   Aether X.Y.Z REPL   "
    int ver_len = (int)strlen(AE_VERSION);
    int title_len = 15 + ver_len;  // "   Aether " (10) + ver + " REPL" (5)
    int help_len  = 21;            // "   :help for commands"
    int inner = title_len + 3;     // 3 chars right padding
    if (inner < help_len + 3) inner = help_len + 3;
    printf("  ┌"); for (int i = 0; i < inner; i++) printf("─"); printf("┐\n");
    printf("  │   Aether %s REPL", AE_VERSION);
    for (int i = title_len; i < inner; i++) printf(" ");
    printf("│\n");
    printf("  │   :help for commands");
    for (int i = help_len; i < inner; i++) printf(" ");
    printf("│\n");
    printf("  └"); for (int i = 0; i < inner; i++) printf("─"); printf("┘\n");
    printf("\n");

    char* history[REPL_MAX_LINES];
    int history_count = 0;
    char input[16384] = {0};
    char line[REPL_LINE_LEN];
    int brace_depth = 0;

    char ae_file[1024], c_file[1024], exe_file[1024];
    snprintf(ae_file,  sizeof(ae_file),  "%s/_aether_repl_%d.ae",  get_temp_dir(), (int)getpid());
    snprintf(c_file,   sizeof(c_file),   "%s/_aether_repl_%d.c",   get_temp_dir(), (int)getpid());
    snprintf(exe_file, sizeof(exe_file), "%s/_aether_repl_%d" EXE_EXT, get_temp_dir(), (int)getpid());

    while (1) {
        if (brace_depth > 0)
            printf("...  ");
        else if (input[0])
            printf("  .. ");
        else
            printf("  ae> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = '\0';

        // Commands (only at top level, not mid-block)
        if (brace_depth == 0 && !input[0]) {
            if (strcmp(line, ":quit") == 0 || strcmp(line, ":q") == 0 ||
                strcmp(line, "exit") == 0  || strcmp(line, "quit") == 0) break;
            if (strcmp(line, ":help") == 0 || strcmp(line, ":h") == 0) {
                printf("\n");
                printf("  Commands:\n");
                printf("    :help  :h    Show this help\n");
                printf("    :reset :r    Clear session state\n");
                printf("    :show  :s    Show session code\n");
                printf("    :quit  :q    Exit (also: quit, exit)\n");
                printf("\n");
                printf("  Usage:\n");
                printf("    Single lines run immediately:\n");
                printf("      ae> println(\"hello\")\n");
                printf("      hello\n");
                printf("\n");
                printf("    Assignments persist across evaluations:\n");
                printf("      ae> x = 5\n");
                printf("      ae> println(x + 1)\n");
                printf("      6\n");
                printf("\n");
                printf("    Multi-line blocks auto-continue until braces close:\n");
                printf("      ae> if x > 3 {\n");
                printf("      ...   println(\"big\")\n");
                printf("      ... }\n");
                printf("      big\n");
                printf("\n");
                continue;
            }
            if (strcmp(line, ":reset") == 0 || strcmp(line, ":r") == 0) {
                for (int i = 0; i < history_count; i++) free(history[i]);
                history_count = 0;
                printf("  Session reset.\n");
                continue;
            }
            if (strcmp(line, ":show") == 0 || strcmp(line, ":s") == 0) {
                if (history_count == 0) { printf("  (empty session)\n"); continue; }
                printf("\n");
                for (int i = 0; i < history_count; i++)
                    printf("    %s\n", history[i]);
                printf("\n");
                continue;
            }
        }

        // Track brace depth
        int prev_depth = brace_depth;
        for (char* p = line; *p; p++) {
            if (*p == '{') brace_depth++;
            else if (*p == '}' && brace_depth > 0) brace_depth--;
        }
        int is_empty = (strlen(line) == 0);
        int block_closed = (prev_depth > 0 && brace_depth == 0);

        // Accumulate non-empty lines
        if (!is_empty) {
            if (input[0]) strncat(input, "\n", sizeof(input) - strlen(input) - 1);
            strncat(input, line, sizeof(input) - strlen(input) - 1);
        }

        // Decide when to execute:
        // 1. Block just closed (multi-line if/while/for)
        // 2. Empty line with pending input (explicit trigger)
        // 3. Single complete line (no open braces, no prior accumulation)
        int should_run = 0;
        if (block_closed && input[0])
            should_run = 1;
        else if (is_empty && input[0])
            should_run = 1;
        else if (!is_empty && brace_depth == 0 && prev_depth == 0 &&
                 !strchr(input, '\n') && repl_is_complete_line(input))
            should_run = 1;

        if (should_run) {
            if (repl_eval(ae_file, c_file, exe_file, history,
                          history_count, input)) {
                repl_persist(history, &history_count, input);
            }
            input[0] = '\0';
            brace_depth = 0;
        }
    }

    for (int i = 0; i < history_count; i++) free(history[i]);
    remove(ae_file);
    remove(c_file);
    remove(exe_file);
    printf("\n  Goodbye!\n\n");
    return 0;
}
