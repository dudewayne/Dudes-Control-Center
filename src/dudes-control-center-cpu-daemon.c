/*
 * dudes-control-center-cpu-daemon.c
 * Background daemon for CPU speed/frequency control
 * Part of Dudes Control Center
 * Runs as a systemd service and provides IPC via Unix socket
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
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
#include <stdarg.h>

#include "dudes-control-center-log.h"
#include "dudes-control-center-secure.h"
#include "dudes-control-center-cpu-common.h"

#define PROGRAM_NAME "dudes-control-center-cpu-daemon"
#define SOCKET_PATH "/run/dudes-control-center-cpu.sock"
#define SOCKET_PERMISSIONS 0666
#define BUFFER_SIZE 4096
#define MAX_RESPONSE_LEN 8192
#define MAX_CMD_LEN 2048

/* Auto-switching timer */
#define AUTO_CHECK_INTERVAL_SEC 5

/* Hard limit enforcement timer (re-apply to counter thermald) */
#define HARD_LIMIT_ENFORCE_INTERVAL_SEC 5

/* Global state */
static volatile sig_atomic_t g_running = 1;
static int server_fd = -1;

/* --- Signal handlers --- */
static void on_sigint(int sig) {
    (void)sig;
    LOG_INFO("[DAEMON] Received SIGINT, shutting down...");
    g_running = 0;
}

static void on_sighup(int sig) {
    (void)sig;
    LOG_INFO("[DAEMON] Received SIGHUP, reloading configuration...");
    cpu_profiles_load_all();
    cpu_custom_profile_load_json();
}

static void on_sigterm(int sig) {
    (void)sig;
    LOG_INFO("[DAEMON] Received SIGTERM, shutting down...");
    g_running = 0;
}

/* --- Parse command from client --- */
static void parse_command(const char *cmd, char *action, char *arg1, char *arg2, char *arg3, char *arg4) {
    action[0] = '\0';
    arg1[0] = '\0';
    arg2[0] = '\0';
    arg3[0] = '\0';
    arg4[0] = '\0';

    char cmd_copy[MAX_CMD_LEN];
    strncpy(cmd_copy, cmd, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';

    /* Remove trailing newline */
    size_t len = strlen(cmd_copy);
    if (len > 0 && cmd_copy[len - 1] == '\n') {
        cmd_copy[len - 1] = '\0';
    }

    char *pos = cmd_copy;

    /* Extract action (delimiter is '|' to allow spaces in profile names) */
    char *sep = strchr(pos, '|');
    if (sep) {
        size_t act_len = sep - pos;
        if (act_len > 31) act_len = 31;
        strncpy(action, pos, act_len);
        action[act_len] = '\0';
        pos = sep + 1;
    } else {
        strncpy(action, pos, 31);
        action[31] = '\0';
        return;
    }

    /* Extract arg1 */
    sep = strchr(pos, '|');
    if (sep) {
        size_t arg_len = sep - pos;
        if (arg_len > 255) arg_len = 255;
        strncpy(arg1, pos, arg_len);
        arg1[arg_len] = '\0';
        pos = sep + 1;
    } else {
        strncpy(arg1, pos, 255);
        arg1[255] = '\0';
        return;
    }

    /* Extract arg2 */
    sep = strchr(pos, '|');
    if (sep) {
        size_t arg_len = sep - pos;
        if (arg_len > 255) arg_len = 255;
        strncpy(arg2, pos, arg_len);
        arg2[arg_len] = '\0';
        pos = sep + 1;
    } else {
        strncpy(arg2, pos, 255);
        arg2[255] = '\0';
        return;
    }

    /* Extract arg3 */
    sep = strchr(pos, '|');
    if (sep) {
        size_t arg_len = sep - pos;
        if (arg_len > 63) arg_len = 63;
        strncpy(arg3, pos, arg_len);
        arg3[arg_len] = '\0';
        pos = sep + 1;
    } else {
        strncpy(arg3, pos, 63);
        arg3[63] = '\0';
        return;
    }

    /* Extract arg4 */
    if (*pos != '\0') {
        strncpy(arg4, pos, 1023);
        arg4[1023] = '\0';
    }
}

/* --- Handle client commands --- */
static void handle_command(const char *cmd, char *response, size_t resp_size) {
    char action[32], arg1[256], arg2[256], arg3[64], arg4[1024];

    parse_command(cmd, action, arg1, arg2, arg3, arg4);

    if (arg1[0] != '\0' && arg2[0] != '\0' && arg3[0] != '\0' && arg4[0] != '\0') {
        LOG_INFO("[DAEMON] Received command: %s args=[%s] [%s] [%s] [%s]", action, arg1, arg2, arg3, arg4);
    } else if (arg1[0] != '\0' && arg2[0] != '\0' && arg3[0] != '\0') {
        LOG_INFO("[DAEMON] Received command: %s args=[%s] [%s] [%s]", action, arg1, arg2, arg3);
    } else if (arg1[0] != '\0' && arg2[0] != '\0') {
        LOG_INFO("[DAEMON] Received command: %s args=[%s] [%s]", action, arg1, arg2);
    } else if (arg1[0] != '\0') {
        LOG_INFO("[DAEMON] Received command: %s args=[%s]", action, arg1);
    } else {
        LOG_INFO("[DAEMON] Received command: %s args=", action);
    }

    /* --- STATUS --- */
    if (strcmp(action, "STATUS") == 0) {
        CpuSystemStatus status;
        get_cpu_system_status(&status);

        const CpuProfile *active = cpu_profile_get_active();
        const char *active_name = active ? active->name : "None";

        /* Get min/max freq from first core (all cores share the same range) */
        char freq_min_buf[32], freq_max_buf[32];
        format_frequency(status.cpus[0].min_freq_hz, freq_min_buf, sizeof(freq_min_buf));
        format_frequency(status.cpus[0].max_freq_hz, freq_max_buf, sizeof(freq_max_buf));

        snprintf(response, resp_size,
            "STATUS OK\n"
            "NUM_CPUS=%d\n"
            "ON_AC=%d\n"
            "CPU_TEMP=%d\n"
            "CPU_LOAD=%.1f\n"
            "MIN_FREQ=%s\n"
            "MAX_FREQ=%s\n"
            "ACTIVE_GOVERNOR=%s\n"
            "AUTO_MODE=%d\n",
            status.num_cpus,
            status.on_ac_power,
            status.cpu_temp_c,
            status.cpu_load_percent,
            freq_min_buf,
            freq_max_buf,
            active_name,
            g_cpu_state.auto_mode_enabled);

        /* Add CPU details */
        size_t pos = strlen(response);
        for (int i = 0; i < status.num_cpus && pos < resp_size - 128; i++) {
            char freq_buf[32];
            format_frequency(status.cpus[i].current_freq_hz, freq_buf, sizeof(freq_buf));
            pos += snprintf(response + pos, resp_size - pos,
                "CPU_%d_FREQ=%s\nCPU_%d_GOVERNOR=%s\n",
                i, freq_buf, i, status.cpus[i].governor);
        }
        return;
    }

    /* --- LIST_GOVERNORS --- */
    if (strcmp(action, "LIST_GOVERNORS") == 0) {
        char profiles_buf[4096] = {0};
        cpu_profile_list(profiles_buf, sizeof(profiles_buf));
        snprintf(response, resp_size, "GOVERNORS\n%s", profiles_buf);
        return;
    }

    /* --- APPLY_GOVERNOR <name> --- */
    if (strcmp(action, "APPLY_GOVERNOR") == 0) {
        if (!arg1[0]) {
            snprintf(response, resp_size, "ERROR No governor policy name specified\n");
            return;
        }

        if (cpu_profile_apply(arg1) == 0) {
            snprintf(response, resp_size, "OK Governor Policy '%s' applied successfully\n", arg1);
            LOG_INFO("[DAEMON] Governor Policy '%s' applied successfully", arg1);
        } else {
            snprintf(response, resp_size, "ERROR Failed to apply governor policy '%s'\n", arg1);
        }
        return;
    }

    /* --- CREATE_GOVERNOR <name> <governor> [min_freq] [max_freq] --- */
    if (strcmp(action, "CREATE_GOVERNOR") == 0) {
        if (!arg1[0] || !arg2[0]) {
            snprintf(response, resp_size, "ERROR Usage: CREATE_GOVERNOR <name> <governor> [min_freq] [max_freq]\n");
            return;
        }

        CpuGovernor governor;
        if (governor_to_enum(arg2, &governor) != 0) {
            snprintf(response, resp_size, "ERROR Unknown governor: %s\n", arg2);
            return;
        }

        uint32_t min_freq = arg3[0] ? parse_frequency(arg3) : 0;
        uint32_t max_freq = arg4[0] ? parse_frequency(arg4) : 0;

        LOG_INFO("[DAEMON] Creating governor policy: %s governor=%s min=%uHz max=%uHz",
                 arg1, arg2, min_freq, max_freq);

        if (cpu_profile_create(arg1, governor, min_freq, max_freq) == 0) {
            snprintf(response, resp_size, "OK Governor Policy '%s' created\n", arg1);
        } else {
            snprintf(response, resp_size, "ERROR Failed to create governor policy '%s'\n", arg1);
        }
        return;
    }

    /* --- DELETE_GOVERNOR <name> --- */
    if (strcmp(action, "DELETE_GOVERNOR") == 0) {
        if (!arg1[0]) {
            snprintf(response, resp_size, "ERROR No governor policy name specified\n");
            return;
        }

        if (cpu_profile_delete(arg1) == 0) {
            snprintf(response, resp_size, "OK Governor Policy '%s' deleted\n", arg1);
        } else {
            snprintf(response, resp_size, "ERROR Failed to delete governor policy '%s'\n", arg1);
        }
        return;
    }

    /* --- SET_GOVERNOR <governor> --- */
    if (strcmp(action, "SET_GOVERNOR") == 0) {
        if (!arg1[0]) {
            snprintf(response, resp_size, "ERROR No governor specified\n");
            return;
        }

        /* Validate and resolve governor name (supports abbreviations like perf, sched, pwr) */
        CpuGovernor gov;
        if (governor_to_enum(arg1, &gov) != 0) {
            snprintf(response, resp_size, "ERROR Invalid governor '%s' (use: performance, schedutil, powersave, etc.)\n", arg1);
            return;
        }

        /* Use the resolved full governor name for sysfs writes */
        const char *gov_name = enum_to_governor(gov);

        /* Find matching profile and update internal state */
        int found = -1;
        for (int i = 0; i < g_cpu_state.num_profiles; i++) {
            if (g_cpu_state.profiles[i].governor == gov) {
                found = i;
                break;
            }
        }

        if (found >= 0) {
            g_cpu_state.active_profile_idx = found;
            LOG_INFO("[CPU] Governor changed to '%s' (profile idx=%d)", gov_name, found);
        } else {
            LOG_INFO("[CPU] Governor changed to '%s' (no matching profile)", gov_name);
        }

        if (set_all_cpus_governor(gov_name) == 0) {
            snprintf(response, resp_size, "OK Governor set to '%s'\n", gov_name);
        } else {
            snprintf(response, resp_size, "ERROR Failed to set governor '%s'\n", gov_name);
        }
        return;
    }

    /* --- SET_MIN_FREQ <frequency> --- */
    if (strcmp(action, "SET_MIN_FREQ") == 0) {
        if (!arg1[0]) {
            snprintf(response, resp_size, "ERROR No frequency specified\n");
            return;
        }

        uint32_t freq = parse_frequency(arg1);
        if (set_all_cpus_min_freq(freq) == 0) {
            char freq_buf[32];
            format_frequency(freq, freq_buf, sizeof(freq_buf));
            snprintf(response, resp_size, "OK Min frequency set to %s\n", freq_buf);
        } else {
            snprintf(response, resp_size, "ERROR Failed to set min frequency\n");
        }
        return;
    }

    /* --- SET_MAX_FREQ <frequency> --- */
    if (strcmp(action, "SET_MAX_FREQ") == 0) {
        if (!arg1[0]) {
            snprintf(response, resp_size, "ERROR No frequency specified\n");
            return;
        }

        uint32_t freq = parse_frequency(arg1);
        if (set_all_cpus_max_freq(freq) == 0) {
            char freq_buf[32];
            format_frequency(freq, freq_buf, sizeof(freq_buf));
            snprintf(response, resp_size, "OK Max frequency set to %s\n", freq_buf);
        } else {
            snprintf(response, resp_size, "ERROR Failed to set max frequency\n");
        }
        return;
    }

    /* --- GET_CPU_INFO [cpu_id] --- */
    if (strcmp(action, "GET_CPU_INFO") == 0) {
        int cpu_id = arg1[0] ? atoi(arg1) : 0;
        const CpuInfo *info = get_cpu_info(cpu_id);

        if (!info) {
            snprintf(response, resp_size, "ERROR CPU %d not found\n", cpu_id);
            return;
        }

        char freq_min[32], freq_max[32], freq_curr[32];
        format_frequency(info->min_freq_hz, freq_min, sizeof(freq_min));
        format_frequency(info->max_freq_hz, freq_max, sizeof(freq_max));
        format_frequency(info->current_freq_hz, freq_curr, sizeof(freq_curr));

        snprintf(response, resp_size,
            "CPU_INFO OK\n"
            "ID=%d\n"
            "ONLINE=%d\n"
            "GOVERNOR=%s\n"
            "MIN_FREQ=%s\n"
            "MAX_FREQ=%s\n"
            "CURRENT_FREQ=%s\n",
            info->cpu_id,
            info->online,
            info->governor,
            freq_min,
            freq_max,
            freq_curr);
        return;
    }

    /* --- GET_AVAILABLE_GOVERNORS --- */
    if (strcmp(action, "GET_AVAILABLE_GOVERNORS") == 0) {
        /* get_all_governors() may fill up to MAX_CPUS entries of CPU_GOVERNOR_NAME_MAX bytes */
        char governor_storage[MAX_CPUS][CPU_GOVERNOR_NAME_MAX];
        char *governors[MAX_CPUS];
        int count = 0;

        for (int i = 0; i < MAX_CPUS; i++) governors[i] = governor_storage[i];

        if (get_all_governors(governors, &count) == 0) {
            snprintf(response, resp_size, "GOVERNORS OK\nCOUNT=%d\n", count);

            size_t pos = strlen(response);
            for (int i = 0; i < count && pos + CPU_GOVERNOR_NAME_MAX + 2 < resp_size; i++) {
                pos += snprintf(response + pos, resp_size - pos, "%s\n", governors[i]);
            }
        } else {
            snprintf(response, resp_size, "ERROR Failed to get available governors\n");
        }
        return;
    }

    /* --- TOGGLE_AUTO_MODE --- */
    if (strcmp(action, "TOGGLE_AUTO_MODE") == 0) {
        g_cpu_state.auto_mode_enabled = !g_cpu_state.auto_mode_enabled;
        LOG_INFO("[DAEMON] Auto mode %s", g_cpu_state.auto_mode_enabled ? "ENABLED" : "DISABLED");
        snprintf(response, resp_size, "OK AUTO_MODE=%d\n", g_cpu_state.auto_mode_enabled);
        return;
    }

    /* --- SET_AUTO_CONDITION <policy_name> <condition_type> [threshold] --- */
    if (strcmp(action, "SET_AUTO_CONDITION") == 0) {
        if (!arg1[0] || !arg2[0]) {
            snprintf(response, resp_size, "ERROR Usage: SET_AUTO_CONDITION <policy> <condition> [threshold]\n");
            return;
        }

        ConditionType condition = (ConditionType)atoi(arg2);
        int threshold = arg3[0] ? atoi(arg3) : 0;

        if (cpu_profile_set_condition(arg1, condition, threshold) == 0) {
            snprintf(response, resp_size, "OK Condition set for policy '%s'\n", arg1);
        } else {
            snprintf(response, resp_size, "ERROR Failed to set condition\n");
        }
        return;
    }

    /* --- CLEAR_AUTO_CONDITION <policy_name> --- */
    if (strcmp(action, "CLEAR_AUTO_CONDITION") == 0) {
        if (!arg1[0]) {
            snprintf(response, resp_size, "ERROR No governor policy name specified\n");
            return;
        }

        cpu_profile_clear_condition(arg1);
        snprintf(response, resp_size, "OK Condition cleared for policy '%s'\n", arg1);
        return;
    }

    /* --- LIST_CUSTOM_PROFILES --- */
    if (strcmp(action, "LIST_CUSTOM_PROFILES") == 0) {
        char profiles_buf[4096] = {0};
        cpu_custom_profile_list(profiles_buf, sizeof(profiles_buf));
        snprintf(response, resp_size, "%s", profiles_buf[0] ? profiles_buf : "No custom profiles saved\n");
        return;
    }

    /* --- LIST_CUSTOM_PROFILES_JSON --- */
    if (strcmp(action, "LIST_CUSTOM_PROFILES_JSON") == 0) {
        char *json_str = cpu_custom_profile_list_json();
        if (json_str) {
            snprintf(response, resp_size, "%s", json_str);
            free(json_str);
        } else {
            snprintf(response, resp_size, "[]");
        }
        return;
    }

    /* --- CREATE_CUSTOM_PROFILE <name> <governor> <default_max_freq_hz> <apply_all_cores> --- */
    if (strcmp(action, "CREATE_CUSTOM_PROFILE") == 0) {
        if (!arg1[0] || !arg2[0]) {
            snprintf(response, resp_size, "ERROR Usage: CREATE_CUSTOM_PROFILE <name> <governor> [max_freq_hz] [apply_all_cores]\n");
            return;
        }

        CpuGovernor gov = CPU_GOVERNOR_PERFORMANCE;
        governor_to_enum(arg2, &gov);

        uint32_t max_freq = get_hard_max_freq_hz();
        if (arg3[0]) {
            max_freq = (uint32_t)strtoul(arg3, NULL, 10);
        }

        int apply_all = 1;
        if (arg4[0]) {
            apply_all = atoi(arg4);
        }

        if (cpu_custom_profile_create(arg1, gov, max_freq, apply_all) == 0) {
            snprintf(response, resp_size, "OK Custom profile '%s' created\n", arg1);
        } else {
            snprintf(response, resp_size, "ERROR Failed to create custom profile '%s'\n", arg1);
        }
        return;
    }

    /* --- APPLY_CUSTOM_PROFILE <index> --- */
    if (strcmp(action, "APPLY_CUSTOM_PROFILE") == 0) {
        if (!arg1[0]) {
            snprintf(response, resp_size, "ERROR No custom profile index specified\n");
            return;
        }

        int idx = atoi(arg1);
        if (cpu_custom_profile_apply(idx) == 0) {
            snprintf(response, resp_size, "OK Custom profile '%s' applied\n",
                     g_cpu_state.custom_profiles[idx].name);
        } else {
            snprintf(response, resp_size, "ERROR Failed to apply custom profile index %d\n", idx);
        }
        return;
    }

    /* --- DELETE_CUSTOM_PROFILE <index> --- */
    if (strcmp(action, "DELETE_CUSTOM_PROFILE") == 0) {
        if (!arg1[0]) {
            snprintf(response, resp_size, "ERROR No custom profile index specified\n");
            return;
        }

        int idx = atoi(arg1);
        if (cpu_custom_profile_delete(idx) == 0) {
            snprintf(response, resp_size, "OK Custom profile deleted\n");
        } else {
            snprintf(response, resp_size, "ERROR Failed to delete custom profile index %d\n", idx);
        }
        return;
    }

    /* --- RENAME_CUSTOM_PROFILE <index> <new_name> --- */
    if (strcmp(action, "RENAME_CUSTOM_PROFILE") == 0) {
        if (!arg1[0] || !arg2[0]) {
            snprintf(response, resp_size, "ERROR Usage: RENAME_CUSTOM_PROFILE <index> <new_name>\n");
            return;
        }

        int idx = atoi(arg1);
        if (cpu_custom_profile_rename(idx, arg2) == 0) {
            snprintf(response, resp_size, "OK Custom profile renamed to '%s'\n", arg2);
        } else {
            snprintf(response, resp_size, "ERROR Failed to rename custom profile (name may already exist)\n");
        }
        return;
    }

    /* --- SAVE_CUSTOM_PROFILES --- */
    if (strcmp(action, "SAVE_CUSTOM_PROFILES") == 0) {
        snprintf(response, resp_size, "OK Custom profiles saved (by GUI)\n");
        return;
    }

    /* --- SET_CORE_FREQ <profile_idx> <cpu_id> <max_freq_hz> --- */
    if (strcmp(action, "SET_CORE_FREQ") == 0) {
        if (!arg1[0] || !arg2[0] || !arg3[0]) {
            snprintf(response, resp_size, "ERROR Usage: SET_CORE_FREQ <profile_idx> <cpu_id> <max_freq_hz>\n");
            return;
        }

        int profile_idx = atoi(arg1);
        int cpu_id = atoi(arg2);
        uint32_t max_freq = (uint32_t)strtoul(arg3, NULL, 10);

        /* Read current min freq so we don't overwrite it */
        uint32_t current_min = 0;
        const CpuCustomProfile *tmp = cpu_custom_profile_get(profile_idx);
        if (tmp && cpu_id >= 0 && cpu_id < tmp->num_cores && cpu_id < MAX_CPUS) {
            current_min = tmp->per_core_min_freq_hz[cpu_id];
        }

        if (cpu_custom_profile_set_core_freq(profile_idx, cpu_id, max_freq, current_min) == 0) {
            snprintf(response, resp_size, "OK Core %d max frequency set to %u Hz\n", cpu_id, max_freq);
        } else {
            snprintf(response, resp_size, "ERROR Failed to set core frequency\n");
        }
        return;
    }

    /* --- SET_CORE_MIN_FREQ <profile_idx> <cpu_id> <min_freq_hz> --- */
    if (strcmp(action, "SET_CORE_MIN_FREQ") == 0) {
        if (!arg1[0] || !arg2[0] || !arg3[0]) {
            snprintf(response, resp_size, "ERROR Usage: SET_CORE_MIN_FREQ <profile_idx> <cpu_id> <min_freq_hz>\n");
            return;
        }

        int profile_idx = atoi(arg1);
        int cpu_id = atoi(arg2);
        uint32_t min_freq = (uint32_t)strtoul(arg3, NULL, 10);

        /* Read current max freq so we don't overwrite it */
        uint32_t current_max = 0;
        const CpuCustomProfile *tmp2 = cpu_custom_profile_get(profile_idx);
        if (tmp2 && cpu_id >= 0 && cpu_id < tmp2->num_cores && cpu_id < MAX_CPUS) {
            current_max = tmp2->per_core_max_freq_hz[cpu_id];
        }

        if (cpu_custom_profile_set_core_freq(profile_idx, cpu_id, current_max, min_freq) == 0) {
            snprintf(response, resp_size, "OK Core %d min frequency set to %u Hz\n", cpu_id, min_freq);
        } else {
            snprintf(response, resp_size, "ERROR Failed to set core min frequency\n");
        }
        return;
    }

    /* --- SET_CORE_MAX_FREQ <profile_idx> <cpu_id> <max_freq_hz> --- */
    if (strcmp(action, "SET_CORE_MAX_FREQ") == 0) {
        if (!arg1[0] || !arg2[0] || !arg3[0]) {
            snprintf(response, resp_size, "ERROR Usage: SET_CORE_MAX_FREQ <profile_idx> <cpu_id> <max_freq_hz>\n");
            return;
        }

        int profile_idx = atoi(arg1);
        int cpu_id = atoi(arg2);
        uint32_t max_freq = (uint32_t)strtoul(arg3, NULL, 10);

        /* Read current min freq so we don't overwrite it */
        uint32_t current_min = 0;
        const CpuCustomProfile *tmp3 = cpu_custom_profile_get(profile_idx);
        if (tmp3 && cpu_id >= 0 && cpu_id < tmp3->num_cores && cpu_id < MAX_CPUS) {
            current_min = tmp3->per_core_min_freq_hz[cpu_id];
        }

        if (cpu_custom_profile_set_core_freq(profile_idx, cpu_id, max_freq, current_min) == 0) {
            snprintf(response, resp_size, "OK Core %d max frequency set to %u Hz\n", cpu_id, max_freq);
        } else {
            snprintf(response, resp_size, "ERROR Failed to set core max frequency\n");
        }
        return;
    }

    /* --- SET_APPLY_ALL_CORES <profile_idx> <value> --- */
    if (strcmp(action, "SET_APPLY_ALL_CORES") == 0) {
        if (!arg1[0] || !arg2[0]) {
            snprintf(response, resp_size, "ERROR Usage: SET_APPLY_ALL_CORES <profile_idx> <value>\n");
            return;
        }

        int profile_idx = atoi(arg1);
        int value = atoi(arg2);

        if (profile_idx < 0 || profile_idx >= g_cpu_state.num_custom_profiles) {
            snprintf(response, resp_size, "ERROR Invalid profile index %d\n", profile_idx);
            return;
        }

        g_cpu_state.custom_profiles[profile_idx].apply_all_cores = value;
        snprintf(response, resp_size, "OK apply_all_cores set to %d for profile %d\n", value, profile_idx);
        return;
    }

    /* --- SET_PROFILE_GOVERNOR <profile_idx> <governor> --- */
    if (strcmp(action, "SET_PROFILE_GOVERNOR") == 0) {
        if (!arg1[0] || !arg2[0]) {
            snprintf(response, resp_size, "ERROR Usage: SET_PROFILE_GOVERNOR <profile_idx> <governor>\n");
            return;
        }

        int profile_idx = atoi(arg1);
        CpuGovernor gov;

        if (profile_idx < 0 || profile_idx >= g_cpu_state.num_custom_profiles) {
            snprintf(response, resp_size, "ERROR Invalid profile index %d\n", profile_idx);
            return;
        }

        if (governor_to_enum(arg2, &gov) != 0) {
            snprintf(response, resp_size, "ERROR Unknown governor: %s\n", arg2);
            return;
        }

        g_cpu_state.custom_profiles[profile_idx].governor = gov;
        snprintf(response, resp_size, "OK governor set to %s for profile %d\n", arg2, profile_idx);
        return;
    }

    /* --- GET_CUSTOM_PROFILE_INFO <index> --- */
    if (strcmp(action, "GET_CUSTOM_PROFILE_INFO") == 0) {
        if (!arg1[0]) {
            snprintf(response, resp_size, "ERROR No custom profile index specified\n");
            return;
        }

        int idx = atoi(arg1);
        const CpuCustomProfile *cp = cpu_custom_profile_get(idx);
        if (!cp) {
            snprintf(response, resp_size, "ERROR Custom profile index %d not found\n", idx);
            return;
        }

        size_t pos = 0;
        pos += snprintf(response + pos, resp_size - pos, "NAME:%s\n", cp->name);
        pos += snprintf(response + pos, resp_size - pos, "GOVERNOR:%s\n", enum_to_governor(cp->governor));
        pos += snprintf(response + pos, resp_size - pos, "APPLY_ALL:%d\n", cp->apply_all_cores);
        pos += snprintf(response + pos, resp_size - pos, "NUM_CORES:%d\n", cp->num_cores);

        for (int c = 0; c < cp->num_cores && c < MAX_CPUS && pos < resp_size - 128; c++) {
            pos += snprintf(response + pos, resp_size - pos, "CORE_%d_MAX:%u\n", c, cp->per_core_max_freq_hz[c]);
            pos += snprintf(response + pos, resp_size - pos, "CORE_%d_MIN:%u\n", c, cp->per_core_min_freq_hz[c]);
        }
        return;
    }

    /* --- GET_HARD_LIMIT --- */
    if (strcmp(action, "GET_HARD_LIMIT") == 0) {
        snprintf(response, resp_size, "OK HARD_LIMIT=%u\n", get_hard_max_freq_hz());
        return;
    }

    /* --- SET_HARD_LIMIT <hz> --- */
    if (strcmp(action, "SET_HARD_LIMIT") == 0) {
        if (!arg1[0]) {
            snprintf(response, resp_size, "ERROR Usage: SET_HARD_LIMIT <hz>\n");
            return;
        }

        uint32_t new_limit = (uint32_t)atol(arg1);
        if (new_limit == 0) {
            snprintf(response, resp_size, "ERROR Invalid hard limit value: %s\n", arg1);
            return;
        }

        set_hard_max_freq_hz(new_limit);
        cpu_custom_profile_save_json(); /* Persist hard limit to profiles.json */
        clamp_cpus_to_hard_limit();
        set_all_cpus_epp("performance");
        snprintf(response, resp_size, "OK Hard limit set to %u Hz (%.2f GHz)\n", new_limit, new_limit / 1e9);
        LOG_INFO("[DAEMON] Hard limit changed to %u Hz (%.2f GHz)", new_limit, new_limit / 1e9);
        return;
    }

    /* --- SAVE_CUSTOM_PROFILE (batch save) --- *
     * Format: SAVE_CUSTOM_PROFILE|<profile_idx>|<governor>|<apply_all>|<core0_min>,<core0_max>,<core1_min>,<core1_max>,...
     * arg4 contains comma-separated core data to fit in a single round-trip. */
    if (strcmp(action, "SAVE_CUSTOM_PROFILE") == 0) {
        if (!arg1[0] || !arg2[0]) {
            snprintf(response, resp_size, "ERROR Usage: SAVE_CUSTOM_PROFILE <idx> <governor> <apply_all> <core_data>\n");
            return;
        }

        int profile_idx = atoi(arg1);
        if (profile_idx < 0 || profile_idx >= g_cpu_state.num_custom_profiles) {
            snprintf(response, resp_size, "ERROR Invalid profile index %d\n", profile_idx);
            return;
        }

        /* Set governor */
        CpuGovernor gov;
        if (governor_to_enum(arg2, &gov) == 0) {
            g_cpu_state.custom_profiles[profile_idx].governor = gov;
        }

        /* Set apply_all_cores */
        int apply_all = atoi(arg3);
        g_cpu_state.custom_profiles[profile_idx].apply_all_cores = apply_all;

        /* Parse comma-separated core data: min0,max0,min1,max1,... */
        int cores_set = 0;
        if (arg4[0]) {
            char core_data[1024];
            strncpy(core_data, arg4, sizeof(core_data) - 1);
            core_data[sizeof(core_data) - 1] = '\0';

            char *save_ptr = NULL;
            char *token = strtok_r(core_data, ",", &save_ptr);
            int core_id = 0;
            int value_in_pair = 0; /* 0 = min, 1 = max */
            uint32_t min_freq = 0, max_freq = 0;

            while (token != NULL && core_id < MAX_CPUS) {
                uint64_t val = strtoull(token, NULL, 10);

                if (value_in_pair == 0) {
                    min_freq = (uint32_t)val;
                } else {
                    max_freq = (uint32_t)val;
                    /* Apply both values using the existing per-core setter */
                    cpu_custom_profile_set_core_freq(profile_idx, core_id, max_freq, min_freq);
                    cores_set++;
                    core_id++;
                }
                value_in_pair = 1 - value_in_pair;
                token = strtok_r(NULL, ",", &save_ptr);
            }
        }

        snprintf(response, resp_size, "OK Batch saved profile %d (%d cores)\n", profile_idx, cores_set);
        LOG_INFO("[DAEMON] Batch save completed for profile %d, %d cores set", profile_idx, cores_set);
        return;
    }

    /* --- PING --- */
    if (strcmp(action, "PING") == 0) {
        snprintf(response, resp_size, "PONG\n");
        return;
    }

    /* Unknown command */
    snprintf(response, resp_size, "ERROR Unknown command: %s\n", action);
    LOG_ERROR("[DAEMON] Unknown command received: %s", action);
}

/* --- Check conditions for automatic policy switching --- */
static void check_auto_conditions(void) {
    if (!g_cpu_state.auto_mode_enabled) return;

    int on_ac = is_on_ac_power();
    int temp = get_cpu_temperature();
    double load = get_cpu_load_percent();

    for (int i = 0; i < g_cpu_state.num_profiles; i++) {
        CpuProfile *profile = &g_cpu_state.profiles[i];

        if (!profile->is_auto_switchable) continue;

        int condition_met = 0;

        switch (profile->condition_type) {
            case CONDITION_AC_POWER:
                condition_met = (on_ac == 1);
                break;
            case CONDITION_BATTERY:
                condition_met = (on_ac == 0);
                break;
            case CONDITION_TEMP_HIGH:
                if (temp > 0) condition_met = (temp >= profile->condition_threshold);
                break;
            case CONDITION_TEMP_LOW:
                if (temp > 0) condition_met = (temp < profile->condition_threshold);
                break;
            case CONDITION_LOAD_HIGH:
                if (load >= 0) condition_met = (load >= profile->condition_threshold);
                break;
            case CONDITION_LOAD_LOW:
                if (load >= 0) condition_met = (load < profile->condition_threshold);
                break;
            default:
                condition_met = 0;
                break;
        }

        if (condition_met && g_cpu_state.active_profile_idx != i) {
            LOG_INFO("[DAEMON] Auto-switching to policy '%s' (condition: %s)",
                     profile->name, enum_to_governor(profile->governor));
            cpu_profile_apply(profile->name);
            break;
        }
    }
}

/* --- Handle client connection (non-blocking) --- */
static int g_client_fd = -1;

static void handle_client_nonblocking(int client_fd) {
    /* Set client socket to non-blocking */
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    g_client_fd = client_fd;
    LOG_INFO("[DAEMON] Client connected (non-blocking)");
}

/* Process pending client data (call from main loop) */
static void process_client(void) {
    if (g_client_fd < 0) return;

    char buffer[BUFFER_SIZE] = {0};
    char response[MAX_RESPONSE_LEN] = {0};

    memset(buffer, 0, sizeof(buffer));
    ssize_t bytes_read = read(g_client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read == 0) {
        LOG_INFO("[DAEMON] Client disconnected");
        close(g_client_fd);
        g_client_fd = -1;
        return;
    } else if (bytes_read < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("[DAEMON] Client read error: %s", strerror(errno));
            close(g_client_fd);
            g_client_fd = -1;
        }
        return; /* No data available, that's OK */
    }

    buffer[bytes_read] = '\0';
    memset(response, 0, sizeof(response));
    handle_command(buffer, response, sizeof(response));

    size_t total_len = strlen(response);
    size_t total_sent = 0;
    while (total_sent < total_len) {
        ssize_t bytes_written = write(g_client_fd, response + total_sent, total_len - total_sent);
        if (bytes_written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000); /* 1ms wait for non-blocking socket */
                continue;
            }
            LOG_ERROR("[DAEMON] Error writing to client: %s", strerror(errno));
            close(g_client_fd);
            g_client_fd = -1;
            return;
        }
        total_sent += bytes_written;
    }
}

/* --- Create Unix socket server --- */
static int create_socket_server(void) {
    unlink(SOCKET_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_ERROR("[DAEMON] Failed to create socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("[DAEMON] Failed to bind socket: %s", strerror(errno));
        close(server_fd);
        return -1;
    }

    chmod(SOCKET_PATH, SOCKET_PERMISSIONS);

    if (listen(server_fd, SOMAXCONN) < 0) {
        LOG_ERROR("[DAEMON] Failed to listen on socket: %s", strerror(errno));
        close(server_fd);
        return -1;
    }

    LOG_INFO("[DAEMON] Socket server listening on %s", SOCKET_PATH);
    return 0;
}

/* --- Main function --- */
int main(int argc, char *argv[]) {
    int foreground = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--foreground") == 0 || strcmp(argv[i], "-f") == 0) {
            foreground = 1;
        }
    }

    /* Initialize logging */
    if (log_init("CPU_DAEMON", LOG_LEVEL_INFO, "/var/log/dudes-control-center") != 0) {
        fprintf(stderr, "Failed to initialize logging system\n");
    }

    LOG_INFO("[DAEMON] Starting %s", PROGRAM_NAME);
    LOG_INFO("[DAEMON] Foreground mode: %s", foreground ? "yes" : "no");

    /* Initialize CPU control subsystem */
    if (cpu_control_init() != 0) {
        LOG_ERROR("[DAEMON] Failed to initialize CPU control subsystem");
        return 1;
    }

    LOG_INFO("[DAEMON] CPU control initialized, loaded %d governor policies", g_cpu_state.num_profiles);

    /* Load custom profiles from JSON */
    cpu_custom_profile_load_json();
    LOG_INFO("[DAEMON] Loaded %d custom profiles", g_cpu_state.num_custom_profiles);

    /* Set up signal handlers */
    signal(SIGINT, on_sigint);
    signal(SIGHUP, on_sighup);
    dcc_ignore_sigpipe();
    signal(SIGTERM, on_sigterm);

    /* Create socket server */
    if (create_socket_server() != 0) {
        LOG_ERROR("[DAEMON] Failed to create socket server");
        return 1;
    }

    /* Set non-blocking mode for server socket */
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    LOG_INFO("[DAEMON] Daemon ready, waiting for connections...");

    /* Main loop */
    while (g_running) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd >= 0 && !dcc_peer_authorized(client_fd)) {
            /* Reject before touching the current client connection */
            dcc_reject_client(client_fd);
        } else if (client_fd >= 0) {
            handle_client_nonblocking(client_fd);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            if (g_running) {
                LOG_ERROR("[DAEMON] Accept error: %s", strerror(errno));
            }
        }

        /* Process any pending client commands */
        process_client();

        /* Check auto-switching conditions periodically */
        time_t current_time = time(NULL);
        static time_t last_check_time = 0;
        if (current_time - last_check_time >= AUTO_CHECK_INTERVAL_SEC) {
            check_auto_conditions();
            last_check_time = current_time;
        }

        /* Enforce hard limit and min freq periodically to counter thermald/kernel */
        static time_t last_enforce_time = 0;
        if (current_time - last_enforce_time >= HARD_LIMIT_ENFORCE_INTERVAL_SEC) {
            uint32_t hard_limit = get_hard_max_freq_hz();
            int cpu_count = get_cpu_count();

            /* Get active custom profile for min freq enforcement */
            CpuCustomProfile *active_profile = NULL;
            if (g_cpu_state.active_custom_profile_idx >= 0 &&
                g_cpu_state.active_custom_profile_idx < g_cpu_state.num_custom_profiles) {
                active_profile = &g_cpu_state.custom_profiles[g_cpu_state.active_custom_profile_idx];
            }

            int max_restored = 0;
            int min_restored = 0;
            for (int i = 0; i < cpu_count; i++) {
                /* Enforce hard limit on max freq */
                uint32_t current_max = get_cpu_max_freq(i);
                if (current_max < hard_limit) {
                    set_cpu_max_freq(i, hard_limit);
                    max_restored++;
                }

                /* Enforce min freq from active custom profile */
                if (active_profile != NULL) {
                    uint32_t profile_min = active_profile->per_core_min_freq_hz[i];
                    if (profile_min > 0) {
                        uint32_t current_min = get_cpu_min_freq(i);
                        if (current_min < profile_min) {
                            set_cpu_min_freq(i, profile_min);
                            min_restored++;
                        }
                    }
                }
            }
            LOG_INFO("[ENFORCE] max_restored=%d, min_restored=%d, active_profile_idx=%d",
                     max_restored, min_restored, g_cpu_state.active_custom_profile_idx);
            last_enforce_time = current_time;
        }

        usleep(100000);  /* 100ms */
    }

    /* Cleanup */
    LOG_INFO("[DAEMON] Shutting down...");

    close(server_fd);
    unlink(SOCKET_PATH);

    cpu_control_cleanup();

    LOG_INFO("[DAEMON] Daemon exited cleanly");
    log_cleanup();

    return 0;
}
