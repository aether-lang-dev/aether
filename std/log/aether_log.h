#ifndef AETHER_LOG_H
#define AETHER_LOG_H

#include <stdarg.h>
#include <stdio.h>

// Log levels
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_FATAL = 4
} LogLevel;

// Log configuration
typedef struct {
    LogLevel min_level;
    FILE* output_file;
    int use_colors;
    int show_timestamps;
    int show_source_location;
    const char* format_string;  // e.g., "[{time}] {level}: {message}"
} LogConfig;

// Initialize logging system. Returns 1 on success, 0 if the requested
// log file could not be opened (logging still works via stderr).
int log_init_raw(const char* filename, LogLevel min_level);
void log_init_with_config(LogConfig* config);
void log_shutdown(void);

// Core logging functions
void log_write(LogLevel level, const char* fmt, ...);
void log_with_location(LogLevel level, const char* file, int line,
                       const char* func, const char* fmt, ...);

// Convenience macros
#define LOG_DEBUG(...)  log_write(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)   log_write(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_WARN(...)   log_write(LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_ERROR(...)  log_write(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_FATAL(...)  log_write(LOG_LEVEL_FATAL, __VA_ARGS__)

// With source location
#define LOG_DEBUG_LOC(...)  log_with_location(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_INFO_LOC(...)   log_with_location(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_WARN_LOC(...)   log_with_location(LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_ERROR_LOC(...)  log_with_location(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)

// Configuration
void log_set_level(LogLevel level);
void log_set_colors(int enabled);
void log_set_timestamps(int enabled);
void log_set_format(const char* format);

// Statistics
typedef struct {
    size_t debug_count;
    size_t info_count;
    size_t warn_count;
    size_t error_count;
    size_t fatal_count;
} LogStats;

LogStats* log_get_stats(void);
void log_print_stats(void);

#endif // AETHER_LOG_H

