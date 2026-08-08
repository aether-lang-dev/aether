#include "aether_diagnostics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
#endif

// Global state for diagnostics
static struct {
    const char* source_code;
    const char* filename;
    int errors_count;
    int warnings_count;
} g_diag = {0};

// Runtime color flag: 1 when stderr is a terminal that supports ANSI
static int g_use_colors = 0;

// ANSI color codes — enabled at runtime when the terminal supports them
#define COLOR_RED    (g_use_colors ? "\033[1;31m" : "")
#define COLOR_YELLOW (g_use_colors ? "\033[1;33m" : "")
#define COLOR_CYAN   (g_use_colors ? "\033[1;36m" : "")
#define COLOR_GREEN  (g_use_colors ? "\033[1;32m" : "")
#define COLOR_BOLD   (g_use_colors ? "\033[1m"    : "")
#define COLOR_RESET  (g_use_colors ? "\033[0m"    : "")

static void detect_color_support(void) {
#ifdef _WIN32
    // Enable ANSI escape processing on Windows 10+ (build 14931+) and Windows Terminal.
    // Falls back gracefully to no-color when the handle is redirected or on older Windows.
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return;  // not a console (piped/redirected)
    if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        g_use_colors = 1;
#else
    g_use_colors = isatty(fileno(stderr));
#endif
}

void diagnostics_init(const char* source_code, const char* filename) {
    detect_color_support();
    g_diag.source_code = source_code;
    g_diag.filename = filename;
    g_diag.errors_count = 0;
    g_diag.warnings_count = 0;
}

// Levenshtein distance - for finding similar strings
int levenshtein_distance(const char* s1, const char* s2) {
    int len1 = strlen(s1);
    int len2 = strlen(s2);
    
    // Create distance matrix
    int matrix[len1 + 1][len2 + 1];
    
    for (int i = 0; i <= len1; i++) {
        matrix[i][0] = i;
    }
    for (int j = 0; j <= len2; j++) {
        matrix[0][j] = j;
    }
    
    for (int i = 1; i <= len1; i++) {
        for (int j = 1; j <= len2; j++) {
            /* (unsigned char) casts are required, not cosmetic: plain `char`
             * is signed on every platform we ship, so a byte >= 0x80 — any
             * UTF-8 continuation byte in an identifier — arrives negative and
             * indexes outside the ctype table, which is undefined behaviour.
             * Reachable here because this runs over arbitrary identifier text
             * to produce "did you mean?" suggestions. */
            int cost = (tolower((unsigned char)s1[i-1]) ==
                        tolower((unsigned char)s2[j-1])) ? 0 : 1;
            
            int deletion = matrix[i-1][j] + 1;
            int insertion = matrix[i][j-1] + 1;
            int substitution = matrix[i-1][j-1] + cost;
            
            int min = deletion;
            if (insertion < min) min = insertion;
            if (substitution < min) min = substitution;
            
            matrix[i][j] = min;
        }
    }
    
    return matrix[len1][len2];
}

// Find similar identifier using Levenshtein distance
const char* find_similar_identifier(const char* typo, const char** valid_ids, int count) {
    if (!typo || !valid_ids || count == 0) return NULL;
    
    int min_distance = 999;
    const char* best_match = NULL;
    int typo_len = strlen(typo);
    
    for (int i = 0; i < count; i++) {
        if (!valid_ids[i]) continue;
        
        int distance = levenshtein_distance(typo, valid_ids[i]);
        
        // Only suggest if distance is small relative to string length
        // and is the best match so far
        if (distance < min_distance && distance <= typo_len / 2 + 1) {
            min_distance = distance;
            best_match = valid_ids[i];
        }
    }
    
    // Only suggest if distance is reasonable (max 3 edits)
    return (min_distance <= 3) ? best_match : NULL;
}

// Extract source context lines
static void copy_line_content(const char* start, char* dest) {
    if (!start) {
        dest[0] = '\0';
        return;
    }
    int i = 0;
    while (start[i] && start[i] != '\n' && i < 255) {
        dest[i] = start[i];
        i++;
    }
    dest[i] = '\0';
}

static void extract_context(int line, EnhancedDiagnostic* diag) {
    if (!g_diag.source_code) {
        diag->context_line[0] = '\0';
        diag->context_before_count = 0;
        diag->context_after_count = 0;
        return;
    }
    
    const char* src = g_diag.source_code;
    int current_line = 1;
    const char* line_start = src;
    
    // Store line starts in a circular buffer
    const char* line_starts[MAX_CONTEXT_LINES + 1 + MAX_CONTEXT_LINES];
    int line_numbers[MAX_CONTEXT_LINES + 1 + MAX_CONTEXT_LINES];
    int buffer_size = MAX_CONTEXT_LINES + 1 + MAX_CONTEXT_LINES;
    
    line_starts[0] = src;
    line_numbers[0] = 1;
    int stored_lines = 1;
    
    // Scan through source and store all line starts
    while (*src) {
        if (*src == '\n') {
            current_line++;
            line_start = src + 1;
            
            if (stored_lines < buffer_size) {
                line_starts[stored_lines] = line_start;
                line_numbers[stored_lines] = current_line;
                stored_lines++;
            }
            
            // Stop scanning after we've found enough lines past the target
            if (current_line > line + MAX_CONTEXT_LINES) {
                break;
            }
        }
        src++;
    }
    
    // Find target line and extract context
    diag->context_before_count = 0;
    diag->context_after_count = 0;
    diag->context_line[0] = '\0';
    
    for (int i = 0; i < stored_lines; i++) {
        if (line_numbers[i] == line) {
            // Found target line - extract context
            
            // Lines before (up to MAX_CONTEXT_LINES)
            int before_start = (i >= MAX_CONTEXT_LINES) ? i - MAX_CONTEXT_LINES : 0;
            for (int j = before_start; j < i; j++) {
                copy_line_content(line_starts[j], diag->context_before[diag->context_before_count]);
                diag->context_before_count++;
            }
            
            // Target line
            copy_line_content(line_starts[i], diag->context_line);
            
            // Lines after (up to MAX_CONTEXT_LINES)
            for (int j = i + 1; j < stored_lines && diag->context_after_count < MAX_CONTEXT_LINES; j++) {
                copy_line_content(line_starts[j], diag->context_after[diag->context_after_count]);
                diag->context_after_count++;
            }
            
            break;
        }
    }
}

// Report error with enhanced information
void report_error_enhanced(ErrorCode code, int line, int column, 
                          const char* message, const char* suggestion) {
    EnhancedDiagnostic diag = {0};
    diag.code = code;
    diag.message = message;
    diag.suggestion = suggestion;
    diag.line = line;
    diag.column = column;
    diag.filename = g_diag.filename;
    diag.context_before_count = 0;
    diag.context_after_count = 0;
    
    extract_context(line, &diag);
    
    print_diagnostic_colored(&diag);
    g_diag.errors_count++;
}

// Print diagnostic with colors and formatting
void print_diagnostic_colored(EnhancedDiagnostic* diag) {
    // Print header: error code and location
    fprintf(stderr, "%serror[E%04d]%s: %s\n",
            COLOR_RED, diag->code, COLOR_RESET, diag->message);
    
    fprintf(stderr, "  %s--> %s%s:%d:%d%s\n",
            COLOR_CYAN, COLOR_RESET,
            diag->filename ? diag->filename : "<unknown>",
            diag->line, diag->column, "");
    
    // Print context if available
    if (diag->context_line[0]) {
        fprintf(stderr, "   %s|%s\n", COLOR_CYAN, COLOR_RESET);
        
        // Lines before (up to 3)
        for (int i = 0; i < diag->context_before_count; i++) {
            int line_num = diag->line - (diag->context_before_count - i);
            fprintf(stderr, "%s%3d%s %s|%s %s\n",
                    COLOR_CYAN, line_num, COLOR_RESET,
                    COLOR_CYAN, COLOR_RESET,
                    diag->context_before[i]);
        }
        
        // Error line with highlight
        fprintf(stderr, "%s%3d%s %s|%s %s\n",
                COLOR_CYAN, diag->line, COLOR_RESET,
                COLOR_CYAN, COLOR_RESET,
                diag->context_line);
        
        // Error indicator (^ at column)
        fprintf(stderr, "   %s|%s ", COLOR_CYAN, COLOR_RESET);
        for (int i = 0; i < diag->column - 1; i++) {
            fprintf(stderr, " ");
        }
        fprintf(stderr, "%s^%s here\n", COLOR_RED, COLOR_RESET);
        
        // Lines after (up to 3)
        for (int i = 0; i < diag->context_after_count; i++) {
            int line_num = diag->line + i + 1;
            fprintf(stderr, "%s%3d%s %s|%s %s\n",
                    COLOR_CYAN, line_num, COLOR_RESET,
                    COLOR_CYAN, COLOR_RESET,
                    diag->context_after[i]);
        }
        
        fprintf(stderr, "   %s|%s\n", COLOR_CYAN, COLOR_RESET);
    }
    
    // Print suggestion if available
    if (diag->suggestion) {
        fprintf(stderr, "   %s=%s %shelp:%s Did you mean '%s%s%s'?\n",
                COLOR_CYAN, COLOR_RESET,
                COLOR_GREEN, COLOR_RESET,
                COLOR_BOLD, diag->suggestion, COLOR_RESET);
    }
    
    // Print documentation link
    fprintf(stderr, "   %s=%s %snote:%s For more info, see %s\n",
            COLOR_CYAN, COLOR_RESET,
            COLOR_CYAN, COLOR_RESET,
            get_error_docs_url(diag->code));
    
    fprintf(stderr, "\n");
}

// Get documentation URL for error code
const char* get_error_docs_url(ErrorCode code) {
    static char url[128];
    snprintf(url, sizeof(url),
             "https://github.com/aether-lang-dev/aether/wiki/E%04d", code);
    return url;
}

// Example wrapper for parser errors with suggestion
void parser_error_with_suggestion(int line, int column, const char* message, 
                                  const char* typo, const char** valid_ids, int count) {
    const char* suggestion = find_similar_identifier(typo, valid_ids, count);
    report_error_enhanced(ERR_UNDEFINED_VARIABLE, line, column, message, suggestion);
}

