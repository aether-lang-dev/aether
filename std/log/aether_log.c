#include "aether_log.h"
#include <time.h>
#include <string.h>
#include <stdlib.h>

// Global logging state
static struct {
    LogConfig config;
    int initialized;
    LogStats stats;
} g_log = {
    .config = {
        .min_level = LOG_LEVEL_INFO,
        .output_file = NULL,
        .use_colors = 1,
        .show_timestamps = 1,
        .show_source_location = 0,
        .format_string = "[{time}] {level}: {message}"
    },
    .initialized = 0,
    .stats = {0}
};

// ANSI color codes
#ifdef _WIN32
    #define LOG_COLOR_DEBUG ""
    #define LOG_COLOR_INFO ""
    #define LOG_COLOR_WARN ""
    #define LOG_COLOR_ERROR ""
    #define LOG_COLOR_FATAL ""
    #define LOG_COLOR_RESET ""
#else
    #define LOG_COLOR_DEBUG "\033[0;36m"  // Cyan
    #define LOG_COLOR_INFO  "\033[0;32m"  // Green
    #define LOG_COLOR_WARN  "\033[0;33m"  // Yellow
    #define LOG_COLOR_ERROR "\033[0;31m"  // Red
    #define LOG_COLOR_FATAL "\033[1;35m"  // Bold Magenta
    #define LOG_COLOR_RESET "\033[0m"
#endif

// Get color for log level
static const char* get_level_color(LogLevel level) {
    if (!g_log.config.use_colors) return "";

    switch (level) {
        case LOG_LEVEL_DEBUG: return LOG_COLOR_DEBUG;
        case LOG_LEVEL_INFO:  return LOG_COLOR_INFO;
        case LOG_LEVEL_WARN:  return LOG_COLOR_WARN;
        case LOG_LEVEL_ERROR: return LOG_COLOR_ERROR;
        case LOG_LEVEL_FATAL: return LOG_COLOR_FATAL;
        default: return "";
    }
}

// Get string name for log level
static const char* get_level_name(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO ";
        case LOG_LEVEL_WARN:  return "WARN ";
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_FATAL: return "FATAL";
        default: return "UNKN ";
    }
}

// Get current timestamp string (thread-safe)
static void get_timestamp(char* buffer, size_t size) {
    time_t now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    /* Numeric-only conversions — no locale-sensitive specifier, so plain
     * strftime is safe here. Do NOT add %b/%a/%p: those emit the locale's
     * month/day/AM-PM names and would make log output environment-dependent. */
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &tm_buf);
}

// Initialize logging. Returns 1 on success, 0 if the requested log
// file could not be opened (logging still works, falling back to stderr).
// The Aether wrapper `log.init` turns 0 into an error string.
int log_init_raw(const char* filename, LogLevel min_level) {
    int ok = 1;
    if (g_log.initialized && g_log.config.output_file) {
        fclose(g_log.config.output_file);
    }

    if (filename) {
        g_log.config.output_file = fopen(filename, "a");
        if (!g_log.config.output_file) {
            fprintf(stderr, "[LOG] Failed to open log file: %s\n", filename);
            g_log.config.output_file = stderr;
            ok = 0;
        }
    } else {
        g_log.config.output_file = stderr;
    }

    g_log.config.min_level = min_level;
    g_log.initialized = 1;
    return ok;
}

void log_init_with_config(LogConfig* config) {
    if (g_log.initialized && g_log.config.output_file) {
        fclose(g_log.config.output_file);
    }

    memcpy(&g_log.config, config, sizeof(LogConfig));

    if (!g_log.config.output_file) {
        g_log.config.output_file = stderr;
    }

    g_log.initialized = 1;
}

void log_shutdown() {
    if (g_log.initialized && g_log.config.output_file &&
        g_log.config.output_file != stderr && g_log.config.output_file != stdout) {
        fclose(g_log.config.output_file);
    }
    g_log.initialized = 0;
}

// Core logging function
void log_write(LogLevel level, const char* fmt, ...) {
    if (!g_log.initialized) {
        log_init_raw(NULL, LOG_LEVEL_INFO);
    }

    if (level < g_log.config.min_level) {
        return;
    }

    // Update statistics
    switch (level) {
        case LOG_LEVEL_DEBUG: g_log.stats.debug_count++; break;
        case LOG_LEVEL_INFO:  g_log.stats.info_count++; break;
        case LOG_LEVEL_WARN:  g_log.stats.warn_count++; break;
        case LOG_LEVEL_ERROR: g_log.stats.error_count++; break;
        case LOG_LEVEL_FATAL: g_log.stats.fatal_count++; break;
    }

    FILE* out = g_log.config.output_file;
    const char* color = get_level_color(level);
    const char* reset = g_log.config.use_colors ? LOG_COLOR_RESET : "";

    // Print timestamp if enabled
    if (g_log.config.show_timestamps) {
        char timestamp[64];
        get_timestamp(timestamp, sizeof(timestamp));
        fprintf(out, "[%s] ", timestamp);
    }

    // Print log level
    fprintf(out, "%s%s%s: ", color, get_level_name(level), reset);

    // Print message
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);

    // If fatal, abort
    if (level == LOG_LEVEL_FATAL) {
        abort();
    }
}

// Logging with source location
void log_with_location(LogLevel level, const char* file, int line,
                       const char* func, const char* fmt, ...) {
    if (!g_log.initialized) {
        log_init_raw(NULL, LOG_LEVEL_INFO);
    }

    if (level < g_log.config.min_level) {
        return;
    }

    // Update statistics
    switch (level) {
        case LOG_LEVEL_DEBUG: g_log.stats.debug_count++; break;
        case LOG_LEVEL_INFO:  g_log.stats.info_count++; break;
        case LOG_LEVEL_WARN:  g_log.stats.warn_count++; break;
        case LOG_LEVEL_ERROR: g_log.stats.error_count++; break;
        case LOG_LEVEL_FATAL: g_log.stats.fatal_count++; break;
    }

    FILE* out = g_log.config.output_file;
    const char* color = get_level_color(level);
    const char* reset = g_log.config.use_colors ? LOG_COLOR_RESET : "";

    // Print timestamp if enabled
    if (g_log.config.show_timestamps) {
        char timestamp[64];
        get_timestamp(timestamp, sizeof(timestamp));
        fprintf(out, "[%s] ", timestamp);
    }

    // Print log level
    fprintf(out, "%s%s%s: ", color, get_level_name(level), reset);

    // Print source location
    fprintf(out, "[%s:%d in %s] ", file, line, func);

    // Print message
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);

    // If fatal, abort
    if (level == LOG_LEVEL_FATAL) {
        abort();
    }
}

// Configuration functions
void log_set_level(LogLevel level) {
    g_log.config.min_level = level;
}

void log_set_colors(int enabled) {
    g_log.config.use_colors = enabled;
}

void log_set_timestamps(int enabled) {
    g_log.config.show_timestamps = enabled;
}

void log_set_format(const char* format) {
    g_log.config.format_string = format;
}

// Statistics
LogStats* log_get_stats() {
    return &g_log.stats;
}

void log_print_stats() {
    fprintf(stderr, "\n========== Logging Statistics ==========\n");
    fprintf(stderr, "DEBUG: %zu\n", g_log.stats.debug_count);
    fprintf(stderr, "INFO:  %zu\n", g_log.stats.info_count);
    fprintf(stderr, "WARN:  %zu\n", g_log.stats.warn_count);
    fprintf(stderr, "ERROR: %zu\n", g_log.stats.error_count);
    fprintf(stderr, "FATAL: %zu\n", g_log.stats.fatal_count);
    fprintf(stderr, "========================================\n\n");
}

