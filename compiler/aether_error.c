/*
 * Aether Programming Language - Error Reporting
 * Copyright (c) 2025 Aether Programming Language Contributors
 * Licensed under the MIT License. See LICENSE file in the project root.
 */

#include "aether_error.h"
#include "aether_source_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <io.h>
#include <direct.h>   // _getcwd: aether_display_path
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

/* The components of `path` (already '/'-separated, absolute) into `parts`,
 * "." dropped and ".." taking the one before it. `path` is cut in place.
 * Returns how many; the drive or root is the first ("D:" or ""). */
static int display_split(char* path, char** parts, int cap) {
    int n = 0;
    char* p = path;
    while (n < cap) {
        char* slash = strchr(p, '/');
        if (slash) *slash = '\0';
        if (n > 0 && (p[0] == '\0' || strcmp(p, ".") == 0)) {
            /* an empty or "." component names the same directory */
        } else if (n > 1 && strcmp(p, "..") == 0) {
            n--;
        } else {
            parts[n++] = p;
        }
        if (!slash) break;
        p = slash + 1;
    }
    return n;
}

static int display_same(const char* a, const char* b) {
#ifdef _WIN32
    return _stricmp(a, b) == 0;   /* D: and d:, as Windows reads them */
#else
    return strcmp(a, b) == 0;
#endif
}

/* `here` relative to `base` into `buf`, their components split into `hp`
 * and `bp`; 0 when it cannot be (another drive, no room in `buf`). */
static int display_relative(char* here, char* base, char** hp, int hcap, char** bp, int bcap,
                            char* buf, size_t cap) {
    int hn = display_split(here, hp, hcap);
    int bn = display_split(base, bp, bcap);
    if (hn == 0 || bn == 0 || !display_same(hp[0], bp[0])) return 0;   /* another drive */
    int common = 0;
    while (common < hn - 1 && common < bn && display_same(hp[common], bp[common])) common++;
    size_t used = 0;
    buf[0] = '\0';
    for (int i = common; i < bn; i++) {
        int w = snprintf(buf + used, cap - used, "../");
        if (w < 0 || (size_t)w >= cap - used) return 0;
        used += (size_t)w;
    }
    for (int i = common; i < hn; i++) {
        int w = snprintf(buf + used, cap - used, "%s%s", hp[i], i + 1 < hn ? "/" : "");
        if (w < 0 || (size_t)w >= cap - used) return 0;
        used += (size_t)w;
    }
    return 1;
}

/* `path` as the user should see it (#3215): relative to the directory the
 * command was typed in, which `ae` puts in AETHER_DISPLAY_FROM when it has
 * entered a project elsewhere to compile it (a walk-up to aether.toml, or a
 * project directory named on the command line). The compiler runs from the
 * project root and named every file from there: `sub/bad.ae` for the bad.ae
 * typed in sub/, which neither a terminal nor an editor could open. An
 * absolute path, or no variable, is left as it is. Returns `path` or `buf`. */
const char* aether_display_path(const char* path, char* buf, size_t cap) {
    const char* from = getenv("AETHER_DISPLAY_FROM");
    if (!path || !path[0] || !from || !from[0] || !buf || cap == 0) return path;
    if (path[0] == '/' || path[0] == '\\' || (isalpha((unsigned char)path[0]) && path[1] == ':'))
        return path;
    char cwd[4096];   /* a longer one getcwd refuses: the path is shown as it is */
#ifdef _WIN32
    if (!_getcwd(cwd, (int)sizeof cwd)) return path;
#else
    if (!getcwd(cwd, sizeof cwd)) return path;
#endif
    /* Each as long as it is, and every component (#3574): 8192 bytes cut
     * the path and 256 slots dropped the components of a deeper one, and
     * the diagnostic named another file, or a directory above it. */
    size_t hlen = strlen(cwd) + 1 + strlen(path) + 1;
    char* here = (char*)malloc(hlen);
    char* base = (char*)malloc(strlen(from) + 1);
    if (!here || !base) {
        free(here);
        free(base);
        return path;
    }
    snprintf(here, hlen, "%s/%s", cwd, path);
    strcpy(base, from);
    for (char* q = here; *q; q++) if (*q == '\\') *q = '/';
    for (char* q = base; *q; q++) if (*q == '\\') *q = '/';
    int hcap = 1, bcap = 1;
    for (const char* q = here; *q; q++) hcap += *q == '/';
    for (const char* q = base; *q; q++) bcap += *q == '/';
    char** hp = (char**)malloc(sizeof(char*) * (size_t)hcap);
    char** bp = (char**)malloc(sizeof(char*) * (size_t)bcap);
    const char* shown = path;
    if (hp && bp && display_relative(here, base, hp, hcap, bp, bcap, buf, cap)) shown = buf;
    free(hp);
    free(bp);
    free(here);
    free(base);
    return shown;
}

// Color support: respect NO_COLOR (https://no-color.org/) and non-tty stderr
static int colors_enabled = -1;  // -1 = not yet checked

static int should_use_colors(void) {
    if (colors_enabled < 0) {
        if (getenv("NO_COLOR") != NULL) {
            colors_enabled = 0;
        } else {
            colors_enabled = isatty(fileno(stderr));
        }
    }
    return colors_enabled;
}

const char* aether_color_reset(void)  { return should_use_colors() ? "\033[0m"  : ""; }
const char* aether_color_red(void)    { return should_use_colors() ? "\033[31m" : ""; }
const char* aether_color_yellow(void) { return should_use_colors() ? "\033[33m" : ""; }
const char* aether_color_blue(void)   { return should_use_colors() ? "\033[34m" : ""; }
const char* aether_color_cyan(void)   { return should_use_colors() ? "\033[36m" : ""; }
const char* aether_color_bold(void)   { return should_use_colors() ? "\033[1m"  : ""; }

static const char* current_filename = NULL;
static const char* current_source = NULL;
static int error_count_global = 0;
static int warning_count_global = 0;
/* While set, a diagnostic is neither printed nor counted. Codegen sets it
 * around the second rendering of one function (#3029's view body): the
 * same source emitted twice would otherwise report everything twice, and
 * the other rendering reports it once. */
static int diagnostics_muted = 0;

void aether_diagnostics_set_muted(int muted) {
    diagnostics_muted = muted ? 1 : 0;
}

int aether_diagnostics_muted(void) {
    return diagnostics_muted;
}

// Get suggestion based on error code
static const char* get_common_suggestion(AetherErrorCode code) {
    switch (code) {
        case AETHER_ERR_SYNTAX:
            return "check for missing parentheses, braces, or keywords";
        case AETHER_ERR_TYPE_MISMATCH:
            return "ensure types are compatible or add explicit type conversion";
        case AETHER_ERR_ARRAY_RESULT:
            return "wrap the array in a struct and return the struct";
        case AETHER_ERR_UNDEFINED_VAR:
            return "check spelling or declare the variable before use";
        case AETHER_ERR_UNDEFINED_FUNC:
            return "check function name spelling or ensure it's defined before use";
        case AETHER_ERR_NOT_EXPORTED:
            return "add 'export' to the declaration in the module, or use an exported alternative";
        case AETHER_ERR_UNDEFINED_TYPE:
            return "check type name or define the struct/actor first";
        case AETHER_ERR_REDEFINITION:
            return "use a different name or remove the duplicate definition";
        case AETHER_ERR_INVALID_OPERAND:
            return "ensure operands have compatible types for this operation";
        case AETHER_ERR_ACTOR_ERROR:
            return "check actor definition syntax and state variables";
        case AETHER_ERR_MISSING_RETURN:
            return "return a value on every path that reaches the end of the function";
        case AETHER_ERR_DANGLING_VIEW:
            return "keep the slice's storage alive as long as the slice";
        case AETHER_WARN_UNUSED_VAR:
            return "prefix with '_' to suppress this warning";
        case AETHER_WARN_UNREACHABLE:
            return "remove unreachable code or restructure control flow";
        default:
            return NULL;
    }
}

void aether_error_init(const char* filename, const char* source) {
    current_filename = filename;
    current_source = source;
    error_count_global = 0;
    warning_count_global = 0;
}

// Read the active source context (filename + buffer) without disturbing
// it or the error/warning counts. Paired with aether_error_set_source to
// save/restore the context across a nested parse — e.g. parsing an
// imported module, whose diagnostics must name the module's own file and
// render from its own source buffer, not the importing file's (#646).
void aether_error_get_source(const char** out_filename, const char** out_source) {
    if (out_filename) *out_filename = current_filename;
    if (out_source)   *out_source   = current_source;
}

// Re-point the active source context. Unlike aether_error_init this does
// NOT reset the error/warning counts, so swapping in a module's source
// for the duration of its parse and swapping the caller's back afterward
// preserves the running totals across the whole compilation (#646).
void aether_error_set_source(const char* filename, const char* source) {
    current_filename = filename;
    current_source = source;
}


/* The snippet source for a diagnostic, and NEVER another file's.
 *
 * #1946: the fallback buffer belongs to whichever file is being compiled, so
 * using it for a diagnostic that names a different file renders line N of the
 * wrong source. A warning from an imported module printed the importing file's
 * line N underneath it, and when N was past that file's end it printed nothing
 * at all: asn1/module.ae:613 was shown as a 175-line test file's line 613.
 *
 * So the buffer is used only when the diagnostic names the file it belongs to,
 * and otherwise the one line is read from the file itself. A module's buffer is
 * freed after its parse, which is exactly the case that needs the read. The
 * caller frees a non-NULL *owned. */
static const char* snippet_source_for(const char* file, const char* explicit_src,
                                      char** owned) {
    *owned = NULL;
    if (explicit_src) return explicit_src;
    if (!file) return NULL;
    if (current_source && current_filename && strcmp(file, current_filename) == 0)
        return current_source;

    FILE* f = fopen(file, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || n > (16L * 1024 * 1024)) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *owned = buf;
    return buf;
}

// Get a specific line from source code
static const char* get_line(const char* source, int line_num, int* line_length) {
    if (!source) return NULL;

    int current_line = 1;
    /* #2999: a byte order mark starting the file is no part of its first
     * line, whose columns the lexer counts from after it. */
    const char* line_start = source + aether_source_bom_length(source);
    const char* p = line_start;
    
    while (*p && current_line < line_num) {
        if (*p == '\n') {
            current_line++;
            line_start = p + 1;
        }
        p++;
    }
    
    if (current_line != line_num) return NULL;
    
    // Find end of line
    const char* line_end = line_start;
    while (*line_end && *line_end != '\n') line_end++;
    
    *line_length = line_end - line_start;
    return line_start;
}

void aether_error_report(AetherError* error) {
    if (!error || diagnostics_muted) return;

    error_count_global++;

    /* A diagnostic built by hand often leaves filename and source_code NULL
     * even though it knows its line and column, and the location was then
     * simply not printed: the reader got "unused variable 'digest'" with no
     * file, no line and no snippet, which is unactionable in a build that
     * compiles hundreds of modules. Fall back to the active source context,
     * which is exactly the file being compiled when the diagnostic fires. */
    const char* e_file = error->filename ? error->filename : current_filename;
    char* e_owned = NULL;
    const char* e_src = snippet_source_for(e_file, error->source_code, &e_owned);
    
    // Print error header with code
    if (error->code != AETHER_ERR_NONE) {
        fprintf(stderr, "%s%serror[E%04d]%s: %s\n",
                AETHER_COLOR_RED, AETHER_COLOR_BOLD,
                error->code,
                AETHER_COLOR_RESET,
                error->message);
    } else {
        fprintf(stderr, "%s%serror%s: %s\n",
                AETHER_COLOR_RED, AETHER_COLOR_BOLD,
                AETHER_COLOR_RESET,
                error->message);
    }
    
    // Print location
    if (e_file) {
        char shown[4096];
        fprintf(stderr, "  %s-->%s %s:%d:%d\n",
                AETHER_COLOR_BLUE,
                AETHER_COLOR_RESET,
                aether_display_path(e_file, shown, sizeof shown),
                error->line,
                error->column);
    }
    
    // Print source context
    if (e_src && error->line > 0) {
        int line_length;
        const char* line = get_line(e_src, error->line, &line_length);
        
        if (line) {
            // Line number width
            int line_num_width = snprintf(NULL, 0, "%d", error->line);
            
            /* A long line is windowed around the caret rather than echoed
             * whole. Generated or minified source reaches thousands of columns,
             * and printing all of it (plus one space per column on the caret
             * line) buries the message it is supposed to illustrate. */
            const int MAX_SNIPPET = 160;
            int col0 = error->column > 0 ? error->column - 1 : 0;
            int win_start = 0;
            int win_len = line_length;
            int elided_left = 0, elided_right = 0;
            if (line_length > MAX_SNIPPET) {
                win_start = col0 - MAX_SNIPPET / 2;
                if (win_start > line_length - MAX_SNIPPET)
                    win_start = line_length - MAX_SNIPPET;
                if (win_start < 0) win_start = 0;
                win_len = MAX_SNIPPET;
                if (win_start + win_len > line_length)
                    win_len = line_length - win_start;
                elided_left  = win_start > 0;
                elided_right = win_start + win_len < line_length;
            }

            // Print line with line number
            fprintf(stderr, "%s%*d |%s ", 
                    AETHER_COLOR_BLUE,
                    line_num_width, error->line,
                    AETHER_COLOR_RESET);
            if (elided_left) fprintf(stderr, "...");
            fwrite(line + win_start, 1, (size_t)win_len, stderr);
            if (elided_right) fprintf(stderr, "...");
            fprintf(stderr, "\n");
            
            // Print caret (^) pointing to error column
            fprintf(stderr, "%*s |%s ", line_num_width, "", AETHER_COLOR_BLUE);
            int caret_col = col0 - win_start + (elided_left ? 3 : 0);
            if (caret_col < 0) caret_col = 0;
            for (int i = 0; i < caret_col; i++) {
                fprintf(stderr, " ");
            }
            fprintf(stderr, "%s%s^%s", AETHER_COLOR_RED, AETHER_COLOR_BOLD, AETHER_COLOR_RESET);
            
            // Print suggestion on same line if available
            const char* suggestion = error->suggestion;
            if (!suggestion && error->code != AETHER_ERR_NONE) {
                suggestion = get_common_suggestion(error->code);
            }
            
            if (suggestion) {
                fprintf(stderr, " %shelp:%s %s",
                        AETHER_COLOR_CYAN,
                        AETHER_COLOR_RESET,
                        suggestion);
            }
            fprintf(stderr, "\n");
        }
    }
    
    // Print context if available
    if (error->context) {
        fprintf(stderr, "   %s= note:%s %s\n",
                AETHER_COLOR_CYAN,
                AETHER_COLOR_RESET,
                error->context);
    }
    
    fprintf(stderr, "\n");
    free(e_owned);
}

void aether_warning_report(AetherError* warning) {
    if (!warning || diagnostics_muted) return;
    
    warning_count_global++;

    /* Same fallback as aether_error_report: a warning that knows its line but
     * not its file still names the file being compiled. */
    const char* w_file = warning->filename ? warning->filename : current_filename;
    char* w_owned = NULL;
    const char* w_src = snippet_source_for(w_file, warning->source_code, &w_owned);
    
    // Print warning header (yellow instead of red)
    if (warning->code >= 1000) {
        fprintf(stderr, "%s%swarning[W%04d]%s: %s\n",
                AETHER_COLOR_YELLOW, AETHER_COLOR_BOLD,
                warning->code,
                AETHER_COLOR_RESET,
                warning->message);
    } else {
        fprintf(stderr, "%s%swarning%s: %s\n",
                AETHER_COLOR_YELLOW, AETHER_COLOR_BOLD,
                AETHER_COLOR_RESET,
                warning->message);
    }
    
    // Print location
    if (w_file) {
        char shown[4096];
        fprintf(stderr, "  %s-->%s %s:%d:%d\n",
                AETHER_COLOR_BLUE,
                AETHER_COLOR_RESET,
                aether_display_path(w_file, shown, sizeof shown),
                warning->line,
                warning->column);
    }
    
    // Print source context (same as error)
    if (w_src && warning->line > 0) {
        int line_length;
        const char* line = get_line(w_src, warning->line, &line_length);
        
        if (line) {
            int line_num_width = snprintf(NULL, 0, "%d", warning->line);
            
            fprintf(stderr, "%s%*d |%s ",
                    AETHER_COLOR_BLUE,
                    line_num_width, warning->line,
                    AETHER_COLOR_RESET);
            fwrite(line, 1, line_length, stderr);
            fprintf(stderr, "\n");
            
            fprintf(stderr, "%*s |%s ", line_num_width, "", AETHER_COLOR_BLUE);
            for (int i = 0; i < warning->column - 1; i++) {
                fprintf(stderr, " ");
            }
            fprintf(stderr, "%s^%s", AETHER_COLOR_YELLOW, AETHER_COLOR_RESET);
            
            if (warning->suggestion) {
                fprintf(stderr, " %shelp:%s %s",
                        AETHER_COLOR_CYAN,
                        AETHER_COLOR_RESET,
                        warning->suggestion);
            }
            fprintf(stderr, "\n");
        }
    }
    
    fprintf(stderr, "\n");
    free(w_owned);
}

// Helper functions
void aether_error_simple(const char* message, int line, int column) {
    AetherError error = {
        .filename = current_filename,
        .source_code = current_source,
        .line = line,
        .column = column,
        .message = message,
        .suggestion = NULL,
        .context = NULL,
        .code = AETHER_ERR_NONE
    };
    aether_error_report(&error);
}

void aether_error_with_suggestion(const char* message, int line, int column, const char* suggestion) {
    AetherError error = {
        .filename = current_filename,
        .source_code = current_source,
        .line = line,
        .column = column,
        .message = message,
        .suggestion = suggestion,
        .context = NULL,
        .code = AETHER_ERR_NONE
    };
    aether_error_report(&error);
}

void aether_error_in_context(const char* message, int line, int column, const char* context) {
    AetherError error = {
        .filename = current_filename,
        .source_code = current_source,
        .line = line,
        .column = column,
        .message = message,
        .suggestion = NULL,
        .context = context,
        .code = AETHER_ERR_NONE
    };
    aether_error_report(&error);
}

// Richest variant: all four fields (message, suggestion, context, code).
// filename and source_code are pulled from the global init state so the
// caller doesn't have to.
void aether_error_full(const char* message, int line, int column,
                       const char* suggestion, const char* context,
                       AetherErrorCode code) {
    AetherError error = {
        .filename = current_filename,
        .source_code = current_source,
        .line = line,
        .column = column,
        .message = message,
        .suggestion = suggestion,
        .context = context,
        .code = code
    };
    aether_error_report(&error);
}

void aether_error_with_code(const char* message, int line, int column, AetherErrorCode code) {
    AetherError error = {
        .filename = current_filename,
        .source_code = current_source,
        .line = line,
        .column = column,
        .message = message,
        .suggestion = NULL,
        .context = NULL,
        .code = code
    };
    aether_error_report(&error);
}

// Error statistics
int aether_error_count(void) {
    return error_count_global;
}

int aether_warning_count(void) {
    return warning_count_global;
}

void aether_error_reset_counts(void) {
    error_count_global = 0;
    warning_count_global = 0;
}

