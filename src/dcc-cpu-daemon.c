// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc-cpu-daemon.c
 * Background daemon for CPU speed/frequency control
 * Part of Dudes Control Center
 * Runs as a systemd service and provides IPC via Unix socket
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

#include "dcc-log.h"
#include "dcc-secure.h"
#include "dcc-cpu-common.h"

#define PROGRAM_NAME "dcc-cpu-daemon"
#ifndef SOCKET_PATH   /* overridable for tests */
#define SOCKET_PATH "/run/dcc-cpu.sock"
#endif
#ifndef CPU_LOG_DIR
#define CPU_LOG_DIR "/var/log/dudes-control-center"
#endif
#define SOCKET_PERMISSIONS 0666
#define MAX_RESPONSE_LEN 16384
#define MAX_CMD_LEN 8192          /* SAVE_CUSTOM_PROFILE carries 2 values per core */
#define ARG4_LEN 4096
#define MAX_CLIENTS 10
#define CLIENT_BUF_SIZE MAX_CMD_LEN
#define UNTERMINATED_CMD_MS 200   /* treat a command without newline as complete after this idle time */

/* Auto-switching timer */
#define AUTO_CHECK_INTERVAL_SEC 5

/* Hard limit enforcement timer (re-apply to counter thermald) */
#define HARD_LIMIT_ENFORCE_INTERVAL_SEC 5

/* Global state */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload = 0;
static int server_fd = -1;

/* --- Signal handlers --- */
/* Signal handlers only set flags; the main loop does the work */
static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void on_sighup(int sig) {
    (void)sig;
    g_reload = 1;
}

static void on_sigterm(int sig) {
    (void)sig;
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
        size_t act_len = (size_t)(sep - pos);
        if (act_len > 31) act_len = 31;
        memcpy(action, pos, act_len);
        action[act_len] = '\0';
        pos = sep + 1;
    } else {
        snprintf(action, 32, "%.31s", pos);   /* command word, at most 31 chars */
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
        strncpy(arg4, pos, ARG4_LEN - 1);
        arg4[ARG4_LEN - 1] = '\0';
    }
}

/* Commands that only read state (1.34.3: logged at DEBUG, not INFO) */
static int cpu_command_is_query(const char *action) {
    return strcmp(action, "STATUS") == 0 || strcmp(action, "PING") == 0 ||
           strncmp(action, "GET_", 4) == 0 || strncmp(action, "LIST_", 5) == 0;
}

/* --- Handle client commands --- */
static void handle_command(const char *cmd, char *response, size_t resp_size) {
    char action[32], arg1[256], arg2[256], arg3[64], arg4[ARG4_LEN];

    parse_command(cmd, action, arg1, arg2, arg3, arg4);

    /* Read-only commands (the GUI asks for STATUS every 5 s) are logged only
     * at DEBUG; commands that change something stay at INFO (1.34.3) */
    LogLevel lvl = cpu_command_is_query(action) ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO;
    if (log_should_output(lvl)) {
        if (arg1[0] != '\0' && arg2[0] != '\0' && arg3[0] != '\0' && arg4[0] != '\0')
            log_write(lvl, "[DAEMON] Received command: %s args=[%s] [%s] [%s] [%s]", action, arg1, arg2, arg3, arg4);
        else if (arg1[0] != '\0' && arg2[0] != '\0' && arg3[0] != '\0')
            log_write(lvl, "[DAEMON] Received command: %s args=[%s] [%s] [%s]", action, arg1, arg2, arg3);
        else if (arg1[0] != '\0' && arg2[0] != '\0')
            log_write(lvl, "[DAEMON] Received command: %s args=[%s] [%s]", action, arg1, arg2);
        else if (arg1[0] != '\0')
            log_write(lvl, "[DAEMON] Received command: %s args=[%s]", action, arg1);
        else
            log_write(lvl, "[DAEMON] Received command: %s args=", action);
    }

    /* --- SET_LOG_LEVEL|<ERROR|WARN|INFO|DEBUG>, GET_LOG_LEVEL (1.34.3) --- */
    if (strcmp(action, "SET_LOG_LEVEL") == 0) {
        static const struct { const char *name; LogLevel level; } levels[] = {
            { "ERROR", LOG_LEVEL_ERROR }, { "WARN", LOG_LEVEL_WARN },
            { "INFO", LOG_LEVEL_INFO }, { "DEBUG", LOG_LEVEL_DEBUG } };
        for (size_t i = 0; i < sizeof levels / sizeof levels[0]; i++) {
            if (strcasecmp(arg1, levels[i].name) == 0) {
                log_set_level(levels[i].level);
                LOG_INFO("[DAEMON] Log level set to %s", levels[i].name);
                snprintf(response, resp_size, "OK Log level set to %s\n", levels[i].name);
                return;
            }
        }
        snprintf(response, resp_size, "ERROR Invalid log level: %s (use ERROR, WARN, INFO, DEBUG)\n", arg1);
        return;
    }
    if (strcmp(action, "GET_LOG_LEVEL") == 0) {
        snprintf(response, resp_size, "LOG_LEVEL=%s\n", log_level_to_string(log_get_level()));
        return;
    }

    /* --- STATUS --- */
    if (strcmp(action, "STATUS") == 0) {
        CpuSystemStatus status;
        get_cpu_system_status(&status);

        /* The mode the CPU really runs (read from sysfs). It used to be the
         * daemon's internal default mode, which stayed "Balanced" whatever
         * profile or mode was applied. */
        char cur_gov[64], cur_epp[64];
        const char *active_name = cpu_current_mode(cur_gov, sizeof cur_gov, cur_epp, sizeof cur_epp);
        char mode_buf[160];
        if (strcmp(active_name, "Other") == 0 || strcmp(active_name, "Unknown") == 0)
            snprintf(mode_buf, sizeof mode_buf, "%s", cur_gov[0] ? cur_gov : "unknown");
        else
            snprintf(mode_buf, sizeof mode_buf, "%s", active_name);
        active_name = mode_buf;

        char pp_cur[64], pp_choices[256];
        platform_profile_get(pp_cur, sizeof pp_cur, pp_choices, sizeof pp_choices);

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
            "GOVERNOR=%s\n"
            "EPP=%s\n"
            "AUTO_MODE=%d\n"
            "CPU_POWER_W=%.1f\n"
            "CPU_POWER_LIMIT_W=%d\n"
            "PLATFORM_PROFILE=%s\n"
            "PLATFORM_PROFILE_CHOICES=%s\n",
            status.num_cpus,
            status.on_ac_power,
            status.cpu_temp_c,
            status.cpu_load_percent,
            freq_min_buf,
            freq_max_buf,
            active_name,
            cur_gov,
            cur_epp,
            g_cpu_state.auto_mode_enabled,
            cpu_package_power_w(),
            cpu_power_limit_w(),
            pp_cur, pp_choices);

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

    /* --- SET_POWER_MODE <name> --- governor + EPP of a power mode only,
     * leaving the active profile's frequency limits alone (APPLY_GOVERNOR
     * also resets every core's maximum to the hard limit) */
    if (strcmp(action, "SET_POWER_MODE") == 0) {
        const CpuProfile *p = arg1[0] ? cpu_profile_get(arg1) : NULL;
        if (!p) {
            snprintf(response, resp_size, "ERROR Unknown power mode '%s'\n", arg1);
            return;
        }
        if (cpu_apply_governor_and_epp(enum_to_governor(p->governor), p->epp, arg1) == 0) {
            snprintf(response, resp_size, "OK Power mode '%s' set\n", arg1);
            LOG_INFO("[DAEMON] Power mode '%s' set (profile frequencies unchanged)", arg1);
        } else {
            snprintf(response, resp_size, "ERROR Failed to set power mode '%s'\n", arg1);
        }
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

        uint64_t min_freq = arg3[0] ? parse_frequency(arg3) : 0;
        uint64_t max_freq = arg4[0] ? parse_frequency(arg4) : 0;

        LOG_INFO("[DAEMON] Creating governor policy: %s governor=%s min=%" PRIu64 "Hz max=%" PRIu64 "Hz",
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

        uint64_t freq = parse_frequency(arg1);
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

        uint64_t freq = parse_frequency(arg1);
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
        char mode_epp[CPU_EPP_NAME_MAX] = "";
        cpu_store_mode(&gov, mode_epp, arg2, "CREATE_CUSTOM_PROFILE");

        uint64_t max_freq = get_hard_max_freq_hz();
        if (arg3[0]) {
            max_freq = (uint64_t)strtoull(arg3, NULL, 10);
        }

        int apply_all = 1;
        if (arg4[0]) {
            apply_all = atoi(arg4);
        }

        if (cpu_custom_profile_create(arg1, gov, max_freq, apply_all) == 0) {
            snprintf(g_cpu_state.custom_profiles[g_cpu_state.num_custom_profiles - 1].epp,
                     CPU_EPP_NAME_MAX, "%s", mode_epp);
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
        uint64_t max_freq = (uint64_t)strtoull(arg3, NULL, 10);

        /* Read current min freq so we don't overwrite it */
        uint64_t current_min = 0;
        const CpuCustomProfile *tmp = cpu_custom_profile_get(profile_idx);
        if (tmp && cpu_id >= 0 && cpu_id < tmp->num_cores && cpu_id < MAX_CPUS) {
            current_min = tmp->per_core_min_freq_hz[cpu_id];
        }

        if (cpu_custom_profile_set_core_freq(profile_idx, cpu_id, max_freq, current_min) == 0) {
            snprintf(response, resp_size, "OK Core %d max frequency set to %" PRIu64 " Hz\n", cpu_id, max_freq);
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
        uint64_t min_freq = (uint64_t)strtoull(arg3, NULL, 10);

        /* Read current max freq so we don't overwrite it */
        uint64_t current_max = 0;
        const CpuCustomProfile *tmp2 = cpu_custom_profile_get(profile_idx);
        if (tmp2 && cpu_id >= 0 && cpu_id < tmp2->num_cores && cpu_id < MAX_CPUS) {
            current_max = tmp2->per_core_max_freq_hz[cpu_id];
        }

        if (cpu_custom_profile_set_core_freq(profile_idx, cpu_id, current_max, min_freq) == 0) {
            snprintf(response, resp_size, "OK Core %d min frequency set to %" PRIu64 " Hz\n", cpu_id, min_freq);
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
        uint64_t max_freq = (uint64_t)strtoull(arg3, NULL, 10);

        /* Read current min freq so we don't overwrite it */
        uint64_t current_min = 0;
        const CpuCustomProfile *tmp3 = cpu_custom_profile_get(profile_idx);
        if (tmp3 && cpu_id >= 0 && cpu_id < tmp3->num_cores && cpu_id < MAX_CPUS) {
            current_min = tmp3->per_core_min_freq_hz[cpu_id];
        }

        if (cpu_custom_profile_set_core_freq(profile_idx, cpu_id, max_freq, current_min) == 0) {
            snprintf(response, resp_size, "OK Core %d max frequency set to %" PRIu64 " Hz\n", cpu_id, max_freq);
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

        char gov_part[CPU_GOVERNOR_NAME_MAX], epp_part[CPU_EPP_NAME_MAX];
        cpu_parse_mode(arg2, gov_part, sizeof(gov_part), epp_part, sizeof(epp_part));
        if (governor_to_enum(gov_part, &gov) != 0) {
            snprintf(response, resp_size, "ERROR Unknown governor: %s\n", arg2);
            return;
        }

        CpuCustomProfile *sp = &g_cpu_state.custom_profiles[profile_idx];
        cpu_store_mode(&sp->governor, sp->epp, arg2, "SET_PROFILE_GOVERNOR");
        snprintf(response, resp_size, "OK governor set to %s (epp=%s) for profile %d\n",
                 enum_to_governor(sp->governor), sp->epp[0] ? sp->epp : "-", profile_idx);
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
        pos += snprintf(response + pos, resp_size - pos, "EPP:%s\n", cp->epp);
        pos += snprintf(response + pos, resp_size - pos, "PLATFORM_PROFILE:%s\n", cp->platform_profile);
        pos += snprintf(response + pos, resp_size - pos, "APPLY_ALL:%d\n", cp->apply_all_cores);
        pos += snprintf(response + pos, resp_size - pos, "NUM_CORES:%d\n", cp->num_cores);

        for (int c = 0; c < cp->num_cores && c < MAX_CPUS && pos < resp_size - 128; c++) {
            pos += snprintf(response + pos, resp_size - pos, "CORE_%d_MAX:%" PRIu64 "\n", c, cp->per_core_max_freq_hz[c]);
            pos += snprintf(response + pos, resp_size - pos, "CORE_%d_MIN:%" PRIu64 "\n", c, cp->per_core_min_freq_hz[c]);
        }
        return;
    }

    /* --- SET_CUSTOM_PROFILE_PLATFORM <index> <choice|-> (1.43.0) ---
     * the firmware power profile a CPU profile sets; "-" = leave unchanged.
     * Applied right away if the profile is active. */
    if (strcmp(action, "SET_CUSTOM_PROFILE_PLATFORM") == 0) {
        int idx = arg1[0] ? atoi(arg1) : -1;
        if (idx < 0 || idx >= g_cpu_state.num_custom_profiles || !arg2[0]) {
            snprintf(response, resp_size, "ERROR Usage: SET_CUSTOM_PROFILE_PLATFORM <index> <choice|->\n");
            return;
        }
        CpuCustomProfile *cp = &g_cpu_state.custom_profiles[idx];
        if (strcmp(arg2, "-") && !platform_profile_offered(arg2)) {
            snprintf(response, resp_size, "ERROR Firmware power profile '%s' is not offered by this laptop\n", arg2);
            return;
        }
        snprintf(cp->platform_profile, sizeof cp->platform_profile, "%s", strcmp(arg2, "-") ? arg2 : "");
        cpu_custom_profile_save_json();
        if (cp->platform_profile[0] && idx == g_cpu_state.active_custom_profile_idx)
            platform_profile_set(cp->platform_profile, cp->name);
        snprintf(response, resp_size, "OK Firmware power profile of '%s': %s\n", cp->name,
                 cp->platform_profile[0] ? cp->platform_profile : "unchanged");
        return;
    }

    /* --- GET_HARD_LIMIT --- */
    if (strcmp(action, "GET_HARD_LIMIT") == 0) {
        snprintf(response, resp_size, "OK HARD_LIMIT=%" PRIu64 "\n", get_hard_max_freq_hz());
        return;
    }

    /* --- SET_HARD_LIMIT <hz> --- */
    if (strcmp(action, "SET_HARD_LIMIT") == 0) {
        if (!arg1[0]) {
            snprintf(response, resp_size, "ERROR Usage: SET_HARD_LIMIT <hz>\n");
            return;
        }

        uint64_t new_limit = (uint64_t)strtoull(arg1, NULL, 10);
        if (new_limit == 0) {
            snprintf(response, resp_size, "ERROR Invalid hard limit value: %s\n", arg1);
            return;
        }

        set_hard_max_freq_hz(new_limit);
        cpu_custom_profile_save_json(); /* Persist hard limit to profiles.json */
        clamp_cpus_to_hard_limit();
        snprintf(response, resp_size, "OK Hard limit set to %" PRIu64 " Hz (%.2f GHz)\n", new_limit, new_limit / 1e9);
        LOG_INFO("[DAEMON] Hard limit changed to %" PRIu64 " Hz (%.2f GHz)", new_limit, new_limit / 1e9);
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
        (void)gov;
        /* arg2 = "governor[:epp]"; only available governors/EPPs are stored */
        cpu_store_mode(&g_cpu_state.custom_profiles[profile_idx].governor,
                       g_cpu_state.custom_profiles[profile_idx].epp, arg2, "SAVE_CUSTOM_PROFILE");

        /* Set apply_all_cores */
        int apply_all = atoi(arg3);
        g_cpu_state.custom_profiles[profile_idx].apply_all_cores = apply_all;

        /* Parse comma-separated core data: min0,max0,min1,max1,... */
        int cores_set = 0;
        if (arg4[0]) {
            char core_data[ARG4_LEN];
            strncpy(core_data, arg4, sizeof(core_data) - 1);
            core_data[sizeof(core_data) - 1] = '\0';

            char *save_ptr = NULL;
            char *token = strtok_r(core_data, ",", &save_ptr);
            int core_id = 0;
            int value_in_pair = 0; /* 0 = min, 1 = max */
            uint64_t min_freq = 0, max_freq = 0;

            while (token != NULL && core_id < MAX_CPUS) {
                uint64_t val = strtoull(token, NULL, 10);

                if (value_in_pair == 0) {
                    min_freq = (uint64_t)val;
                } else {
                    max_freq = (uint64_t)val;
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

/* --- Client handling: several clients at once, newline-framed commands --- */
typedef struct {
    int fd;
    char buf[CLIENT_BUF_SIZE];
    size_t len;
    double last_rx;          /* monotonic time of the last received data */
} CpuClient;

static CpuClient g_clients[MAX_CLIENTS];

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void client_drop(CpuClient *c, const char *why) {
    if (c->fd < 0) return;
    LOG_INFO("[DAEMON] Client fd %d disconnected: %s", c->fd, why);
    close(c->fd);
    c->fd = -1;
    c->len = 0;
}

/* Never block the daemon on a client that stopped reading */
static int client_send(CpuClient *c, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = send(c->fd, data + sent, len - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

/* Run one command and send its response. Returns -1 if the client was dropped. */
static int client_run_command(CpuClient *c, const char *cmd) {
    static char response[MAX_RESPONSE_LEN];
    response[0] = '\0';
    handle_command(cmd, response, sizeof(response));
    /* The reply ends with its NUL byte: an end marker, so clients can stop
     * reading at once instead of waiting for the connection to go quiet */
    if (client_send(c, response, strlen(response) + 1) != 0) {
        client_drop(c, "not reading responses");
        return -1;
    }
    return 0;
}

/* Process every complete (newline-terminated) command in the client buffer */
static void client_process_lines(CpuClient *c) {
    char *nl;
    while (c->fd >= 0 && (nl = memchr(c->buf, '\n', c->len)) != NULL) {
        size_t line_len = (size_t)(nl - c->buf);
        char cmd[CLIENT_BUF_SIZE];
        memcpy(cmd, c->buf, line_len);
        cmd[line_len] = '\0';
        if (line_len > 0 && cmd[line_len - 1] == '\r') cmd[line_len - 1] = '\0';

        size_t rest = c->len - line_len - 1;
        memmove(c->buf, nl + 1, rest);
        c->len = rest;

        if (cmd[0] != '\0' && client_run_command(c, cmd) != 0) return;
    }
}

static void client_read(CpuClient *c) {
    if (c->len >= sizeof(c->buf) - 1) {
        client_drop(c, "command too long");
        return;
    }
    ssize_t n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len, MSG_DONTWAIT);
    if (n == 0) { client_drop(c, "closed by peer"); return; }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) client_drop(c, strerror(errno));
        return;
    }
    c->len += (size_t)n;
    c->last_rx = mono_now();
    client_process_lines(c);
}

/* Older clients send a command without a trailing newline: run it once the
 * client has been quiet for UNTERMINATED_CMD_MS */
static void client_flush_unterminated(CpuClient *c, double now) {
    if (c->fd < 0 || c->len == 0) return;
    if ((now - c->last_rx) * 1000.0 < UNTERMINATED_CMD_MS) return;
    char cmd[CLIENT_BUF_SIZE];
    memcpy(cmd, c->buf, c->len);
    cmd[c->len] = '\0';
    c->len = 0;
    client_run_command(c, cmd);
}

static void client_accept(void) {
    int fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            LOG_ERROR("[DAEMON] Accept error: %s", strerror(errno));
        }
        return;
    }
    if (!dcc_peer_authorized(fd)) {
        dcc_reject_client(fd);
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd < 0) {
            g_clients[i].fd = fd;
            g_clients[i].len = 0;
            g_clients[i].last_rx = mono_now();
            LOG_INFO("[DAEMON] Client connected (fd %d, slot %d)", fd, i);
            return;
        }
    }
    LOG_WARN("[DAEMON] Too many clients (%d), rejecting fd %d", MAX_CLIENTS, fd);
    close(fd);
}

/* Re-apply frequency limits every few seconds (the kernel/thermald may change them).
 * Per core the target max is the active custom profile's value, capped by the
 * hard limit; without a profile value it is the hard limit itself. The hard
 * limit is therefore a ceiling: lower per-core values are kept, not raised. */
static void enforce_frequency_limits(void) {
    uint64_t hard_limit = get_hard_max_freq_hz();
    int cpu_count = get_cpu_count();

    const CpuCustomProfile *active_profile = NULL;
    if (g_cpu_state.active_custom_profile_idx >= 0 &&
        g_cpu_state.active_custom_profile_idx < g_cpu_state.num_custom_profiles) {
        active_profile = &g_cpu_state.custom_profiles[g_cpu_state.active_custom_profile_idx];
    }

    int max_changed = 0, min_changed = 0;
    for (int i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        uint64_t target_max = hard_limit;
        uint64_t target_min = 0;
        if (active_profile && i < active_profile->num_cores) {
            if (active_profile->per_core_max_freq_hz[i] > 0 &&
                active_profile->per_core_max_freq_hz[i] < target_max) {
                target_max = active_profile->per_core_max_freq_hz[i];
            }
            target_min = active_profile->per_core_min_freq_hz[i];
            if (target_min > target_max) target_min = target_max;
        }

        uint64_t current_max = get_cpu_max_freq(i);
        if (current_max != 0 && current_max != target_max) {
            if (set_cpu_max_freq(i, target_max) == 0) max_changed++;
        }

        /* A minimum set by the profile is enforced exactly, in both directions:
         * only raising it left stale higher minimums in place (e.g. 3.6 GHz
         * after the profile was re-saved with 0.8 GHz). */
        if (target_min > 0) {
            uint64_t current_min = get_cpu_min_freq(i);
            if (current_min != 0 && current_min != target_min) {
                if (set_cpu_min_freq(i, target_min) == 0) min_changed++;
            }
        }
    }

    if (max_changed || min_changed) {
        LOG_INFO("[ENFORCE] Re-applied limits: max on %d cores, min on %d cores (hard limit %" PRIu64 " Hz, profile %d)",
                 max_changed, min_changed, hard_limit, g_cpu_state.active_custom_profile_idx);
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
    if (log_init("CPU_DAEMON", LOG_LEVEL_INFO, CPU_LOG_DIR) != 0) {
        fprintf(stderr, "Failed to initialize logging system\n");
    }

    LOG_INFO("[DAEMON] Starting %s", PROGRAM_NAME);
    LOG_INFO("[DAEMON] Foreground mode: %s", foreground ? "yes" : "no");

    /* Initialize CPU control subsystem */
    if (cpu_control_init() != 0) {
        LOG_ERROR("[DAEMON] Failed to initialize CPU control subsystem");
        return 1;
    }

    cpu_package_power_w();   /* first energy sample, so the first STATUS has a value */

    /* cpu_control_init() already loaded the custom profiles and the hard limit */
    LOG_INFO("[DAEMON] CPU control initialized: %d power modes, %d custom profiles",
             g_cpu_state.num_profiles, g_cpu_state.num_custom_profiles);

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

    for (int i = 0; i < MAX_CLIENTS; i++) g_clients[i].fd = -1;

    LOG_INFO("[DAEMON] Daemon ready, waiting for connections...");

    double last_check = 0, last_enforce = 0;

    /* Main loop: sleep in select() until a client talks or a timer is due */
    while (g_running) {
        if (g_reload) {
            g_reload = 0;
            LOG_INFO("[DAEMON] SIGHUP: reloading configuration");
            cpu_custom_profile_load_json();
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        int max_fd = server_fd;
        int pending_unterminated = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].fd >= 0) {
                FD_SET(g_clients[i].fd, &rfds);
                if (g_clients[i].fd > max_fd) max_fd = g_clients[i].fd;
                if (g_clients[i].len > 0) pending_unterminated = 1;
            }
        }

        struct timeval tv;
        tv.tv_sec = pending_unterminated ? 0 : 1;
        tv.tv_usec = pending_unterminated ? 50000 : 0;
        int ready = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("[DAEMON] select() failed: %s", strerror(errno));
            break;
        }

        if (ready > 0) {
            if (FD_ISSET(server_fd, &rfds)) client_accept();
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (g_clients[i].fd >= 0 && FD_ISSET(g_clients[i].fd, &rfds)) {
                    client_read(&g_clients[i]);
                }
            }
        }

        double now = mono_now();
        for (int i = 0; i < MAX_CLIENTS; i++) client_flush_unterminated(&g_clients[i], now);

        /* Check auto-switching conditions periodically */
        if (now - last_check >= AUTO_CHECK_INTERVAL_SEC) {
            check_auto_conditions();
            last_check = now;
        }

        /* Re-apply frequency limits periodically */
        if (now - last_enforce >= HARD_LIMIT_ENFORCE_INTERVAL_SEC) {
            enforce_frequency_limits();
            last_enforce = now;
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++) client_drop(&g_clients[i], "daemon shutting down");

    /* Cleanup */
    LOG_INFO("[DAEMON] Shutting down...");

    close(server_fd);
    unlink(SOCKET_PATH);

    cpu_control_cleanup();

    LOG_INFO("[DAEMON] Daemon exited cleanly");
    log_cleanup();

    return 0;
}
