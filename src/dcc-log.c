#define _GNU_SOURCE
#include "dcc-log.h"
#include <dirent.h>
#include <libgen.h>

/* Global state */
FILE* g_log_file = NULL;
LogLevel g_min_log_level = LOG_LEVEL_INFO;
char g_app_name[32] = "";
char g_log_dir[256] = "";

const char* log_level_to_string(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        default:              return "UNKNOWN";
    }
}

static int ensure_log_dir(void) {
    if (strlen(g_log_dir) == 0) {
        return -1;
    }
    
    struct stat st;
    if (stat(g_log_dir, &st) == 0) {
        /* Directory exists */
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    
    /* Create parent directories first using mkdir -p approach */
    char temp_path[256];
    strncpy(temp_path, g_log_dir, sizeof(temp_path) - 1);
    temp_path[sizeof(temp_path) - 1] = '\0';
    
    /* Find each slash and try to create directory up to that point */
    for (char *p = temp_path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(temp_path, 0755);
            *p = '/';
        }
    }
    
    /* Create the final directory */
    if (mkdir(g_log_dir, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    
    /* Verify it exists now */
    return stat(g_log_dir, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static void rotate_logs(void) {
    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", g_log_dir, g_app_name);
    
    /* Remove oldest rotated file if it exists */
    char old_path[512];
    snprintf(old_path, sizeof(old_path), "%s/%s.log.%d", g_log_dir, g_app_name, LOG_MAX_ROTATED_FILES);
    unlink(old_path);
    
    /* Rotate existing files: .log.4 -> .log.5, .log.3 -> .log.4, etc. */
    for (int i = LOG_MAX_ROTATED_FILES - 1; i >= 1; i--) {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s/%s.log.%d", g_log_dir, g_app_name, i);
        snprintf(dst, sizeof(dst), "%s/%s.log.%d", g_log_dir, g_app_name, i + 1);
        
        struct stat st;
        if (stat(src, &st) == 0) {
            rename(src, dst);
        }
    }
    
    /* Move current log to .log.1 */
    char src_path[512], dst_path[512];
    snprintf(src_path, sizeof(src_path), "%s/%s.log", g_log_dir, g_app_name);
    snprintf(dst_path, sizeof(dst_path), "%s/%s.log.1", g_log_dir, g_app_name);
    
    if (rename(src_path, dst_path) != 0 && errno != ENOENT) {
        /* Ignore if file doesn't exist */
    }
}

static void open_log_file(void) {
    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", g_log_dir, g_app_name);
    
    /* Open in append mode */
    g_log_file = fopen(log_path, "a");
    if (!g_log_file) {
        fprintf(stderr, "[LOG] FAILED to open log file '%s': %s (errno=%d)\n",
                log_path, strerror(errno), errno);
        return;
    }
    
    /* Set buffering for performance */
    setvbuf(g_log_file, NULL, _IOLBF, LOG_BUFFER_SIZE);  /* Line buffered */
    fprintf(stderr, "[LOG] Log file opened: %s\n", log_path);
}

/* Fallback: write to stderr if log file is unavailable */
static void log_write_stderr(const char* buffer) {
    fprintf(stderr, "%s", buffer);
    fflush(stderr);
}

/* Try to reopen the log file if it's closed */
static void try_reopen_log_file(void) {
    if (!g_log_file && strlen(g_log_dir) > 0) {
        open_log_file();
    }
}

static void check_and_rotate(void) {
    if (!g_log_file) return;
    
    /* Get current file size */
    long pos = ftell(g_log_file);
    if (pos >= LOG_MAX_FILE_SIZE) {
        fclose(g_log_file);
        g_log_file = NULL;
        
        rotate_logs();
        open_log_file();
    }
}

void log_write(LogLevel level, const char* format, ...) {
    va_list args;
    
    /* Format timestamp */
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    /* Get milliseconds */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int ms = ts.tv_nsec / 1000000;
    
    /* Format the log line */
    char buffer[LOG_BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "[%s.%03d] [%-5s] [%s] ", 
             timestamp, ms, log_level_to_string(level), g_app_name);
    
    /* Add the message */
    va_start(args, format);
    vsnprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), format, args);
    va_end(args);
    
    /* Append newline if not present */
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len-1] != '\n') {
        strncat(buffer, "\n", sizeof(buffer) - len - 1);
    }
    
    /* Write to log file with stderr fallback */
    if (g_log_file) {
        check_and_rotate();
        
        if (!g_log_file) {
            try_reopen_log_file();
        }
        
        if (g_log_file) {
            fputs(buffer, g_log_file);
        } else {
            log_write_stderr(buffer);
        }
    } else {
        /* No log file at all - write to stderr */
        log_write_stderr(buffer);
    }

}

void log_cleanup(void) {
    if (g_log_file) {
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

int log_should_output(LogLevel level) {
    return level <= g_min_log_level;
}

void log_set_level(LogLevel level) {
    LogLevel old_level = g_min_log_level;
    g_min_log_level = level;
    char new_ts[32];
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(new_ts, sizeof(new_ts), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(g_log_file ? g_log_file : stderr, "[%s] [INFO ] [LOG] Log level changed: %s -> %s\n",
            new_ts, log_level_to_string(old_level), log_level_to_string(level));
}

LogLevel log_get_level(void) {
    return g_min_log_level;
}

int log_init(const char* app_name, LogLevel min_level, const char* custom_path) {
    /* Store app name */
    strncpy(g_app_name, app_name, sizeof(g_app_name) - 1);
    g_app_name[sizeof(g_app_name) - 1] = '\0';
    
    /* Set minimum log level */
    g_min_log_level = min_level;
    
    /* Determine log directory */
    if (custom_path && strlen(custom_path) > 0) {
        /* Use custom path (e.g., /var/log for daemon) */
        strncpy(g_log_dir, custom_path, sizeof(g_log_dir) - 1);
        g_log_dir[sizeof(g_log_dir) - 1] = '\0';
    } else if (!strcmp(app_name, "DAEMON") || !strcmp(app_name, "FAN_DAEMON")) {
        /* Daemon uses /var/log by default */
        strncpy(g_log_dir, "/var/log/dudes-control-center", sizeof(g_log_dir) - 1);
        g_log_dir[sizeof(g_log_dir) - 1] = '\0';
    } else {
        /* GUI and others use user directory */
        const char* home = getenv("HOME");
        if (!home) {
            return -1;
        }
        snprintf(g_log_dir, sizeof(g_log_dir), "%s/.cache/dudes-control-center/logs", home);
    }
    
    /* Ensure log directory exists */
    if (ensure_log_dir() != 0) {
        fprintf(stderr, "[LOG] Failed to create log directory: %s\n", g_log_dir);
        return -1;
    }
    
    /* Open log file */
    open_log_file();
    
    LOG_INFO("Logging initialized for %s (level=%d)", g_app_name, min_level);
    
    return 0;
}


