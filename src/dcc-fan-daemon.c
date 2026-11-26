// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc-fan-daemon.c
 * Background daemon for controlling notebook fans
 * Runs as a systemd service and provides IPC via Unix socket
 */

#define _GNU_SOURCE   /* accept4() */
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
#include <stdint.h>
#include <stddef.h>
#include "dcc-common.h"
#include "dcc-log.h"
#include "dcc-secure.h"
#include <cjson/cJSON.h>

#define PROGRAM_NAME "dcc-fan-daemon"
#define SOCKET_PATH "/run/dcc-fan.sock"
/* profiles.json of the desktop user (see dcc_profiles_path in secure.h) */
static const char *profile_path(void) {
#ifdef PROFILE_PATH_OVERRIDE
    return PROFILE_PATH_OVERRIDE;
#else
    const char *p = dcc_profiles_path();
    return p ? p : "(no desktop user)";
#endif
}
#define PROFILE_PATH profile_path()
#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024
#define RESPONSE_SIZE 4096   /* large enough for a full 50-point GET_PROFILE */
#define MAX_CMD_LEN 256

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
} FanProfile;

/* Global state */
static volatile sig_atomic_t g_running = 1;
static int debug_mode = 0;
static FanProfile profiles[10];
static int num_profiles = 0;
static int active_profile_idx = -1;
static int auto_mode_enabled = 0;
static int manual_duty = 20;
/* Times manual mode was ended by an emergency (reported as EMERGENCY_AUTO=
 * in STATUS so the GUI can tell the user why the mode changed) */
static int emergency_auto_count = 0;
static int current_fan_duty = 20; /* Last duty successfully written to the EC */

/* --- Safety configuration --- */
#define CONTROL_INTERVAL_SEC     5   /* Temperature check / fan update interval */
#define SENSOR_FAIL_LIMIT        2   /* Consecutive invalid CPU readings before fail-safe */
#define EMERGENCY_TEMP_C        95   /* Any mode: force 100% at or above this temperature */
#define EMERGENCY_HYSTERESIS_C   5   /* ...and keep it until temp drops this far below */
#define LOOP_STALL_WARN_SEC     10   /* Log if one main-loop iteration takes this long */
#define SD_WATCHDOG_PING_SEC     2   /* How often to ping the systemd watchdog */

/* Safety override state: while active, fans are held at 100% in any mode */
typedef enum {
    OVERRIDE_NONE = 0,
    OVERRIDE_SENSOR_FAILURE = 1,
    OVERRIDE_EMERGENCY_TEMP = 2
} OverrideReason;
static OverrideReason override_reason = OVERRIDE_NONE;
static int sensor_fail_count = 0;

/* Built-in safe curve, used whenever the active profile is missing or empty */
static const TempCurvePoint DEFAULT_CURVE[] = {
    {45, 20}, {55, 30}, {60, 40}, {70, 50}, {80, 70}, {85, 100}
};
#define DEFAULT_CURVE_POINTS ((int)(sizeof(DEFAULT_CURVE) / sizeof(DEFAULT_CURVE[0])))

/* Socket file descriptors (-1 = free slot; fd 0 is a valid descriptor) */
static int server_fd = -1;
static int client_fds[MAX_CLIENTS];
static int num_clients = 0;

static const char *override_reason_str(OverrideReason r) {
    switch (r) {
        case OVERRIDE_SENSOR_FAILURE: return "sensor failure";
        case OVERRIDE_EMERGENCY_TEMP: return "emergency temperature";
        default:                      return "none";
    }
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Write a duty to the EC. current_fan_duty only changes if the write worked,
 * so a failed write is retried on the next control step. */
/* No dcc_fan module and no (allowed, plausible) direct EC access (1.41.0):
 * the daemon only reports temperatures; the firmware controls the fans and
 * nothing is written */
static int monitor_only;

static int apply_fan_duty(int pct) {
    if (monitor_only) return -1;
    pct = clamp(pct, 0, 100);
    if (set_fan_duty(pct) != 0) {
        LOG_ERROR("Failed to write fan duty %d%% to EC (EC not responding)", pct);
        return -1;
    }
    current_fan_duty = pct;
    return 0;
}

/* Fail-safe: Set fans to 100% regardless of mode */
static void fail_safe_fan_max(const char *reason) {
    if (apply_fan_duty(100) == 0) {
        LOG_WARN("FAIL-SAFE: Fans set to 100%% (%s)", reason);
    }
}

/* Minimal sd_notify() so we don't depend on libsystemd.
 * Does nothing when not started by systemd (NOTIFY_SOCKET unset). */
static void sd_notify_msg(const char *msg) {
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || !*path) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t len = strlen(path);
    if (len >= sizeof(addr.sun_path)) return;
    memcpy(addr.sun_path, path, len);
    if (addr.sun_path[0] == '@') addr.sun_path[0] = '\0';  /* abstract namespace */

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    (void)!sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&addr,
                  (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len));
    close(fd);
}

/* Signal handlers: only set a flag (async-signal-safe).
 * The fail-safe and cleanup run in main() after the loop ends. */
static void on_sigint(int sig) {
    (void)sig;
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
    num_profiles = 1;
    active_profile_idx = 0;
}

/* Load all profiles from disk as JSON */
static void load_profiles(void) {
    /* Read as the file's owner, never through a symlink (see secure.h) */
    char *json_buf = dcc_user_file_read(PROFILE_PATH, 1024 * 1024, NULL);
    if (!json_buf) {
        LOG_INFO("No readable profiles at %s, using defaults", PROFILE_PATH);
        init_default_profile();
        return;
    }
    if (json_buf[0] == '\0') {
        free(json_buf);
        LOG_WARN("Empty profile file at %s, using defaults", PROFILE_PATH);
        init_default_profile();
        return;
    }

    /* Parse JSON */
    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);
    
    if (!root) {
        LOG_WARN("Invalid JSON in profile file %s, using defaults", PROFILE_PATH);
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
    
    /* Try new nested format first: fan_profiles object with active_profile_index inside */
    cJSON *fan_section = cJSON_GetObjectItem(root, "fan_profiles");
    cJSON *profiles_array = NULL;

    if (cJSON_IsObject(fan_section)) {
        /* New nested format */
        cJSON *active_idx = cJSON_GetObjectItem(fan_section, "active_profile_index");
        if (cJSON_IsNumber(active_idx)) {
            active_profile_idx = active_idx->valueint;
        } else {
            active_profile_idx = 0;
        }
        profiles_array = cJSON_GetObjectItem(fan_section, "profiles");
    } else if (cJSON_IsArray(fan_section)) {
        /* Old flat format: fan_profiles was directly an array */
        cJSON *active_idx = cJSON_GetObjectItem(root, "active_profile_index");
        if (cJSON_IsNumber(active_idx)) {
            active_profile_idx = active_idx->valueint;
        } else {
            active_profile_idx = 0;
        }
        profiles_array = fan_section;
    }

    if (!profiles_array || !cJSON_IsArray(profiles_array)) {
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
    if (active_profile_idx < 0 || active_profile_idx >= num_profiles) {
        LOG_WARN("Saved active profile index %d is invalid - using profile 0", active_profile_idx);
        active_profile_idx = 0;
    }
    LOG_INFO("Loaded %d profiles from %s (active: %d)", num_profiles, PROFILE_PATH, active_profile_idx);
}

/* Calculate target fan duty from temperature using active profile.
 * Falls back to the built-in safe curve if the active profile is missing or
 * has no points (never to a fixed low duty). Does not modify the profile. */
static int get_target_duty_from_temp(int temp_c) {
    const TempCurvePoint *src = DEFAULT_CURVE;
    int n = DEFAULT_CURVE_POINTS;

    if (active_profile_idx >= 0 && active_profile_idx < num_profiles &&
        profiles[active_profile_idx].num_points > 0) {
        src = profiles[active_profile_idx].points;
        n = profiles[active_profile_idx].num_points;
    } else {
        static int warned = 0;
        if (!warned) {
            LOG_WARN("Active fan profile %d is missing or empty - using built-in safe curve",
                     active_profile_idx);
            warned = 1;
        }
    }
    if (n > 50) n = 50;

    /* Sort a copy, so point indices seen by clients never change */
    TempCurvePoint points[50];
    memcpy(points, src, sizeof(TempCurvePoint) * (size_t)n);
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (points[j].temperature > points[j + 1].temperature) {
                TempCurvePoint tmp = points[j];
                points[j] = points[j + 1];
                points[j + 1] = tmp;
            }
        }
    }

    int duty = points[n - 1].fan_percent;
    if (temp_c <= points[0].temperature) {
        duty = points[0].fan_percent;
    } else if (temp_c < points[n - 1].temperature) {
        /* Linear interpolation */
        for (int i = 0; i < n - 1; i++) {
            int t1 = points[i].temperature, p1 = points[i].fan_percent;
            int t2 = points[i + 1].temperature, p2 = points[i + 1].fan_percent;
            if (temp_c >= t1 && temp_c < t2) {
                double fraction = (double)(temp_c - t1) / (double)(t2 - t1);
                double percentage = p1 + (p2 - p1) * fraction;
                duty = (int)(percentage + 0.5);
                break;
            }
        }
    }
    return clamp(duty, 0, 100);
}

/* Does this profile have a usable curve? */
static int profile_has_curve(int idx) {
    return idx >= 0 && idx < num_profiles && profiles[idx].num_points > 0;
}

/* Parse command from client */
/* Duty of one fan for STATUS: the EC's value where the EC keeps it, otherwise
 * (EC driven through command 0x99, registers stay 0) what the daemon set. */
static int status_fan_duty(int ec_duty, int rpm, const char *fan) {
    static int warned = 0;
    if (fan_duty_plausible(ec_duty, rpm)) return ec_duty;
    if (!warned) {
        LOG_INFO("EC duty register of the %s fan reads %d at %d RPM - this EC does not report the duty; "
                 "STATUS shows the duty set by the daemon", fan, ec_duty, rpm);
        warned = 1;
    }
    return current_fan_duty;
}

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
    char action[32] = "";
    char arg1[64] = "";
    char arg2[64] = "";
    
    sscanf(cmd_copy, "%31s %63s %63s", action, arg1, arg2);
    
    LOG_DEBUG("Received command: '%s' args='%s' '%s'", action, arg1, arg2);
    
    /* Command: STATUS */
    if (strcmp(action, "STATUS") == 0) {
        /* NVIDIA GPU (1.37.0): values only while it is awake anyway */
        char gpu_status[512];
        GpuInfo gi;
        int gs = gpu_info_get(&gi);
        if (gs == GPU_AWAKE)
            snprintf(gpu_status, sizeof gpu_status,
                     "GPU_STATE=awake\nGPU_NAME=%s\nGPU_POWER_W=%.1f\nGPU_CLOCK_MHZ=%d\nGPU_MEM_CLOCK_MHZ=%d\n"
                     "GPU_UTIL=%d\nGPU_VRAM_USED_MB=%d\nGPU_VRAM_TOTAL_MB=%d\nGPU_PSTATE=%s\n",
                     gi.name, gi.power_w, gi.clock_mhz, gi.mem_clock_mhz, gi.util,
                     gi.vram_used_mb, gi.vram_total_mb, gi.pstate);
        else
            snprintf(gpu_status, sizeof gpu_status, "GPU_STATE=%s\n", gs == GPU_SLEEPING ? "sleeping" : "none");
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
            "CPU_FAN_DUTY=%d\n"
            "GPU_FAN_DUTY=%d\n"
            "AUTO_MODE=%d\n"
            "FAILSAFE=%d\n"
            "EMERGENCY_AUTO=%d\n"
            "EC_ACCESS=%s\n"
            "EC_DIRECT=%s\n"
            "%s%s%s",
            cpu_temp, gpu_temp, cpu_fan_rpm, gpu_fan_rpm, fan_duty,
            status_fan_duty(get_cpu_fan_duty(), cpu_fan_rpm, "CPU"),
            status_fan_duty(get_gpu_fan_duty(), gpu_fan_rpm, "GPU"), auto_mode_enabled,
            override_reason != OVERRIDE_NONE ? 1 : 0, emergency_auto_count,
            ec_backend_name(), ec_direct_allowed() ? "enabled" : "disabled",
            monitor_only ? "EC_REASON=" : "", monitor_only ? ec_init_reason() : "", monitor_only ? "\n" : "");
        strncat(response, gpu_status, resp_size - strlen(response) - 1);
        return 0;
    }
    
    /* Command: SET_MANUAL <percentage> */
    if (strcmp(action, "SET_MANUAL") == 0) {
        if (monitor_only) {
            snprintf(response, resp_size, "ERROR Fan control not available: %s\n", ec_init_reason());
            return -1;
        }
        int pct = atoi(arg1);
        if (pct < 0 || pct > 100) {
            snprintf(response, resp_size, "ERROR Invalid percentage\n");
            return -1;
        }
        
        if (auto_mode_enabled || manual_duty != pct)
            LOG_INFO("Manual mode: fan speed %d%%%s", pct, auto_mode_enabled ? " (auto mode off)" : "");
        auto_mode_enabled = 0;
        manual_duty = pct;

        if (override_reason != OVERRIDE_NONE) {
            /* Safety override wins; the manual value applies once it clears */
            snprintf(response, resp_size, "OK Fan set to %d%% (held at 100%% while fail-safe is active: %s)\n",
                     pct, override_reason_str(override_reason));
            return 0;
        }

        if (apply_fan_duty(pct) != 0) {
            snprintf(response, resp_size, "ERROR Failed to set fan duty (EC not responding)\n");
            return -1;
        }
        snprintf(response, resp_size, "OK Fan set to %d%%\n", pct);
        return 0;
    }
    
    /* Command: SET_AUTO <0|1> */
    if (strcmp(action, "SET_AUTO") == 0) {
        int enable = atoi(arg1) ? 1 : 0;
        if (!enable && auto_mode_enabled) {
            /* Leaving auto mode: hold the current speed instead of jumping
             * to an old manual value */
            manual_duty = clamp(current_fan_duty, 0, 100);
        }
        if (enable != auto_mode_enabled)
            LOG_INFO("Auto mode %s%s", enable ? "on" : "off",
                     enable ? "" : " (manual mode holds the current speed)");
        auto_mode_enabled = enable;
        snprintf(response, resp_size, "OK Auto mode %s\n", 
                 auto_mode_enabled ? "enabled" : "disabled");
        return 0;
    }
    
    /* Command: LIST_PROFILES */
    if (strcmp(action, "LIST_PROFILES") == 0) {
        static const char end_marker[] = "PROFILES_END\n";
        if (resp_size <= sizeof(end_marker)) return -1;
        const size_t limit = resp_size - sizeof(end_marker);

        int n = snprintf(response, limit, "PROFILES_START\n");
        size_t pos = (n < 0) ? 0 : (size_t)n;
        for (int i = 0; i < num_profiles; i++) {
            char line[128];
            char active_marker = (i == active_profile_idx) ? '*' : ' ';
            int len = snprintf(line, sizeof(line),
                     "PROFILE %d [%c] %s\n", i, active_marker, profiles[i].name);
            if (len < 0 || (size_t)len >= sizeof(line) || pos + (size_t)len >= limit) break;
            memcpy(response + pos, line, (size_t)len);
            pos += (size_t)len;
        }
        memcpy(response + pos, end_marker, sizeof(end_marker));
        return 0;
    }
    
    /* Command: ACTIVATE_PROFILE <profile_idx> */
    if (strcmp(action, "ACTIVATE_PROFILE") == 0) {
        int idx = atoi(arg1);
        if (idx < 0 || idx >= num_profiles) {
            snprintf(response, resp_size, "ERROR Invalid profile index\n");
            return -1;
        }
        if (!profile_has_curve(idx)) {
            snprintf(response, resp_size, "ERROR Profile '%s' has no curve points\n", profiles[idx].name);
            return -1;
        }

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
        num_profiles++;
        
        snprintf(response, resp_size, "OK Profile created: %s (index %d)\n", 
                 profile->name, num_profiles - 1);
        return 0;
    }
    
    /* Command: ADD_CURVE_POINT <profile_idx> <temp> <fan_pct> */
    if (strcmp(action, "ADD_CURVE_POINT") == 0) {
        int idx = atoi(arg1);
        int temp = atoi(arg2);
        char arg3[32] = "";
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
        static const char end_marker[] = "PROFILE_END\n";
        /* Space that must always stay free for the end marker + NUL */
        const size_t reserve = sizeof(end_marker);
        if (resp_size <= reserve) {
            return -1;
        }
        const size_t limit = resp_size - reserve;

        int n = snprintf(response, limit,
                 "PROFILE_INFO name=%s points=%d active=%d\n",
                 profile->name, profile->num_points, (idx == active_profile_idx) ? 1 : 0);
        size_t pos = (n < 0) ? 0 : ((size_t)n < limit ? (size_t)n : limit - 1);

        for (int i = 0; i < profile->num_points; i++) {
            char point_str[64];
            int len = snprintf(point_str, sizeof(point_str),
                     "CURVE_POINT temp=%d fan_pct=%d\n",
                     profile->points[i].temperature, profile->points[i].fan_percent);
            if (len < 0 || (size_t)len >= sizeof(point_str)) break;
            /* Only append whole lines; drop the rest if the buffer is full */
            if (pos + (size_t)len >= limit) {
                LOG_WARN("GET_PROFILE %d: response truncated at %d of %d points",
                         idx, i, profile->num_points);
                break;
            }
            memcpy(response + pos, point_str, (size_t)len);
            pos += (size_t)len;
        }

        /* pos < limit, so the marker and its NUL always fit */
        memcpy(response + pos, end_marker, sizeof(end_marker));
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
        char arg3[512] = "";
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
        
        if (!profile_has_curve(idx)) {
            snprintf(response, resp_size, "ERROR Profile '%s' has no curve points\n", profiles[idx].name);
            return -1;
        }

        LOG_INFO("SET_ACTIVE_PROFILE: Activating profile %d (%s)", idx, profiles[idx].name);

        active_profile_idx = idx;

        /* Enable auto mode when a profile is activated */
        auto_mode_enabled = 1;
        LOG_INFO("Auto mode ENABLED for profile: %s", profiles[idx].name);
        
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

        /* Keep active_profile_idx pointing at the same profile, or fall back
         * to "Default" if the active one was deleted */
        if (idx < active_profile_idx) {
            active_profile_idx--;
        } else if (idx == active_profile_idx) {
            int fallback = 0;
            for (int k = 0; k < num_profiles; k++) {
                if (strcmp(profiles[k].name, "Default") == 0) { fallback = k; break; }
            }
            active_profile_idx = fallback;
            LOG_WARN("Active fan profile was deleted - switched to '%s'", profiles[fallback].name);
        }

        snprintf(response, resp_size, "OK Profile deleted\n");
        return 0;
    }

    /* Command: RENAME_PROFILE <idx> <new_name> */
    if (strcmp(action, "RENAME_PROFILE") == 0) {
        int idx = atoi(arg1);
        if (idx < 0 || idx >= num_profiles) {
            snprintf(response, resp_size, "ERROR Invalid profile index\n");
            return -1;
        }

        /* Extract new name from original command to support names with spaces */
        const char *new_name = strstr(cmd_copy, "RENAME_PROFILE");
        if (new_name != NULL) {
            new_name += strlen("RENAME_PROFILE");
            /* Skip whitespace and the index argument */
            while (*new_name == ' ') new_name++;
            while (*new_name && *new_name != ' ') new_name++;
            while (*new_name == ' ') new_name++;
        } else {
            new_name = "";
        }

        if (strlen(new_name) == 0) {
            snprintf(response, resp_size, "ERROR Profile name cannot be empty\n");
            return -1;
        }

        /* Check for duplicate name */
        for (int k = 0; k < num_profiles; k++) {
            if (k != idx && strcmp(profiles[k].name, new_name) == 0) {
                snprintf(response, resp_size, "ERROR A profile with name '%s' already exists\n", new_name);
                return -1;
            }
        }

        snprintf(profiles[idx].name, sizeof(profiles[idx].name), "%s", new_name);
        snprintf(response, resp_size, "OK Profile renamed to '%s'\n", new_name);
        LOG_INFO("Profile %d renamed to '%s'", idx, new_name);
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

static void drop_client(int client_idx, const char *why) {
    int fd = client_fds[client_idx];
    if (fd < 0) return;
    LOG_INFO("Client on fd %d (slot %d) disconnected: %s", fd, client_idx, why);
    close(fd);
    client_fds[client_idx] = -1;
    client_buffer_len[client_idx] = 0;
    if (num_clients > 0) num_clients--;
}

/* Send the whole response without ever blocking the control loop.
 * Responses are far smaller than the socket buffer, so a client that has
 * stopped reading is the only way this fails - that client is dropped. */
static int send_response(int client_fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = send(client_fd, data + sent, len - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;   /* EAGAIN: client not reading; or real error */
        }
        sent += (size_t)w;
    }
    return 0;
}

/* Handle client connection - process ALL available complete commands
 * Returns: 0 if client still connected, 1 if client disconnected */
static int handle_client(int client_fd, int client_idx) {
    char response[RESPONSE_SIZE];

    /* Read whatever is available (socket is non-blocking) */
    int space = BUFFER_SIZE - 1 - client_buffer_len[client_idx];
    if (space <= 0) {
        drop_client(client_idx, "command too long");
        return 1;
    }
    ssize_t bytes_read = recv(client_fd, client_buffers[client_idx] + client_buffer_len[client_idx],
                              (size_t)space, MSG_DONTWAIT);
    if (bytes_read == 0) {
        drop_client(client_idx, "closed by peer");
        return 1;
    }
    if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        drop_client(client_idx, strerror(errno));
        return 1;
    }
    client_buffer_len[client_idx] += (int)bytes_read;

    /* Process every complete line in the buffer */
    char *newline;
    while ((newline = memchr(client_buffers[client_idx], '\n', (size_t)client_buffer_len[client_idx])) != NULL) {
        size_t cmd_len = (size_t)(newline - client_buffers[client_idx]) + 1;  /* incl. newline */

        /* Copy the command out; never write into the shared buffer, which may
         * already hold the start of the next command */
        char cmd[BUFFER_SIZE];
        memcpy(cmd, client_buffers[client_idx], cmd_len);
        cmd[cmd_len] = '\0';

        response[0] = '\0';
        int result = handle_command(cmd, response, sizeof(response));

        /* Only log non-STATUS responses to avoid flooding the log with periodic updates */
        if (strncmp(response, "STATUS OK", 9) != 0) {
            LOG_DEBUG("Sending response: '%s' (result=%d)", response, result);
        }

        size_t remaining = (size_t)client_buffer_len[client_idx] - cmd_len;
        memmove(client_buffers[client_idx], client_buffers[client_idx] + cmd_len, remaining);
        client_buffer_len[client_idx] = (int)remaining;

        if (send_response(client_fd, response, strlen(response)) != 0) {
            drop_client(client_idx, "not reading responses");
            return 1;
        }
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
        LOG_DEBUG("Socket created at %s with permissions: %o", SOCKET_PATH, st.st_mode & 0777);
        LOG_DEBUG("Socket owner: uid=%u, gid=%u", st.st_uid, st.st_gid);
    }
    
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(server_fd);
        unlink(SOCKET_PATH);
        return -1;
    }
    
    return 0;
}

/* One control step: read sensors, apply safety overrides, then the curve.
 * Runs every CONTROL_INTERVAL_SEC in BOTH auto and manual mode, because the
 * safety overrides must also protect manual mode. */
static void fan_control_step(void) {
    static int ring[3] = {-1, -1, -1};
    static int ring_idx = 0, ring_filled = 0;

    if (monitor_only) return;   /* nothing to control, nothing written (1.41.0) */

    /* dcc_fan module (1.36.0): rewrite the duty every step, so its watchdog
     * (fans to 100% without updates) knows the daemon is alive */
    if (fan_keepalive() != 0) LOG_WARN("dcc_fan: keepalive write failed");

    int cpu_temp = get_cpu_temp();
    int gpu_temp = get_gpu_temp();
    int cpu_valid = (cpu_temp >= TEMP_MIN_VALID_C && cpu_temp <= TEMP_MAX_VALID_C);
    int gpu_valid = (gpu_temp >= TEMP_MIN_VALID_C && gpu_temp <= TEMP_MAX_VALID_C);

    int hot_temp = cpu_temp;
    if (gpu_valid && (!cpu_valid || gpu_temp > cpu_temp)) hot_temp = gpu_temp;

    LOG_DEBUG("Control step: CPU=%d GPU=%d valid=%d/%d auto=%d override=%s",
              cpu_temp, gpu_temp, cpu_valid, gpu_valid, auto_mode_enabled,
              override_reason_str(override_reason));

    /* --- 1. Decide whether a safety override is needed --- */
    if (cpu_valid) {
        sensor_fail_count = 0;
    } else if (sensor_fail_count < 1000) {
        sensor_fail_count++;
        LOG_WARN("Invalid CPU temperature reading (%d), %d/%d before fail-safe",
                 cpu_temp, sensor_fail_count, SENSOR_FAIL_LIMIT);
    }

    OverrideReason wanted = OVERRIDE_NONE;
    if (sensor_fail_count >= SENSOR_FAIL_LIMIT) {
        wanted = OVERRIDE_SENSOR_FAILURE;
    } else if (cpu_valid || gpu_valid) {
        int threshold = (override_reason == OVERRIDE_EMERGENCY_TEMP)
                      ? EMERGENCY_TEMP_C - EMERGENCY_HYSTERESIS_C
                      : EMERGENCY_TEMP_C;
        if (hot_temp >= threshold) wanted = OVERRIDE_EMERGENCY_TEMP;
    }

    if (wanted != OVERRIDE_NONE) {
        if (override_reason != wanted) {
            LOG_ERROR("SAFETY OVERRIDE (%s): CPU=%d GPU=%d - holding fans at 100%%",
                      override_reason_str(wanted), cpu_temp, gpu_temp);
            override_reason = wanted;
        }
        if (wanted == OVERRIDE_EMERGENCY_TEMP && !auto_mode_enabled) {
            /* Overheating ends manual mode right away, so the GUI shows it when
             * the fans speed up. Going back to a low manual speed afterwards
             * would just heat up again (seen at 0%: 73 -> 95 degC in ~80 s);
             * the fan curve takes over once the emergency clears. */
            LOG_WARN("Manual mode (%d%%) ended by the emergency temperature - auto mode takes over when it clears",
                     manual_duty);
            auto_mode_enabled = 1;
            emergency_auto_count++;
        }
        if (current_fan_duty != 100) {
            fail_safe_fan_max(override_reason_str(wanted));
        }
        return;
    }

    if (override_reason != OVERRIDE_NONE) {
        LOG_WARN("Safety override (%s) cleared: CPU=%d GPU=%d",
                 override_reason_str(override_reason), cpu_temp, gpu_temp);
        OverrideReason was = override_reason;
        override_reason = OVERRIDE_NONE;
        if (!auto_mode_enabled && was == OVERRIDE_EMERGENCY_TEMP) {
            /* Manual mode set again during the emergency: still hand over */
            LOG_WARN("Manual mode (%d%%) set during the emergency - switching to auto mode",
                     manual_duty);
            auto_mode_enabled = 1;
            emergency_auto_count++;
        } else if (!auto_mode_enabled) {
            apply_fan_duty(manual_duty);   /* sensor recovered: restore the user's manual setting */
        }
        /* auto mode ramps down from 100% through the normal logic below */
    }

    /* A single bad reading (below the limit) holds the current duty */
    if (!cpu_valid) return;

    /* --- 2. Manual mode: just make sure the requested duty is applied --- */
    if (!auto_mode_enabled) {
        if (current_fan_duty != manual_duty) apply_fan_duty(manual_duty);
        return;
    }

    /* --- 3. Auto mode: follow the curve with a smoothed temperature --- */
    ring[ring_idx] = hot_temp;
    ring_idx = (ring_idx + 1) % 3;
    if (ring_filled < 3) ring_filled++;
    int sum = 0;
    for (int j = 0; j < ring_filled; j++) sum += ring[j];
    int avg_temp = sum / ring_filled;

    int target_duty = get_target_duty_from_temp(avg_temp);
    int current = clamp(current_fan_duty, 0, 100);
    int diff = target_duty - current;

    /* every control step (5 s): debug only (1.34.3) */
    LOG_DEBUG("Auto mode: CPU=%d°C GPU=%d°C avg=%d°C target=%d%% current=%d%% diff=%d",
             cpu_temp, gpu_temp, avg_temp, target_duty, current, diff);

    /* Adaptive ramp control to prevent oscillation */
    int new_duty = current;
    if (diff >= 5) {
        if (diff >= 30)      new_duty = target_duty;  /* large: jump to target */
        else if (diff >= 15) new_duty += 15;          /* medium: moderate ramp */
        else                 new_duty += 5;           /* small: gentle ramp */
    } else if (diff <= -5) {
        if (diff <= -30)      new_duty = target_duty;
        else if (diff <= -15) new_duty -= 15;
        else                  new_duty -= 5;
    } else {
        LOG_DEBUG("Fan within ±5%% of target (%d%% vs %d%%), holding steady", current, target_duty);
        return;
    }

    new_duty = clamp(new_duty, 0, 100);
    if (apply_fan_duty(new_duty) == 0) {
        LOG_DEBUG("Setting fan duty to %d%% (target=%d%%, diff=%d)", new_duty, target_duty, diff);   /* routine curve step (1.34.3) */
    }
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
        const char *pid_file = "/run/dcc-fan-daemon.pid";
        FILE *pf = fopen(pid_file, "r");
        if (pf) {
            int old_pid;
            if (fscanf(pf, "%d", &old_pid) == 1) {
                /* Check if old process is still alive */
                if (kill(old_pid, 0) == 0) {
                    fprintf(stderr, "%s: Another instance already running (PID %d). Exiting.\n", PROGRAM_NAME, old_pid); /* Before logging init */
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
        LOG_INFO("Auto mode enabled with profile: %s", profiles[active_profile_idx].name);
    }
    
    /* Set up signal handlers */
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGHUP, on_sighup);
    dcc_ignore_sigpipe();
    
    /* Initialize client buffers */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1;
        client_buffer_len[i] = 0;
        memset(client_buffers[i], 0, BUFFER_SIZE);
    }
    
    /* Initialize EC access */
    int ec_state = ec_init();
    if (ec_state == EC_INIT_DISABLED || ec_state == EC_INIT_IMPLAUSIBLE) {
        monitor_only = 1;
        LOG_WARN("No fan control: %s. Monitoring only; the laptop's firmware controls the fans and "
                 "nothing is written to the EC.", ec_init_reason());
    } else if (ec_state != EC_INIT_OK) {
        LOG_ERROR("Failed to initialize EC access: %s", ec_init_reason());
        return EXIT_FAILURE;
    } else if (ec_backend_module()) {
        LOG_INFO("EC access: dcc_fan kernel module (ACPI EC driver, Clevo WMI)");
    } else {
        LOG_WARN("EC access: direct port I/O (0x62/0x66), enabled by the administrator - only safe on "
                 "laptops with a Clevo EC layout");
    }
    
    /* Create server socket */
    if (create_server_socket() != 0) {
        LOG_ERROR("Failed to create server socket");
        ec_cleanup();
        return EXIT_FAILURE;
    }
    
    LOG_INFO("Server listening on %s", SOCKET_PATH);
    
    /* Set the initial fan speed right away (also applies the fail-safe if the
     * sensors are already broken at startup) */
    if (!auto_mode_enabled) {
        apply_fan_duty(manual_duty);
    }
    fan_control_step();
    if (auto_mode_enabled && override_reason == OVERRIDE_NONE) {
        /* Jump straight to the curve instead of ramping from the default */
        int cpu_temp = get_cpu_temp();
        int gpu_temp = get_gpu_temp();
        int hot_temp = cpu_temp;
        if (gpu_temp >= TEMP_MIN_VALID_C && gpu_temp > cpu_temp) hot_temp = gpu_temp;
        if (cpu_temp >= TEMP_MIN_VALID_C && cpu_temp <= TEMP_MAX_VALID_C) {
            int initial_duty = get_target_duty_from_temp(hot_temp);
            apply_fan_duty(initial_duty);
            LOG_INFO("Initial fan speed set to %d%% based on temp CPU=%d°C GPU=%d°C (profile: %s)",
                     initial_duty, cpu_temp, gpu_temp,
                     profile_has_curve(active_profile_idx) ? profiles[active_profile_idx].name : "built-in");
        }
    }

    /* Tell systemd we're up; WATCHDOG=1 pings follow from the main loop.
     * If the loop ever hangs, systemd (WatchdogSec=) kills and restarts the
     * daemon, and ExecStopPost= sets the fans to 100% in between. */
    sd_notify_msg("READY=1");

    double last_control = monotonic_seconds();
    double last_ping = 0;
    double last_iteration = last_control;

    /* Main event loop */
    while (g_running) {
        double now = monotonic_seconds();

        /* CLOCK_MONOTONIC does not advance during suspend, so this only
         * fires on a real stall (e.g. EC access hanging), never on resume. */
        if (now - last_iteration > LOOP_STALL_WARN_SEC) {
            LOG_ERROR("Main loop stalled for %.1f seconds", now - last_iteration);
        }
        last_iteration = now;

        if (now - last_ping >= SD_WATCHDOG_PING_SEC) {
            sd_notify_msg("WATCHDOG=1");
            last_ping = now;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        int max_fd = server_fd;

        /* Clients can live in any slot, so check all of them */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] >= 0) {
                FD_SET(client_fds[i], &read_fds);
                if (client_fds[i] > max_fd) max_fd = client_fds[i];
            }
        }

        /* Wake up at the latest when the next control step is due */
        double until_control = CONTROL_INTERVAL_SEC - (now - last_control);
        if (until_control < 0) until_control = 0;
        if (until_control > 1.0) until_control = 1.0;
        struct timeval timeout;
        timeout.tv_sec = (time_t)until_control;
        timeout.tv_usec = (suseconds_t)((until_control - (double)timeout.tv_sec) * 1e6);

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (activity < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("select() failed: %s", strerror(errno));
            break;
        }

        if (activity > 0) {
            /* Check for new connections */
            if (FD_ISSET(server_fd, &read_fds)) {
                int client_fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (client_fd >= 0 && !dcc_peer_authorized(client_fd)) {
                    dcc_reject_client(client_fd);
                    client_fd = -1;
                }
                if (client_fd >= 0) {
                    int slot = -1;
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (client_fds[i] < 0) {
                            slot = i;
                            client_fds[i] = client_fd;
                            client_buffer_len[i] = 0;
                            num_clients++;
                            LOG_INFO("New client connected on fd %d (slot %d), total clients: %d",
                                     client_fd, i, num_clients);
                            break;
                        }
                    }
                    if (slot < 0) {
                        /* All slots busy: don't leak the descriptor */
                        LOG_WARN("Too many clients (%d), rejecting fd %d", MAX_CLIENTS, client_fd);
                        close(client_fd);
                    }
                }
            }

            /* Handle existing clients */
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (client_fds[i] >= 0 && FD_ISSET(client_fds[i], &read_fds)) {
                    handle_client(client_fds[i], i);
                }
            }
        }

        /* Time-based control step (not tied to how often clients talk to us) */
        now = monotonic_seconds();
        if (g_running && now - last_control >= CONTROL_INTERVAL_SEC) {
            last_control = now;
            fan_control_step();
        }
    }

    /* Cleanup: fans to 100% before handing control back (any mode) */
    LOG_INFO("Shutting down...");
    sd_notify_msg("STOPPING=1");
    fail_safe_fan_max("daemon shutting down");

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] >= 0) {
            close(client_fds[i]);
        }
    }

    if (server_fd >= 0) {
        close(server_fd);
    }

    remove_old_socket();
    ec_cleanup();

    /* Clean up PID file on normal exit */
    unlink("/run/dcc-fan-daemon.pid");

    LOG_INFO("Daemon stopped.");
    log_cleanup();
    return EXIT_SUCCESS;
}
