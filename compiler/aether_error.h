#ifndef AETHER_ERROR_H
#define AETHER_ERROR_H

#include <stddef.h>

// Error codes for documentation lookup
typedef enum {
    AETHER_ERR_NONE = 0,
    AETHER_ERR_SYNTAX = 100,
    AETHER_ERR_TYPE_MISMATCH = 200,
    AETHER_ERR_UNDEFINED_VAR = 300,
    AETHER_ERR_UNDEFINED_FUNC = 301,
    AETHER_ERR_UNDEFINED_TYPE = 302,
    AETHER_ERR_NOT_EXPORTED = 303,
    AETHER_ERR_HIDDEN_NAME = 304,        // identifier blocked by `hide` or `seal except` in an enclosing scope
    AETHER_ERR_REDEFINITION = 400,
    AETHER_ERR_INVALID_OPERAND = 500,
    AETHER_ERR_ACTOR_ERROR = 600,
    // Warnings (W-series)
    AETHER_WARN_UNUSED_VAR = 1001,
    AETHER_WARN_UNREACHABLE = 1002,
    AETHER_WARN_CONST_OVERFLOW = 1003,   // int constant expression exceeds 32 bits
} AetherErrorCode;

// Error reporting with source context
typedef struct {
    const char* filename;
    const char* source_code;
    int line;
    int column;
    const char* message;
    const char* suggestion;
    const char* context;  // "in actor definition", "in function call", etc.
    AetherErrorCode code;
} AetherError;

// Error reporting functions
void aether_error_init(const char* filename, const char* source);
// Save/restore the active source context (filename + buffer) without
// touching the error/warning counts — used to render an imported
// module's parse errors against the module's own file and source (#646).
void aether_error_get_source(const char** out_filename, const char** out_source);
void aether_error_set_source(const char* filename, const char* source);
void aether_error_report(AetherError* error);
void aether_warning_report(AetherError* warning);

// Helper functions
void aether_error_simple(const char* message, int line, int column);
void aether_error_with_suggestion(const char* message, int line, int column, const char* suggestion);
void aether_error_in_context(const char* message, int line, int column, const char* context);
void aether_error_with_code(const char* message, int line, int column, AetherErrorCode code);
// All four fields. filename + source come from the global init state.
void aether_error_full(const char* message, int line, int column,
                       const char* suggestion, const char* context,
                       AetherErrorCode code);

// Terminal color support: disabled by NO_COLOR env var or non-tty stderr
const char* aether_color_reset(void);
const char* aether_color_red(void);
const char* aether_color_yellow(void);
const char* aether_color_blue(void);
const char* aether_color_cyan(void);
const char* aether_color_bold(void);

#define AETHER_COLOR_RESET aether_color_reset()
#define AETHER_COLOR_RED aether_color_red()
#define AETHER_COLOR_YELLOW aether_color_yellow()
#define AETHER_COLOR_BLUE aether_color_blue()
#define AETHER_COLOR_CYAN aether_color_cyan()
#define AETHER_COLOR_BOLD aether_color_bold()

// Error statistics
int aether_error_count();
int aether_warning_count();
void aether_error_reset_counts();

#endif // AETHER_ERROR_H

