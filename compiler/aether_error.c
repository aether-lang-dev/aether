/*
 * Aether Programming Language - Error Reporting
 * Copyright (c) 2025 Aether Programming Language Contributors
 * Licensed under the MIT License. See LICENSE file in the project root.
 */

#include "aether_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

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

// Get suggestion based on error code
static const char* get_common_suggestion(AetherErrorCode code) {
    switch (code) {
        case AETHER_ERR_SYNTAX:
            return "check for missing parentheses, braces, or keywords";
        case AETHER_ERR_TYPE_MISMATCH:
            return "ensure types are compatible or add explicit type conversion";
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

// Get a specific line from source code
static const char* get_line(const char* source, int line_num, int* line_length) {
    if (!source) return NULL;
    
    int current_line = 1;
    const char* line_start = source;
    const char* p = source;
    
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
    if (!error) return;
    
    error_count_global++;

    /* A diagnostic built by hand often leaves filename and source_code NULL
     * even though it knows its line and column, and the location was then
     * simply not printed: the reader got "unused variable 'digest'" with no
     * file, no line and no snippet, which is unactionable in a build that
     * compiles hundreds of modules. Fall back to the active source context,
     * which is exactly the file being compiled when the diagnostic fires. */
    const char* e_file = error->filename ? error->filename : current_filename;
    const char* e_src  = error->source_code ? error->source_code : current_source;
    
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
        fprintf(stderr, "  %s-->%s %s:%d:%d\n",
                AETHER_COLOR_BLUE,
                AETHER_COLOR_RESET,
                e_file,
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
            
            // Print line with line number
            fprintf(stderr, "%s%*d |%s ", 
                    AETHER_COLOR_BLUE,
                    line_num_width, error->line,
                    AETHER_COLOR_RESET);
            fwrite(line, 1, line_length, stderr);
            fprintf(stderr, "\n");
            
            // Print caret (^) pointing to error column
            fprintf(stderr, "%*s |%s ", line_num_width, "", AETHER_COLOR_BLUE);
            for (int i = 0; i < error->column - 1; i++) {
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
}

void aether_warning_report(AetherError* warning) {
    if (!warning) return;
    
    warning_count_global++;

    /* Same fallback as aether_error_report: a warning that knows its line but
     * not its file still names the file being compiled. */
    const char* w_file = warning->filename ? warning->filename : current_filename;
    const char* w_src  = warning->source_code ? warning->source_code : current_source;
    
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
        fprintf(stderr, "  %s-->%s %s:%d:%d\n",
                AETHER_COLOR_BLUE,
                AETHER_COLOR_RESET,
                w_file,
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
int aether_error_count() {
    return error_count_global;
}

int aether_warning_count() {
    return warning_count_global;
}

void aether_error_reset_counts() {
    error_count_global = 0;
    warning_count_global = 0;
}

