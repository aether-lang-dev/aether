#ifndef AETHER_ERROR_H
#define AETHER_ERROR_H

#include <stddef.h>

// Error codes for documentation lookup
typedef enum {
    AETHER_ERR_NONE = 0,
    AETHER_ERR_SYNTAX = 100,
    AETHER_ERR_TYPE_MISMATCH = 200,
    AETHER_ERR_ARRAY_RESULT = 201,       // a function's result is a fixed-size array, alone or in a tuple (#2886)
    AETHER_ERR_PTR_INTO_STRING = 202,    // a bare `ptr` re-binds a `string` local (W1005 in 0.830/0.831)
    AETHER_ERR_UNDEFINED_VAR = 300,
    AETHER_ERR_UNDEFINED_FUNC = 301,
    AETHER_ERR_UNDEFINED_TYPE = 302,
    AETHER_ERR_NOT_EXPORTED = 303,
    AETHER_ERR_HIDDEN_NAME = 304,        // identifier blocked by `hide` or `seal except` in an enclosing scope
    AETHER_ERR_EXPORT_SHORT_NAME = 305,  // a module exports two names that answer to one short name (#2976)
    AETHER_ERR_REDEFINITION = 400,
    AETHER_ERR_INVALID_OPERAND = 500,
    AETHER_ERR_ACTOR_ERROR = 600,
    AETHER_ERR_MISSING_RETURN = 700,     // control can reach the end of a function whose result is not void (#2684)
    AETHER_ERR_SELECT_PLATFORM = 701,    // a select() with no value for a platform and no other: (#3002)
    // `quote` (docs/quote.md)
    AETHER_ERR_QUOTE_SUBSET = 800,       // a construct a quote's tree has no node for (the v1 subset)
    AETHER_ERR_QUOTE_PARAM = 801,        // a quote parameter without a type
    AETHER_ERR_QUOTE_CALLEE = 802,       // a quote where no quote.Tree is taken, or a closure/block where one is
    AETHER_ERR_QUOTE_CAPTURE = 803,      // writing a captured variable, or taking its address, in a quote
    AETHER_ERR_QUOTE_RECURSION = 804,    // a @quotable function reached again through @quotable calls
    AETHER_ERR_QUOTE_HELPER_READ = 805,  // a @quotable function reading a variable outside it
    AETHER_ERR_DANGLING_VIEW = 900,      // a slice of a function's own storage outlives the function (#2785)
    AETHER_ERR_FREES_BORROWED = 901,     // a function frees a `string` parameter it only borrows (#2924)
    AETHER_ERR_KEEPS_BORROWED = 902,     // a function keeps a `ptr` parameter not declared `@keep` (#2926)
    AETHER_ERR_USE_AFTER_FREE = 903,     // a value used or freed after it was freed, or a view after its storage went (#1300)
    AETHER_ERR_FREES_MODE = 904,         // a `@frees` or `@moves` parameter its function does not honour (#1300)
    AETHER_ERR_OWNED = 905,              // an owned `ptr` given a second owner, freed, or kept where it cannot be (#3142)
    AETHER_ERR_USE_AFTER_MOVE = 906,     // an owned slice used after it was moved, freed or appended to
    // Warnings (W-series)
    AETHER_WARN_UNUSED_VAR = 1001,
    AETHER_WARN_UNREACHABLE = 1002,
    AETHER_WARN_CONST_OVERFLOW = 1003,   // int constant expression exceeds 32 bits
    AETHER_WARN_UNREACHABLE_ARM = 1004,  // match arm shadowed by an earlier arm
    AETHER_WARN_DOUBLED_MODULE_NAME = 1006,  // `ns.ns_x(...)` where `ns.x` is the same declaration (#2976)
    AETHER_WARN_FREE_NOT_OWNED = 1007,   // `free` of a slice its function does not own gives back nothing (#3568)
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
// While muted, errors, warnings and codegen notes are neither printed nor
// counted. Codegen mutes the second rendering of one function (#3029).
void aether_diagnostics_set_muted(int muted);
int aether_diagnostics_muted(void);

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
/* `path` as the user should see it: relative to the directory the command
 * was typed in, when `ae` compiles from a project root it entered
 * (AETHER_DISPLAY_FROM). Returns `path` or `buf`. */
const char* aether_display_path(const char* path, char* buf, size_t cap);

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
int aether_error_count(void);
int aether_warning_count(void);
void aether_error_reset_counts(void);

#endif // AETHER_ERROR_H

