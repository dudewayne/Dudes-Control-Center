#ifndef DUDES_CONTROL_CENTER_LOG_H
#define DUDES_CONTROL_CENTER_LOG_H

#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* Log levels */
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3
} LogLevel;

/* Configuration */
#define LOG_MAX_FILE_SIZE     (1024 * 1024)  /* 1 MB per file */
#define LOG_MAX_ROTATED_FILES 5               /* Keep 5 rotated logs */
#define LOG_BUFFER_SIZE       4096            /* Buffer for formatted log line */

/* Global state */
extern FILE* g_log_file;
extern LogLevel g_min_log_level;
extern char g_app_name[32];
extern char g_log_dir[256];

/**
 * Initialize logging system
 * @param app_name Name of the application (e.g., "GUI", "FAN_DAEMON", "CPU_DAEMON")
 * @param min_level Minimum log level to output (default: LOG_LEVEL_INFO)
 * @param custom_path Optional custom log directory path. If NULL:
 *                    - DAEMON uses /var/log/dudes-control-center
 *                    - Others use ~/.local/share/dudes-control-center/logs
 * @return 0 on success, -1 on failure
 */
int log_init(const char* app_name, LogLevel min_level, const char* custom_path);

/**
 * Cleanup logging system
 */
void log_cleanup(void);

/**
 * Write a log message
 * @param level Log level
 * @param format Printf-style format string
 * @param ... Arguments for format string
 */
void log_write(LogLevel level, const char* format, ...);

/**
 * Check if a log level should be output
 */
int log_should_output(LogLevel level);

/**
 * Set log level at runtime
 * @param level New minimum log level
 */
void log_set_level(LogLevel level);

/**
 * Get current log level
 * @return Current minimum log level
 */
LogLevel log_get_level(void);

/**
 * Convert log level to string
 * @param level Log level enum value
 * @return String representation ("ERROR", "WARN", "INFO", "DEBUG")
 */
const char* log_level_to_string(LogLevel level);

/* Convenience macros */
#define LOG_ERROR(fmt, ...)  do { if (log_should_output(LOG_LEVEL_ERROR))  log_write(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__); } while(0)
#define LOG_WARN(fmt, ...)   do { if (log_should_output(LOG_LEVEL_WARN))   log_write(LOG_LEVEL_WARN, fmt, ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...)   do { if (log_should_output(LOG_LEVEL_INFO))   log_write(LOG_LEVEL_INFO, fmt, ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...)  do { if (log_should_output(LOG_LEVEL_DEBUG))  log_write(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__); } while(0)

#endif /* DUDES_CONTROL_CENTER_LOG_H */
