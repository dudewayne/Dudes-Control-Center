/*
 * dudes-control-center-fan-daemon.c
 * Background daemon for controlling notebook fans
 * Runs as a systemd service and provides IPC via Unix socket
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>
#include <stdbool.h>
#include "dudes-control-center-common.h"
#include "dudes-control-center-log.h"
#include <cjson/cJSON.h>

#define PROGRAM_NAME "dudes-control-center-fan-daemon"
#define SOCKET_PATH "/run/dudes-control-center-fan.sock"
#define PROFILE_PATH "/home/dude/.config/dudes-control-center/profiles.json"
#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024
#define MAX_CMD_LEN 256

/* Helper function to get timestamp string */
static void get_timestamp(char *buf, size_t buf_size) {
    struct timeval tv;
    struct tm *tm_info;
    char time_str[32];
    
    #pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    gettimeofday(&tv, NULL);
    tm_info = localtime(&tv.tv_sec);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
    snprintf(buf, buf_size, "[%s.%03ld] ", time_str, (long)(tv.tv_usec / 1000));
#pragma GCC diagnostic pop
}

/* Temperature curve point */
typedef struct {
    int temperature;  /* °C */
    int fan_percent;  /* 0-100% */
} TempCurvePoint;

/* Profile structure */
typedef struct {
    char name[64];
    TempCurvePoint points[50];
    int num_points;
    int is_active;
} FanProfile;

/* Global state */
static volatile sig_atomic_t g_running = 1;
static int debug_mode = 0;
static FanProfile profiles[10];
static int num_profiles = 0;
static int active_profile_idx = -1;
static int auto_mode_enabled = 0;
static int manual_duty = 20;
static int current_fan_duty = 20; /* Track actual fan duty for STATUS reporting */

/* Watchdog timer for fail-safe */
static time_t last_loop_time = 0; /* Will be set on first iteration */
#define WATCHDOG_TIMEOUT_SEC 10  /* Trigger fail-safe if main loop stops for 10 seconds */

/* Socket file descriptors */
static int server_fd = -1;
static int client_fds[MAX_CLIENTS];
static int num_clients = 0;

/* Fail-safe: Set fans to 100% for safety */
static void fail_safe_fan_max(void) {
    if (auto_mode_enabled) {
        /* Temporarily disable auto mode to set manual control */
        auto_mode_enabled = 0;
        set_fan_duty(100);
        current_fan_duty = 100;
        LOG_INFO("FAIL-SAFE: Fans set to 100%% for safety");
    }
}

/* Signal handlers */
static void on_sigint(int sig) {
    (void)sig;
    /* Fail-safe: Set fans to maximum before shutdown */
    fail_safe_fan_max();
    /* Clean up PID file */
    unlink("/run/dudes-control-center-fan-daemon.pid");
    g_running = 0;
}

static void on_sighup(int sig) {
    (void)sig;
    /* Reload configuration if needed */
}

/* Initialize default profile */
static void init_default_profile(void) {
    FanProfile *profile = &profiles[0];
    strcpy(profile->name, "Default");
    profile->num_points = 6;
    profile->points[0].temperature = 45; profile->points[0].fan_percent = 20;
    profile->points[1].temperature = 55; profile->points[1].fan_percent = 30;
    profile->points[2].temperature = 60; profile->points[2].fan_percent = 40;
    profile->points[3].temperature = 70; profile->points[3].fan_percent = 50;
    profile->points[4].temperature = 80; profile->points[4].fan_percent = 70;
    profile->points[5].temperature = 85; profile->points[5].fan_percent = 100;
    profile->is_active = 1;
    num_profiles = 1;
    active_profile_idx = 0;
}

/* Save all profiles to disk as JSON */
static void save_profiles(void) {
    /* Build JSON document */
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    
    cJSON_AddStringToObject(root, "version", "1.0");
    cJSON_AddNumberToObject(root, "active_profile_index", active_profile_idx);
    
    cJSON *profiles_array = cJSON_CreateArray();
    for (int i = 0; i < num_profiles; i++) {
        FanProfile *p = &profiles[i];
        cJSON *profile_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(profile_obj, "name", p->name);
        cJSON_AddBoolToObject(profile_obj, "is_active", p->is_active ? true : false);
        
        cJSON *curve_array = cJSON_CreateArray();
        for (int j = 0; j < p->num_points; j++) {
            cJSON *point = cJSON_CreateObject();
            cJSON_AddNumberToObject(point, "temperature", p->points[j].temperature);
            cJSON_AddNumberToObject(point, "fan_percent", p->points[j].fan_percent);
            cJSON_AddItemToArray(curve_array, point);
        }
        cJSON_AddItemToObject(profile_obj, "curve_points", curve_array);
        cJSON_AddItemToArray(profiles_array, profile_obj);
    }
    cJSON_AddItemToObject(root, "profiles", profiles_array);
    
    /* Write to file */
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        LOG_WARN("Failed to serialize profiles to JSON");
        return;
    }
    
    FILE *fp = fopen(PROFILE_PATH, "w");
    if (!fp) {
        free(json_str);
        cJSON_Delete(root);
        LOG_WARN("Could not open %s for writing: %s", PROFILE_PATH, strerror(errno));
        return;
    }
    
    fprintf(fp, "%s\n", json_str);
    fclose(fp);
    free(json_str);
    cJSON_Delete(root);
    
    LOG_INFO("Saved %d profiles to %s", num_profiles, PROFILE_PATH);
}

/* Load all profiles from disk as JSON */
static void load_profiles(void) {
    FILE *fp = fopen(PROFILE_PATH, "r");
    if (!fp) {
        LOG_INFO("No saved profiles found at %s, using defaults", PROFILE_PATH);
        init_default_profile();
        return;
    }
    
    /* Read entire file into buffer */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0) {
        fclose(fp);
        LOG_WARN("Empty profile file at %s, using defaults", PROFILE_PATH);
        init_default_profile();
        return;
    }
    
    char *json_buf = malloc(file_size + 1);
    if (!json_buf) {
        fclose(fp);
        LOG_WARN("Failed to allocate memory for profile loading");
        init_default_profile();
        return;
    }
    
    size_t bytes_read = fread(json_buf, 1, file_size, fp);
    json_buf[bytes_read] = '\0';
    fclose(fp);
    
    /* Parse JSON */
    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);
    
    if (!root) {
        LOG_WARN("Invalid JSON in profile file %s: %s", PROFILE_PATH, cJSON_GetErrorPtr());
        init_default_profile();
        return;
    }
    
    /* Check version */
    cJSON *version = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsString(version) || strcmp(version->valuestring, "1.0") != 0) {
        LOG_WARN("Unsupported profile file version, using defaults");
        cJSON_Delete(root);
        init_default_profile();
        return;
    }
    
    /* Read active profile index */
    cJSON *active_idx = cJSON_GetObjectItem(root, "active_profile_index");
    if (cJSON_IsNumber(active_idx)) {
        active_profile_idx = active_idx->valueint;
    } else {
        active_profile_idx = 0;
    }
    
    /* Read profiles array */
    cJSON *profiles_array = cJSON_GetObjectItem(root, "fan_profiles");
    if (!cJSON_IsArray(profiles_array)) {
        LOG_WARN("Missing profiles array in %s, using defaults", PROFILE_PATH);
        cJSON_Delete(root);
        init_default_profile();
        return;
    }
    
    num_profiles = cJSON_GetArraySize(profiles_array);
    if (num_profiles <= 0 || num_profiles > 10) {
        LOG_WARN("Invalid profile count: %d, using defaults", num_profiles);
        cJSON_Delete(root);
        init_default_profile();
        return;
    }
    
    /* Parse each profile */
    for (int i = 0; i < num_profiles; i++) {
        FanProfile *p = &profiles[i];
        p->num_points = 0;
        p->is_active = 0;
        
        cJSON *profile_obj = cJSON_GetArrayItem(profiles_array, i);
        if (!cJSON_IsObject(profile_obj)) continue;
        
        /* Read name */
        cJSON *name = cJSON_GetObjectItem(profile_obj, "name");
        if (cJSON_IsString(name)) {
            strncpy(p->name, name->valuestring, sizeof(p->name) - 1);
            p->name[sizeof(p->name) - 1] = '\0';
        } else {
            snprintf(p->name, sizeof(p->name), "Profile_%d", i);
        }
        
        /* Read is_active */
        cJSON *is_active = cJSON_GetObjectItem(profile_obj, "is_active");
        if (cJSON_IsBool(is_active)) {
            p->is_active = (cJSON_IsTrue(is_active)) ? 1 : 0;
        }
        
        /* Read curve points */
        cJSON *curve_array = cJSON_GetObjectItem(profile_obj, "curve_points");
        if (cJSON_IsArray(curve_array)) {
            int num_points = cJSON_GetArraySize(curve_array);
            for (int j = 0; j < num_points && j < 50; j++) {
                cJSON *point = cJSON_GetArrayItem(curve_array, j);
                if (cJSON_IsObject(point)) {
                    cJSON *temp = cJSON_GetObjectItem(point, "temperature");
                    cJSON *fan = cJSON_GetObjectItem(point, "fan_percent");
                    if (cJSON_IsNumber(temp) && cJSON_IsNumber(fan)) {
                        p->points[j].temperature = temp->valueint;
                        p->points[j].fan_percent = fan->valueint;
                        p->num_points++;
                    }
                }
            }
        }
    }
    
    cJSON_Delete(root);
    LOG_INFO("Loaded %d profiles from %s (active: %d)", num_profiles, PROFILE_PATH, active_profile_idx);
}

/* Calculate target fan duty from temperature using active profile */
static int get_target_duty_from_temp(int temp_c) {
    if (active_profile_idx < 0 || active_profile_idx >= num_profiles) {
        return 20; /* Default fallback */
    }
    
    FanProfile *profile = &profiles[active_profile_idx];
    TempCurvePoint *points = profile->points;
    int n = profile->num_points;
    
    if (n == 0) return 20;
    
    /* Sort points by temperature (simple bubble sort) */
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (points[j].temperature > points[j + 1].temperature) {
                TempCurvePoint tmp = points[j];
                points[j] = points[j + 1];
                points[j + 1] = tmp;
            }
        }
    }
    
    /* Edge cases */
    if (temp_c <= points[0].temperature) {
        return points[0].fan_percent;
    }
    if (temp_c >= points[n - 1].temperature) {
        return points[n - 1].fan_percent;
    }
    
    /* Linear interpolation */
    for (int i = 0; i < n - 1; i++) {
        int t1 = points[i].temperature, p1 = points[i].fan_percent;
        int t2 = points[i + 1].temperature, p2 = points[i + 1].fan_percent;
        if (temp_c >= t1 && temp_c < t2) {
            double fraction = (double)(temp_c - t1) / (double)(t2 - t1);
            double percentage = p1 + (p2 - p1) * fraction;
            return (int)(percentage + 0.5);
        }
    }
    
    return points[n - 1].fan_percent;
}

/* Parse command from client */
static int handle_command(const char *cmd, char *response, size_t resp_size) {
    char cmd_copy[MAX_CMD_LEN];
    strncpy(cmd_copy, cmd, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';
    
    /* Remove trailing newline */
    size_t len = strlen(cmd_copy);
    if (len > 0 && cmd_copy[len - 1] == '\n') {
        cmd_copy[len - 1] = '\0';
    }
    
    /* Parse command */
    char action[32];
    char arg1[64] = "";
    char arg2[64] = "";
    
    sscanf(cmd_copy, "%31s %63s %63s", action, arg1, arg2);
    
    if (debug_mode) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        fprintf(stderr, "%s [DAEMON] Received command: '%s' args='%s' '%s'\n", ts, action, arg1, arg2);
    }
    
    /* Command: STATUS */
    if (strcmp(action, "STATUS") == 0) {
        int cpu_temp = get_cpu_temp();
        int gpu_temp = get_gpu_temp();
        int cpu_fan_rpm = get_cpu_fan_rpm();
        int gpu_fan_rpm = get_gpu_fan_rpm();
        /* Use tracked duty value instead of hardware read (which can be unreliable) */
        int fan_duty = current_fan_duty;
        
        snprintf(response, resp_size, 
            "STATUS OK\n"
            "CPU_TEMP=%d\n"
            "GPU_TEMP=%d\n"
            "CPU_FAN_RPM=%d\n"
            "GPU_FAN_RPM=%d\n"
            "FAN_DUTY=%d\n"
            "AUTO_MODE=%d\n",
            cpu_temp, gpu_temp, cpu_fan_rpm, gpu_fan_rpm, fan_duty, auto_mode_enabled);
        return 0;
    }
    
    /* Command: SET_MANUAL <percentage> */
    if (strcmp(action, "SET_MANUAL") == 0) {
        int pct = atoi(arg1);
        if (pct < 0 || pct > 100) {
            snprintf(response, resp_size, "ERROR Invalid percentage\n");
            return -1;
        }
        
        auto_mode_enabled = 0;
        manual_duty = pct;
        current_fan_duty = pct; /* Track for STATUS reporting */
        set_fan_duty(pct);
        
        snprintf(response, resp_size, "OK Fan set to %d%%\n", pct);
        return 0;
    }
    
    /* Command: SET_AUTO <0|1> */
    if (strcmp(action, "SET_AUTO") == 0) {
        auto_mode_enabled = atoi(arg1);
        snprintf(response, resp_size, "OK Auto mode %s\n", 
                 auto_mode_enabled ? "enabled" : "disabled");
        return 0;
    }
    
    /* Command: LIST_PROFILES */
    if (strcmp(action, "LIST_PROFILES") == 0) {
        int offset = snprintf(response, resp_size, "PROFILES_START\n");
        for (int i = 0; i < num_profiles && offset < (int)resp_size - 64; i++) {
            char active_marker = (i == active_profile_idx) ? '*' : ' ';
            int written = snprintf(response + offset, resp_size - offset,
                     "PROFILE %d [%c] %s\n", i, active_marker, profiles[i].name);
            if (written < 0) break;
            offset += written;
        }
        if (offset < (int)resp_size - 16) {
            strcat(response, "PROFILES_END\n");
        }
        return 0;
    }
    
    /* Command: ACTIVATE_PROFILE <profile_idx> */
    if (strcmp(action, "ACTIVATE_PROFILE") == 0) {
        int idx = atoi(arg1);
        if (idx < 0 || idx >= num_profiles) {
            snprintf(response, resp_size, "ERROR Invalid profile index\n");
            return -1;
        }
        
        /* Deactivate all profiles */
        for (int i = 0; i < num_profiles; i++) {
            profiles[i].is_active = 0;
        }
        
        /* Activate the selected profile */
        profiles[idx].is_active = 1;
        active_profile_idx = idx;
        
        snprintf(response, resp_size, "OK Profile %d activated\n", idx);
        return 0;
    }
    
    /* Command: CREATE_PROFILE <name> */
    if (strcmp(action, "CREATE_PROFILE") == 0) {
        if (num_profiles >= 10) {
            snprintf(response, resp_size, "ERROR Max profiles reached\n");
            return -1;
        }
        
        /* Extract profile name from original command to support names with spaces.
         * arg1 is space-truncated by the generic sscanf parser above. */
        const char *profile_name = strstr(cmd_copy, "CREATE_PROFILE");
        if (profile_name != NULL) {
            profile_name += strlen("CREATE_PROFILE");
            /* Skip any whitespace after the command */
            while (*profile_name == ' ') profile_name++;
        } else {
            profile_name = "";
        }
        
        FanProfile *profile = &profiles[num_profiles];
        snprintf(profile->name, sizeof(profile->name), "%s", profile_name);
        profile->num_points = 0;
        profile->is_active = 0;
        num_profiles++;
        
        snprintf(response, resp_size, "OK Profile created: %s (index %d)\n", 
                 profile->name, num_profiles - 1);
        return 0;
    }
    
    /* Command: ADD_CURVE_POINT <profile_idx> <temp> <fan_pct> */
    if (strcmp(action, "ADD_CURVE_POINT") == 0) {
        int idx = atoi(arg1);
        int temp = atoi(arg2);
        char arg3[32];
        sscanf(cmd_copy, "%*s %*d %*d %31s", arg3);
        int fan_pct = atoi(arg3);
        
        if (idx < 0 || idx >= num_profiles) {
            snprintf(response, resp_size, "ERROR Invalid profile index\n");
            return -1;
        }
        
        FanProfile *profile = &profiles[idx];
        if (profile->num_points >= 50) {
            snprintf(response, resp_size, "ERROR Max curve points reached\n");
            return -1;
        }
        
        profile->points[profile->num_points].temperature = temp;
        profile->points[profile->num_points].fan_percent = fan_pct;
        profile->num_points++;
        
        snprintf(response, resp_size, "OK Curve point added\n");
        return 0;
    }
    
    /* Command: GET_PROFILE <idx> */
    if (strcmp(action, "GET_PROFILE") == 0) {
        int idx = atoi(arg1);
        if (idx < 0 || idx >= num_profiles) {
            snprintf(response, resp_size, "ERROR Invalid profile index\n");
            return -1;
        }
        
        FanProfile *profile = &profiles[idx];
        snprintf(response, resp_size, 
                 "PROFILE_INFO name=%s points=%d active=%d\n",
                 profile->name, profile->num_points, profile->is_active);
        
        for (int i = 0; i < profile->num_points; i++) {
            char point_str[64];
            snprintf(point_str, sizeof(point_str), 
                     "CURVE_POINT temp=%d fan_pct=%d\n",
                     profile->points[i].temperature, profile->points[i].fan_percent);
            strncat(response, point_str, resp_size - strlen(response) - 1);
        }
        
        strcat(response, "PROFILE_END\n");
        return 0;
    }
    
    /* Command: DELETE_CURVE_POINT <profile_idx> <point_index> */
    if (strcmp(action, "DELETE_CURVE_POINT") == 0) {
        int profile_idx = atoi(arg1);
        int point_idx = atoi(arg2);
        
        if (profile_idx < 0 || profile_idx >= num_profiles) {
            snprintf(response, resp_size, "ERROR Invalid profile index\n");
            return -1;
        }
        
        FanProfile *profile = &profiles[profile_idx];
        if (point_idx < 0 || point_idx >= profile->num_points) {
            snprintf(response, resp_size, "ERROR Invalid curve point index\n");
            return -1;
        }
        
        /* Shift all points after the deleted one */
        for (int i = point_idx; i < profile->num_points - 1; i++) {
            profile->points[i] = profile->points[i + 1];
        }
        profile->num_points--;
        
        snprintf(response, resp_size, "OK Curve point deleted\n");
        return 0;
    }
    
    /* Command: SET_CURVE_POINTS <profile_idx> <num_points> <temp1,fan_pct1,temp2,fan_pct2,...>
     * This replaces all curve points for a profile with the provided list
     */
    if (strcmp(action, "SET_CURVE_POINTS") == 0) {
        int profile_idx = atoi(arg1);
        int num_points = atoi(arg2);
        
        if (profile_idx < 0 || profile_idx >= num_profiles) {
            snprintf(response, resp_size, "ERROR Invalid profile index\n");
            return -1;
        }
        
        if (num_points < 0 || num_points > 50) {
            snprintf(response, resp_size, "ERROR Invalid number of points\n");
            return -1;
        }
        
        FanProfile *profile = &profiles[profile_idx];
        profile->num_points = 0;  /* Clear existing points */
        
        /* Parse the curve points from arg3 onwards
         * Format: temp1,fan_pct1,temp2,fan_pct2,... */
        char arg3[512];
        sscanf(cmd_copy, "%*s %*d %*d %511s", arg3);
        
        char *saveptr;
        char *token = strtok_r(arg3, ",", &saveptr);
        int i = 0;
        while (token && i < num_points) {
            int temp = atoi(token);
            token = strtok_r(NULL, ",", &saveptr);
            if (!token) break;
            int fan_pct = atoi(token);
            
            profile->points[profile->num_points].temperature = temp;
            profile->points[profile->num_points].fan_percent = fan_pct;
            profile->num_points++;
            i++;
            
            token = strtok_r(NULL, ",", &saveptr);
        }
        
        snprintf(response, resp_size, "OK Curve points updated: %d points\n", profile->num_points);
        return 0;
    }
    
    /* Command: SET_ACTIVE_PROFILE <idx> */
    if (strcmp(action, "SET_ACTIVE_PROFILE") == 0) {
        int idx = atoi(arg1);
        if (idx < 0 || idx >= num_profiles) {
            snprintf(response, resp_size, "ERROR Invalid profile index\n");
            return -1;
        }
        
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        fprintf(stderr, "%s [DAEMON] SET_ACTIVE_PROFILE: Activating profile %d (%s)\n", ts, idx, profiles[idx].name);
        
        /* Deactivate all profiles */
        for (int i = 0; i < num_profiles; i++) {
            profiles[i].is_active = 0;
        }
        
        profiles[idx].is_active = 1;
        active_profile_idx = idx;
        
        /* Enable auto mode when a profile is activated */
        auto_mode_enabled = 1;
        get_timestamp(ts, sizeof(ts));
        fprintf(stderr, "%s [DAEMON] Auto mode ENABLED for profile: %s\n", ts, profiles[idx].name);
        
        snprintf(response, resp_size, "OK Active profile set to: %s\n", 
                 profiles[idx].name);
        return 0;
    }
    
    /* Command: DELETE_PROFILE <idx> */
    if (strcmp(action, "DELETE_PROFILE") == 0) {
        int idx = atoi(arg1);
        if (idx < 0 || idx >= num_profiles) {
            snprintf(response, resp_size, "ERROR Invalid profile index\n");
            return -1;
        }
        
        /* Don't allow deleting the default profile */
        if (strcmp(profiles[idx].name, "Default") == 0) {
            snprintf(response, resp_size, "ERROR Cannot delete Default profile\n");
            return -1;
        }
        
        /* Shift all profiles after this one down by one */
        for (int i = idx; i < num_profiles - 1; i++) {
            profiles[i] = profiles[i + 1];
        }
        num_profiles--;
        
        snprintf(response, resp_size, "OK Profile deleted\n");
        return 0;
    }
    
    /* Command: GET_CURVE_FOR_TEMP <temp> */
    if (strcmp(action, "GET_CURVE_FOR_TEMP") == 0) {
        int temp = atoi(arg1);
        int duty = get_target_duty_from_temp(temp);
        snprintf(response, resp_size, "CURVE_DUTY %d\n", duty);
        return 0;
    }
    
    /* Command: SAVE_PROFILES */
    if (strcmp(action, "SAVE_PROFILES") == 0) {
        snprintf(response, resp_size, "OK Profiles saved (by GUI)\n");
        return 0;
    }
    
    /* Command: SET_LOG_LEVEL <ERROR|WARN|INFO|DEBUG> */
    if (strcmp(action, "SET_LOG_LEVEL") == 0) {
        if (strcmp(arg1, "ERROR") == 0) {
            log_set_level(LOG_LEVEL_ERROR);
            snprintf(response, resp_size, "OK Log level set to ERROR\n");
        } else if (strcmp(arg1, "WARN") == 0) {
            log_set_level(LOG_LEVEL_WARN);
            snprintf(response, resp_size, "OK Log level set to WARN\n");
        } else if (strcmp(arg1, "INFO") == 0) {
            log_set_level(LOG_LEVEL_INFO);
            snprintf(response, resp_size, "OK Log level set to INFO\n");
        } else if (strcmp(arg1, "DEBUG") == 0) {
            log_set_level(LOG_LEVEL_DEBUG);
            snprintf(response, resp_size, "OK Log level set to DEBUG\n");
        } else {
            snprintf(response, resp_size, "ERROR Invalid log level: %s (use ERROR, WARN, INFO, DEBUG)\n", arg1);
            return -1;
        }
        return 0;
    }
    
    /* Command: GET_LOG_LEVEL */
    if (strcmp(action, "GET_LOG_LEVEL") == 0) {
        LogLevel current_level = log_get_level();
        snprintf(response, resp_size, "LOG_LEVEL=%s\n", log_level_to_string(current_level));
        return 0;
    }
    
    /* Unknown command */
    snprintf(response, resp_size, "ERROR Unknown command: %s\n", action);
    return -1;
}

/* Per-client buffer for accumulating partial messages */
static char client_buffers[MAX_CLIENTS][BUFFER_SIZE];
static int client_buffer_len[MAX_CLIENTS];

/* Handle client connection - process ALL available complete commands
 * Returns: 0 if client still connected, 1 if client disconnected */
static int handle_client(int client_fd, int client_idx) {
    char response[BUFFER_SIZE];
    ssize_t bytes_read;
    
    if (debug_mode) {
        fprintf(stderr, "[DAEMON] handle_client called for slot %d, fd=%d, initial buffer_len=%d\n", client_idx, client_fd, client_buffer_len[client_idx]);
    }
    
    /* Process all complete commands in the buffer */
    while (1) {
        /* Check if we have a complete line */
        char *newline = memchr(client_buffers[client_idx], '\n', client_buffer_len[client_idx]);
        
        if (newline != NULL) {
            /* We have at least one complete command - process it */
            size_t cmd_len = newline - client_buffers[client_idx] + 1;  // Include the newline
            
            /* Null-terminate the command */
            client_buffers[client_idx][cmd_len] = '\0';
            
            /* Log incoming command */
            char cmd_log[BUFFER_SIZE];
            strncpy(cmd_log, client_buffers[client_idx], BUFFER_SIZE - 1);
            cmd_log[BUFFER_SIZE - 1] = '\0';
            size_t len = strlen(cmd_log);
            if (len > 0 && cmd_log[len-1] == '\n') cmd_log[len-1] = '\0';
            
            /* Parse action for logging */
            char action[64] = {0};
            char arg1[64] = {0};
            sscanf(cmd_log, "%63s %63s", action, arg1);
            
            if (debug_mode) {
                char ts[32];
                get_timestamp(ts, sizeof(ts));
                fprintf(stderr, "%s [DAEMON] Received command: '%s' args='%s'\n", ts, action, arg1);
            }
            
            /* Process command */
            int result = handle_command(client_buffers[client_idx], response, sizeof(response));
            
            /* Only log non-STATUS responses to avoid flooding the terminal with periodic updates */
            if (debug_mode && strncmp(response, "STATUS OK", 9) != 0) {
                char ts[32];
                get_timestamp(ts, sizeof(ts));
                fprintf(stderr, "%s [DAEMON] Sending response: '%s' (result=%d)\n", ts, response, result);
            }
            
            /* Send response */
            #pragma GCC diagnostic push
            #pragma GCC diagnostic ignored "-Wunused-result"
            write(client_fd, response, strlen(response));
            #pragma GCC diagnostic pop
            
            /* Remove processed command from buffer */
            size_t remaining = client_buffer_len[client_idx] - cmd_len;
            if (remaining > 0) {
                memmove(client_buffers[client_idx], client_buffers[client_idx] + cmd_len, remaining);
            }
            client_buffer_len[client_idx] = remaining;
            
            /* Continue to process next command in buffer */
            continue;
        }
        
        /* No complete line yet - try to read more data */
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;  // 10ms timeout for non-blocking behavior
        
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);
        
        int ret = select(client_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret <= 0) {
            /* No more data available right now */
            break;  // Exit and let main loop call us again
        }
        
        bytes_read = recv(client_fd, client_buffers[client_idx] + client_buffer_len[client_idx],
                         BUFFER_SIZE - client_buffer_len[client_idx] - 1, 0);
        
        if (bytes_read <= 0) {
            if (debug_mode) {
                char ts[32];
                get_timestamp(ts, sizeof(ts));
                if (bytes_read == 0) {
                    fprintf(stderr, "%s [DAEMON] Client on fd %d disconnected\n", ts, client_fd);
                } else {
                    fprintf(stderr, "%s [DAEMON] recv error on fd %d: %s\n", ts, client_fd, strerror(errno));
                }
            }
            /* Connection closed or error - close fd and remove from active clients */
            close(client_fd);
            client_fds[client_idx] = -1;
            client_buffer_len[client_idx] = 0;
            if (num_clients > 0) num_clients--;
            return 1;  /* Return 1 to indicate disconnection */
        }
        
        if (debug_mode) {
            fprintf(stderr, "[DAEMON] Received %zd bytes from client, total buffer_len=%d\n", bytes_read, client_buffer_len[client_idx] + (int)bytes_read);
        }
        
        client_buffer_len[client_idx] += bytes_read;
    }
    
    return 0;  /* Client still connected */
}

/* Remove old socket file */
static void remove_old_socket(void) {
    struct stat st;
    if (stat(SOCKET_PATH, &st) == 0) {
        unlink(SOCKET_PATH);
    }
}

/* Create server socket */
static int create_server_socket(void) {
    remove_old_socket();
    
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    /* Set umask to allow all users before binding */
    mode_t old_umask = umask(000);
    
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        umask(old_umask);
        close(server_fd);
        return -1;
    }
    
    /* Restore old umask */
    umask(old_umask);
    
    /* Explicitly set socket permissions to allow read/write for all users */
    if (chmod(SOCKET_PATH, 0666) < 0) {
        perror("chmod");
        close(server_fd);
        unlink(SOCKET_PATH);
        return -1;
    }
    
    /* Verify socket permissions before listening */
    struct stat st;
    if (stat(SOCKET_PATH, &st) == 0) {
        printf("Socket created at %s with permissions: %o\n", SOCKET_PATH, st.st_mode & 0777);
        printf("Socket owner: uid=%u, gid=%u\n", st.st_uid, st.st_gid);
    }
    
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(server_fd);
        unlink(SOCKET_PATH);
        return -1;
    }
    
    return 0;
}

/* Auto mode loop - unused, kept for reference */
static __attribute__((unused)) void *auto_mode_loop(void *arg) {
    (void)arg;
    
    int current_duty = 20;
    int ring[3] = {-1, -1, -1};
    int idx = 0, filled = 0;
    
    while (g_running && auto_mode_enabled) {
        int cpu_temp = get_cpu_temp();
        int gpu_temp = get_gpu_temp();
        
        /* Determine the hotter temperature */
        int hot_temp = cpu_temp;
        if (gpu_temp >= 0 && gpu_temp > cpu_temp) {
            hot_temp = gpu_temp;
        }
        
        /* Safety check for invalid CPU temp */
        if (cpu_temp < TEMP_MIN_VALID_C || cpu_temp > TEMP_MAX_VALID_C) {
            current_duty = 100;
            set_fan_duty(100);
        } else {
            /* Update ring buffer */
            ring[idx] = hot_temp;
            idx = (idx + 1) % 3;
            if (filled < 3) filled++;
            
            /* Calculate average temperature */
            int sum = 0;
            for (int i = 0; i < filled; i++) {
                sum += ring[i];
            }
            int avg_temp = sum / filled;
            
            /* Get target duty from profile curve */
            int target_duty = get_target_duty_from_temp(avg_temp);
            
            /* Smooth transitions */
            int diff = target_duty - current_duty;
            if (diff >= 5) {
                current_duty += 5;
                set_fan_duty(current_duty);
            } else if (diff <= -1) {
                current_duty -= 1;
                set_fan_duty(current_duty);
            }
        }
        
        usleep(1000 * 1000); /* 1 second interval */
    }
    
    return NULL;
}

int main(int argc, char *argv[]) {
    int daemon_mode = 1;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--foreground") == 0 || strcmp(argv[i], "-f") == 0) {
            daemon_mode = 0;
        } else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0) {
            debug_mode = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [OPTIONS]\n", PROGRAM_NAME);
            printf("\n");
            printf("Options:\n");
            printf("  --foreground, -f  Run in foreground (for debugging)\n");
            printf("  --debug, -d       Enable verbose debug logging\n");
            printf("  --help, -h        Show this help message\n");
            return EXIT_SUCCESS;
        }
    }
    
    /* Daemonize if requested */
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return EXIT_FAILURE;
        }
        if (pid > 0) {
            exit(0); /* Parent exits */
        }
        
        /* Create new session */
        if (setsid() < 0) {
            perror("setsid");
            return EXIT_FAILURE;
        }
        
        /* Change to root directory */
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wunused-result"
        chdir("/");
        #pragma GCC diagnostic pop
        
        /* Close standard file descriptors */
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }
    
    /* Check for duplicate instance via PID file */
    {
        const char *pid_file = "/run/dudes-control-center-fan-daemon.pid";
        FILE *pf = fopen(pid_file, "r");
        if (pf) {
            int old_pid;
            if (fscanf(pf, "%d", &old_pid) == 1) {
                /* Check if old process is still alive */
                if (kill(old_pid, 0) == 0) {
                    fprintf(stderr, "%s: Another instance already running (PID %d). Exiting.\n", PROGRAM_NAME, old_pid);
                    fclose(pf);
                    return EXIT_FAILURE;
                }
            }
            fclose(pf);
        }
        /* Remove stale PID file or write our own */
        unlink(pid_file);
        pf = fopen(pid_file, "w");
        if (pf) {
            fprintf(pf, "%d\n", getpid());
            fclose(pf);
        }
    }
    
    printf("%s starting...\n", PROGRAM_NAME);
    
    /* Initialize logging (NULL = use /var/log for daemon) */
    if (log_init("FAN_DAEMON", debug_mode ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO, NULL) != 0) {
        fprintf(stderr, "Failed to initialize logging system\n");
    }
    
    LOG_INFO("Daemon starting...");
    
    /* Load saved profiles or initialize defaults */
    load_profiles();
    
    /* Enable auto mode by default if there's an active profile */
    if (active_profile_idx >= 0 && active_profile_idx < num_profiles) {
        auto_mode_enabled = 1;
        fprintf(stderr, "[DAEMON] Auto mode enabled with profile: %s\n", profiles[active_profile_idx].name);
    }
    
    /* Set up signal handlers */
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGHUP, on_sighup);
    
    /* Initialize client buffers */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_buffer_len[i] = 0;
        memset(client_buffers[i], 0, BUFFER_SIZE);
    }
    
    /* Initialize EC access */
    if (ec_init() != 0) {
        fprintf(stderr, "Error: Failed to initialize EC access\n");
        return EXIT_FAILURE;
    }
    
    /* Create server socket */
    if (create_server_socket() != 0) {
        fprintf(stderr, "Error: Failed to create server socket\n");
        ec_cleanup();
        return EXIT_FAILURE;
    }
    
    printf("Server listening on %s\n", SOCKET_PATH);
    
    /* Set initial fan speed based on current temperatures and active profile */
    if (auto_mode_enabled && active_profile_idx >= 0) {
        int cpu_temp = get_cpu_temp();
        int gpu_temp = get_gpu_temp();
        
        /* Use the hotter temperature */
        int hot_temp = cpu_temp;
        if (gpu_temp >= 0 && gpu_temp > cpu_temp) {
            hot_temp = gpu_temp;
        }
        
        /* Validate temperature */
        if (hot_temp < 10 || hot_temp > 110) {
            fprintf(stderr, "[DAEMON] WARNING: Invalid temp %d°C on startup, using safe default\n", hot_temp);
            hot_temp = 50; /* Safe default */
        }
        
        int initial_duty = get_target_duty_from_temp(hot_temp);
        if (initial_duty > 100) initial_duty = 100;
        if (initial_duty < 0) initial_duty = 0;
        
        current_fan_duty = initial_duty;
        set_fan_duty(initial_duty);
        
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        fprintf(stderr, "%s [DAEMON] Initial fan speed set to %d%% based on temp CPU=%d°C GPU=%d°C (profile: %s)\n", 
                ts, initial_duty, cpu_temp, gpu_temp, profiles[active_profile_idx].name);
    }
    
    /* Main event loop */
    fd_set read_fds;
    int max_fd = server_fd;
    
    while (g_running) {
        /* Watchdog: Check if main loop is still responsive */
        time_t now = time(NULL);
        if (last_loop_time != 0 && (now - last_loop_time > WATCHDOG_TIMEOUT_SEC)) {
            LOG_ERROR("WATCHDOG TRIGGERED: Main loop unresponsive for %ld seconds!", (long)(now - last_loop_time));
            fail_safe_fan_max();
            break;  /* Exit main loop and shutdown */
        }
        last_loop_time = now;
        
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        
        for (int i = 0; i < num_clients; i++) {
            if (client_fds[i] > 0) {
                FD_SET(client_fds[i], &read_fds);
                if (client_fds[i] > max_fd) {
                    max_fd = client_fds[i];
                }
            }
        }
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (activity < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        /* Check for new connections */
        if (FD_ISSET(server_fd, &read_fds)) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd >= 0) {
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_fds[i] <= 0) {
                        client_fds[i] = client_fd;
                        num_clients++;
                        if (debug_mode) {
                            char ts[32];
                            get_timestamp(ts, sizeof(ts));
                            fprintf(stderr, "%s [DAEMON] New client connected on fd %d (slot %d), total clients: %d\n", ts, client_fd, i, num_clients);
                        }
                        break;
                    }
                }
            }
        }
        
        /* Handle existing clients */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] > 0 && FD_ISSET(client_fds[i], &read_fds)) {
                if (debug_mode) {
                    char ts[32];
                    get_timestamp(ts, sizeof(ts));
                    fprintf(stderr, "%s [DAEMON] Activity detected on client fd %d (slot %d), buffer_len=%d\n", ts, client_fds[i], i, client_buffer_len[i]);
                }
                
                /* Handle client - returns 1 if disconnected, 0 if still connected */
                int client_disconnected = handle_client(client_fds[i], i);
                
                /* Only do peek check if client is still connected */
                if (!client_disconnected) {
                    /* Check if client disconnected by trying to peek at socket */
                    /* Only close on actual EOF/error, not when socket is just empty (normal for request/response) */
                    char buf[1];
                    ssize_t peek_result = recv(client_fds[i], buf, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (peek_result < 0) {
                        /* Error occurred - check if it's just EAGAIN/EWOULDBLOCK (no data yet) */
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            if (debug_mode) {
                                char ts[32];
                                get_timestamp(ts, sizeof(ts));
                                fprintf(stderr, "%s [DAEMON] Client fd %d error: %s\n", ts, client_fds[i], strerror(errno));
                            }
                            close(client_fds[i]);
                            client_fds[i] = -1;
                            client_buffer_len[i] = 0;
                            num_clients--;
                        }
                        /* If EAGAIN/EWOULDBLOCK, socket is just empty - keep connection alive */
                    }
                    /* peek_result == 0 means EOF (client closed), but we don't expect this in normal operation
                     * The client will close when it exits, which we'll detect on next select() */
                }
            }
        }
        
        /* Check auto mode */
        static int auto_mode_counter = 0;
        auto_mode_counter++;
        
        if (debug_mode) {
            char ts[32];
            get_timestamp(ts, sizeof(ts));
            fprintf(stderr, "%s [DAEMON] Main loop iteration %d: enabled=%d running=%d mod5=%d\n", 
                    ts, auto_mode_counter, auto_mode_enabled, g_running, auto_mode_counter % 5);
            fflush(stderr);  // Force flush to see output immediately
        }
        
        if (auto_mode_enabled && g_running && (auto_mode_counter % 5 == 0)) {
            /* Auto mode is handled in the main loop here for simplicity */
            char ts[32];
            get_timestamp(ts, sizeof(ts));
            fprintf(stderr, "%s [DAEMON] *** AUTO MODE LOOP EXECUTING (profile: %s) ***\n", 
                    ts, active_profile_idx >= 0 ? profiles[active_profile_idx].name : "NONE");
            
            int cpu_temp = get_cpu_temp();
            int gpu_temp = get_gpu_temp();
            
            if (debug_mode) {
                fprintf(stderr, "[DAEMON] Raw temps: CPU=%d GPU=%d\n", cpu_temp, gpu_temp);
            }
            
            int hot_temp = cpu_temp;
            if (gpu_temp >= 0 && gpu_temp > cpu_temp) {
                hot_temp = gpu_temp;
            }
            
            if (debug_mode) {
                fprintf(stderr, "[DAEMON] Temp validation: CPU=%d (valid range: %d-%d)\n", cpu_temp, TEMP_MIN_VALID_C, TEMP_MAX_VALID_C);
            }
            
            if (cpu_temp >= TEMP_MIN_VALID_C && cpu_temp <= TEMP_MAX_VALID_C) {
                static int ring[3] = {-1, -1, -1};
                static int idx = 0, filled = 0;
                
                ring[idx] = hot_temp;
                idx = (idx + 1) % 3;
                if (filled < 3) filled++;
                
                int sum = 0;
                for (int j = 0; j < filled; j++) {
                    sum += ring[j];
                }
                int avg_temp = sum / filled;
                
                int target_duty = get_target_duty_from_temp(avg_temp);
                
                /* Clamp target to valid range */
                if (target_duty > 100) target_duty = 100;
                if (target_duty < 0) target_duty = 0;
                
                /* Use clamped current duty for comparison (treat anything >= 100% as 100%) */
                int clamped_current = current_fan_duty;
                if (clamped_current > 100) clamped_current = 100;
                if (clamped_current < 0) clamped_current = 0;
                
                int diff = target_duty - clamped_current;
                
                char ts[32];
                get_timestamp(ts, sizeof(ts));
                fprintf(stderr, "%s [DAEMON] Auto mode: CPU=%d°C GPU=%d°C avg=%d°C target=%d%% current=%d%% (clamped=%d%%) diff=%d\n", 
                        ts, cpu_temp, gpu_temp, avg_temp, target_duty, current_fan_duty, clamped_current, diff);
                
                /* Adaptive ramp control to prevent oscillation */
                int new_duty = current_fan_duty;
                int made_change = 0;
                
                if (diff >= 5) {
                    /* Ramp up - use smaller steps when close to target */
                    if (diff >= 30) {
                        /* Large difference: jump aggressively */
                        new_duty = target_duty;  /* Go directly to target for large changes */
                    } else if (diff >= 15) {
                        /* Medium difference: moderate ramp */
                        new_duty += 15;
                    } else {
                        /* Small difference: gentle ramp to avoid overshoot */
                        new_duty += 5;  /* Smaller step when close */
                    }
                    
                    /* Cap at 100% */
                    if (new_duty > 100) new_duty = 100;
                    current_fan_duty = new_duty;
                    made_change = 1;
                } else if (diff <= -5) {
                    /* Ramp down - use smaller steps when close to target */
                    if (diff <= -30) {
                        /* Large difference: jump directly to target */
                        new_duty = target_duty;
                    } else if (diff <= -15) {
                        /* Medium difference: moderate ramp */
                        new_duty -= 15;
                    } else {
                        /* Small difference: gentle ramp to avoid overshoot */
                        new_duty -= 5;  /* Smaller step when close */
                    }
                    
                    /* Don't go below 0% */
                    if (new_duty < 0) new_duty = 0;
                    current_fan_duty = new_duty;
                    made_change = 1;
                } else {
                    /* Within deadband - no change needed, but ensure we're stable */
                    if (debug_mode) {
                        fprintf(stderr, "[DAEMON] Fan within ±5%% of target (%d%% vs %d%%), holding steady\n", clamped_current, target_duty);
                    }
                }
                
                /* Apply the new duty if changed */
                if (made_change) {
                    char ts[32];
                    get_timestamp(ts, sizeof(ts));
                    set_fan_duty(current_fan_duty);
                    fprintf(stderr, "%s [DAEMON] Setting fan duty to %d%% (target=%d%%, diff=%d)\n", 
                            ts, current_fan_duty, target_duty, diff);
                }
            }
        }
    }
    
    /* Cleanup */
    LOG_INFO("Shutting down...");
    log_cleanup();
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] > 0) {
            close(client_fds[i]);
        }
    }
    
    if (server_fd >= 0) {
        close(server_fd);
    }
    
    remove_old_socket();
    ec_cleanup();
    
    /* Clean up PID file on normal exit */
    unlink("/run/dudes-control-center-fan-daemon.pid");
    
    printf("Daemon stopped.\n");
    return EXIT_SUCCESS;
}
