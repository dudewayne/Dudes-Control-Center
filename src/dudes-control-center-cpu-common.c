/*
 * dudes-control-center-cpu-common.c
 * Common implementation for Dudes Control Center CPU Speed Control subsystem
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <cjson/cJSON.h>
#include "dudes-control-center-cpu-common.h"
#include "dudes-control-center-log.h"

/* --- Global state --- */
CpuControlState g_cpu_state = {0};

/* --- CPU frequency sysfs paths --- */
#define CPUSYSFS "/sys/devices/system/cpu"
#define CPUFREQ_SUBDIR "cpufreq"

/* --- Config path helpers --- */
static const char* get_cpu_config_dir(void) {
    static char path[512] = {0};
    if (path[0] == '\0') {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(path, sizeof(path), "%s/.config/%s", home, CPU_CONFIG_DIR);
        } else {
            snprintf(path, sizeof(path), "/tmp/%s", CPU_CONFIG_DIR);
        }
    }
    return path;
}

static const char* get_cpu_profiles_path(void) {
    static char path[512] = {0};
    if (path[0] == '\0') {
        snprintf(path, sizeof(path), "%s/profiles.json", get_cpu_config_dir());
    }
    return path;
}

static const char* get_cpu_custom_profiles_path(void) {
    static char path[512] = {0};
    if (path[0] == '\0') {
        snprintf(path, sizeof(path), "%s/custom_profiles.json", get_cpu_config_dir());
    }
    return path;
}

static int ensure_cpu_config_dir(void) {
    const char *dir = get_cpu_config_dir();
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("Could not create CPU config directory: %s", dir);
        return -1;
    }
    return 0;
}

/* --- Helper: Convert governor string to enum --- */
int governor_to_enum(const char *governor_str, CpuGovernor *gov) {
    if (!governor_str || !gov) return -1;

    if (strcmp(governor_str, "performance") == 0) *gov = CPU_GOVERNOR_PERFORMANCE;
    else if (strcmp(governor_str, "powersave") == 0) *gov = CPU_GOVERNOR_POWERSAVE;
    else if (strcmp(governor_str, "userspace") == 0) *gov = CPU_GOVERNOR_USERSPACE;
    else if (strcmp(governor_str, "ondemand") == 0) *gov = CPU_GOVERNOR_ONDEMAND;
    else if (strcmp(governor_str, "conservative") == 0) *gov = CPU_GOVERNOR_CONSERVATIVE;
    else if (strcmp(governor_str, "scaling_savings") == 0) *gov = CPU_GOVERNOR_SCALING_SAVINGS;
    else if (strcmp(governor_str, "schedutil") == 0) *gov = CPU_GOVERNOR_SCHEDUTIL;
    else if (strcmp(governor_str, "EPP") == 0 || strcmp(governor_str, "epp") == 0) *gov = CPU_GOVERNOR_EPP;
    else *gov = CPU_GOVERNOR_UNKNOWN;

    return (*gov == CPU_GOVERNOR_UNKNOWN) ? -1 : 0;
}

/* --- Helper: Convert enum to governor string --- */
const char* enum_to_governor(CpuGovernor gov) {
    switch (gov) {
        case CPU_GOVERNOR_PERFORMANCE: return "performance";
        case CPU_GOVERNOR_POWERSAVE:   return "powersave";
        case CPU_GOVERNOR_USERSPACE:   return "userspace";
        case CPU_GOVERNOR_ONDEMAND:    return "ondemand";
        case CPU_GOVERNOR_CONSERVATIVE: return "conservative";
        case CPU_GOVERNOR_SCALING_SAVINGS: return "scaling_savings";
        case CPU_GOVERNOR_SCHEDUTIL:   return "schedutil";
        case CPU_GOVERNOR_EPP:         return "epp";
        default: return "unknown";
    }
}

/* --- Helper: Parse frequency string like "100MHz" or "2.5GHz" to Hz --- */
uint32_t parse_frequency(const char *str) {
    if (!str || !*str) return 0;

    char unit[8] = {0};
    double value = 0;

    int i = 0, j = 0;
    while (str[i] && (isdigit(str[i]) || str[i] == '.')) {
        i++;
    }
    if (i > 0) {
        char num_str[32];
        strncpy(num_str, str, i);
        num_str[i] = '\0';
        value = atof(num_str);
    }

    while (str[i] && j < 7) {
        unit[j++] = str[i++];
    }

    if (strcmp(unit, "GHz") == 0 || strcmp(unit, "ghz") == 0) {
        return (uint32_t)(value * 1e9);
    } else if (strcmp(unit, "MHz") == 0 || strcmp(unit, "mhz") == 0) {
        return (uint32_t)(value * 1e6);
    } else if (strcmp(unit, "kHz") == 0 || strcmp(unit, "khz") == 0) {
        return (uint32_t)(value * 1e3);
    } else {
        return (uint32_t)value;
    }
}

/* --- Helper: Format frequency to human-readable string --- */
void format_frequency(uint32_t freq_hz, char *buf, size_t buf_size) {
    if (freq_hz >= 1e9) {
        snprintf(buf, buf_size, "%.2f GHz", freq_hz / 1e9);
    } else if (freq_hz >= 1e6) {
        snprintf(buf, buf_size, "%.2f MHz", freq_hz / 1e6);
    } else if (freq_hz >= 1e3) {
        snprintf(buf, buf_size, "%.2f kHz", freq_hz / 1e3);
    } else {
        snprintf(buf, buf_size, "%u Hz", freq_hz);
    }
}

/* --- Sysfs read helpers --- */

/* Read a sysfs file as a string */
static int sysfs_read_string(int cpu_id, const char *filename, char *buffer, size_t buf_size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/cpu%d/%s", CPUSYSFS, cpu_id, filename);

    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    if (fgets(buffer, (int)buf_size, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    /* Strip trailing newline */
    buffer[strcspn(buffer, "\n")] = '\0';
    return 0;
}

/* Read a sysfs file as an unsigned integer (value is in kHz from sysfs, converted to Hz) */
static uint32_t sysfs_read_freq(int cpu_id, const char *filename) {
    char path[512];
    snprintf(path, sizeof(path), "%s/cpu%d/%s", CPUSYSFS, cpu_id, filename);

    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    uint32_t val_khz = 0;
    if (fscanf(fp, "%u", &val_khz) != 1) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    return val_khz * 1000;  /* sysfs reports in kHz, convert to Hz */
}

/* Write a string to a sysfs file */
static int sysfs_write_string(int cpu_id, const char *filename, const char *value) {
    char path[512];
    snprintf(path, sizeof(path), "%s/cpu%d/%s", CPUSYSFS, cpu_id, filename);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR("[CPU] Failed to open %s for writing: %s", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "%s", value);
    fclose(fp);
    return 0;
}

/* Write an unsigned integer (as kHz) to a sysfs file */
static int sysfs_write_freq(int cpu_id, const char *filename, uint32_t freq_hz) {
    if (!freq_hz) return -1;

    char path[512];
    snprintf(path, sizeof(path), "%s/cpu%d/%s", CPUSYSFS, cpu_id, filename);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR("[CPU] Failed to open %s for writing: %s", path, strerror(errno));
        return -1;
    }

    /* sysfs expects kHz */
    uint32_t freq_khz = freq_hz / 1000;
    fprintf(fp, "%u", freq_khz);
    fclose(fp);
    return 0;
}

/* --- Initialize CPU control subsystem --- */
int cpu_control_init(void) {
    LOG_INFO("[CPU] Initializing CPU control subsystem");

    /* Load profiles from disk */
    if (cpu_profiles_load_all() != 0) {
        LOG_WARN("[CPU] Could not load saved profiles, using defaults");

        /* Create default profiles */
        cpu_profile_create("Performance", CPU_GOVERNOR_PERFORMANCE, 0, 0);
        cpu_profile_create("Balanced", CPU_GOVERNOR_SCHEDUTIL, 0, 0);
        cpu_profile_create("Power Save", CPU_GOVERNOR_POWERSAVE, 0, 0);

        /* Set Balanced as default */
        cpu_profile_set_active("Balanced");
    }

    /* Load custom profiles */
    cpu_custom_profile_load_json();

    /* Apply HARD SAFETY LIMIT immediately on startup (3.6 GHz max) */
    LOG_INFO("[CPU] Applying hard safety limit: %u Hz (%.1f GHz)",
             HARD_MAX_FREQ_HZ, HARD_MAX_FREQ_HZ / 1e9);
    set_all_cpus_max_freq(HARD_MAX_FREQ_HZ);

    return 0;
}

/* --- Cleanup CPU control subsystem --- */
void cpu_control_cleanup(void) {
    LOG_INFO("[CPU] Cleaning up CPU control subsystem");
    cpu_profiles_save_all();
    cpu_custom_profile_save_json();
}

/* --- Get number of online CPUs --- */
int get_cpu_count(void) {
    DIR *dir = opendir(CPUSYSFS);
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Match directories like "cpu0", "cpu1", etc. */
        if (strncmp(entry->d_name, "cpu", 3) == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s/topology", CPUSYSFS, entry->d_name);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                count++;
            }
        }
    }
    closedir(dir);

    return count;
}

/* --- Get CPU information for a specific CPU --- */
const CpuInfo* get_cpu_info(int cpu_id) {
    static CpuInfo cpu_info = {0};

    cpu_info.cpu_id = cpu_id;
    cpu_info.online = 1;

    /* Read governor */
    if (sysfs_read_string(cpu_id, "cpufreq/scaling_governor", cpu_info.governor, sizeof(cpu_info.governor)) != 0) {
        strcpy(cpu_info.governor, "unknown");
    }

    /* Read frequencies */
    cpu_info.min_freq_hz = sysfs_read_freq(cpu_id, "cpufreq/scaling_min_freq");
    cpu_info.max_freq_hz = sysfs_read_freq(cpu_id, "cpufreq/scaling_max_freq");
    cpu_info.current_freq_hz = sysfs_read_freq(cpu_id, "cpufreq/scaling_cur_freq");

    /* Fallback: try cpuinfo_cur_freq if scaling_cur_freq unavailable */
    if (!cpu_info.current_freq_hz) {
        cpu_info.current_freq_hz = sysfs_read_freq(cpu_id, "cpufreq/cpuinfo_cur_freq");
    }

    return &cpu_info;
}

/* --- Get list of available governors --- */
int get_all_governors(char **governor_list, int *count) {
    char buffer[256] = {0};

    /* Try to read available governors from sysfs */
    char path[512];
    snprintf(path, sizeof(path), "%s/cpufreq/available_governors", CPUSYSFS);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_WARN("[CPU] Could not read available governors from %s", path);
        return -1;
    }

    if (fgets(buffer, sizeof(buffer), fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    /* Parse space-separated governor names */
    *count = 0;
    char *token = strtok(buffer, " \n");
    while (token && *count < MAX_CPUS) {
        if (!governor_list) {
            (*count)++;
        } else {
            strncpy(governor_list[*count], token, CPU_GOVERNOR_NAME_MAX - 1);
            governor_list[*count][CPU_GOVERNOR_NAME_MAX - 1] = '\0';
            (*count)++;
        }
        token = strtok(NULL, " \n");
    }

    return (*count > 0) ? 0 : -1;
}

/* --- Get current governor for a CPU --- */
int get_current_governor(int cpu_id, char *buffer, size_t buf_size) {
    const CpuInfo *info = get_cpu_info(cpu_id);
    if (!info) return -1;

    strncpy(buffer, info->governor, buf_size - 1);
    buffer[buf_size - 1] = '\0';
    return 0;
}

/* --- Frequency read functions --- */
uint32_t get_cpu_min_freq(int cpu_id) {
    return sysfs_read_freq(cpu_id, "cpufreq/scaling_min_freq");
}

uint32_t get_cpu_max_freq(int cpu_id) {
    return sysfs_read_freq(cpu_id, "cpufreq/scaling_max_freq");
}

uint32_t get_cpu_current_freq(int cpu_id) {
    uint32_t freq = sysfs_read_freq(cpu_id, "cpufreq/scaling_cur_freq");
    if (!freq) {
        freq = sysfs_read_freq(cpu_id, "cpufreq/cpuinfo_cur_freq");
    }
    return freq;
}

/* --- Frequency write functions --- */
int set_cpu_governor(int cpu_id, const char *governor) {
    if (!governor) return -1;
    return sysfs_write_string(cpu_id, "cpufreq/scaling_governor", governor);
}

int set_cpu_min_freq(int cpu_id, uint32_t freq_hz) {
    return sysfs_write_freq(cpu_id, "cpufreq/scaling_min_freq", freq_hz);
}

int set_cpu_max_freq(int cpu_id, uint32_t freq_hz) {
    return sysfs_write_freq(cpu_id, "cpufreq/scaling_max_freq", freq_hz);
}

/* --- Set governor/frequency for all CPUs --- */
int set_all_cpus_governor(const char *governor) {
    int cpu_count = get_cpu_count();
    int errors = 0;

    for (int i = 0; i < cpu_count; i++) {
        if (set_cpu_governor(i, governor) != 0) {
            errors++;
        }
    }
    return (errors == 0) ? 0 : -1;
}

int set_all_cpus_min_freq(uint32_t freq_hz) {
    int cpu_count = get_cpu_count();
    int errors = 0;

    for (int i = 0; i < cpu_count; i++) {
        if (set_cpu_min_freq(i, freq_hz) != 0) {
            errors++;
        }
    }
    return (errors == 0) ? 0 : -1;
}

int set_all_cpus_max_freq(uint32_t freq_hz) {
    int cpu_count = get_cpu_count();
    int errors = 0;

    for (int i = 0; i < cpu_count; i++) {
        if (set_cpu_max_freq(i, freq_hz) != 0) {
            errors++;
        }
    }
    return (errors == 0) ? 0 : -1;
}

/* --- Profile management functions --- */

/* Create a new profile */
int cpu_profile_create(const char *name, CpuGovernor governor,
                       uint32_t min_freq, uint32_t max_freq) {
    if (!name || g_cpu_state.num_profiles >= MAX_CPU_PROFILES) {
        return -1;
    }

    /* Check for duplicate name */
    for (int i = 0; i < g_cpu_state.num_profiles; i++) {
        if (strcmp(g_cpu_state.profiles[i].name, name) == 0) {
            LOG_WARN("[CPU] Profile '%s' already exists", name);
            return -1;
        }
    }

    /* Apply safety limit to max_freq if set */
    if (max_freq > HARD_MAX_FREQ_HZ) {
        max_freq = HARD_MAX_FREQ_HZ;
    }

    CpuProfile *p = &g_cpu_state.profiles[g_cpu_state.num_profiles];
    strncpy(p->name, name, CPU_PROFILE_NAME_MAX - 1);
    p->name[CPU_PROFILE_NAME_MAX - 1] = '\0';
    p->governor = governor;
    p->min_freq_hz = min_freq;
    p->max_freq_hz = max_freq;
    p->condition_type = CONDITION_NONE;
    p->condition_threshold = 0;
    p->is_auto_switchable = 1;

    g_cpu_state.num_profiles++;
    LOG_INFO("[CPU] Created profile '%s' (governor=%s)", name, enum_to_governor(governor));
    return 0;
}

/* Delete a profile by name */
int cpu_profile_delete(const char *name) {
    if (!name) return -1;

    int idx = -1;
    for (int i = 0; i < g_cpu_state.num_profiles; i++) {
        if (strcmp(g_cpu_state.profiles[i].name, name) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) return -1;

    /* Shift remaining profiles down */
    for (int i = idx; i < g_cpu_state.num_profiles - 1; i++) {
        g_cpu_state.profiles[i] = g_cpu_state.profiles[i + 1];
    }
    g_cpu_state.num_profiles--;

    /* Reset active index if needed */
    if (g_cpu_state.active_profile_idx >= g_cpu_state.num_profiles) {
        g_cpu_state.active_profile_idx = g_cpu_state.num_profiles - 1;
    }

    LOG_INFO("[CPU] Deleted profile '%s'", name);
    return 0;
}

/* Apply a profile to all CPUs */
int cpu_profile_apply(const char *name) {
    if (!name) return -1;

    const CpuProfile *p = cpu_profile_get(name);
    if (!p) {
        LOG_ERROR("[CPU] Profile '%s' not found", name);
        return -1;
    }

    const char *gov_str = enum_to_governor(p->governor);
    LOG_INFO("[CPU] Applying profile '%s' (governor=%s)", name, gov_str);

    /* Set governor */
    if (set_all_cpus_governor(gov_str) != 0) {
        LOG_ERROR("[CPU] Failed to set governor '%s'", gov_str);
        return -1;
    }

    /* Set min freq if specified */
    if (p->min_freq_hz > 0) {
        if (set_all_cpus_min_freq(p->min_freq_hz) != 0) {
            LOG_WARN("[CPU] Failed to set min freq to %u Hz", p->min_freq_hz);
        }
    }

    /* Set max freq if specified (always apply safety limit) */
    uint32_t max_freq = p->max_freq_hz > 0 ? p->max_freq_hz : HARD_MAX_FREQ_HZ;
    if (max_freq > HARD_MAX_FREQ_HZ) max_freq = HARD_MAX_FREQ_HZ;
    if (set_all_cpus_max_freq(max_freq) != 0) {
        LOG_WARN("[CPU] Failed to set max freq to %u Hz", max_freq);
    }

    /* Set as active */
    cpu_profile_set_active(name);

    return 0;
}

/* Set active profile (in memory only) */
int cpu_profile_set_active(const char *name) {
    if (!name) return -1;

    for (int i = 0; i < g_cpu_state.num_profiles; i++) {
        if (strcmp(g_cpu_state.profiles[i].name, name) == 0) {
            g_cpu_state.active_profile_idx = i;
            strncpy(g_cpu_state.last_used_profile, name, CPU_PROFILE_NAME_MAX - 1);
            g_cpu_state.last_used_profile[CPU_PROFILE_NAME_MAX - 1] = '\0';
            LOG_INFO("[CPU] Active profile set to '%s'", name);
            return 0;
        }
    }

    LOG_ERROR("[CPU] Cannot set active profile '%s': not found", name);
    return -1;
}

/* Get profile by name */
const CpuProfile* cpu_profile_get(const char *name) {
    if (!name) return NULL;

    for (int i = 0; i < g_cpu_state.num_profiles; i++) {
        if (strcmp(g_cpu_state.profiles[i].name, name) == 0) {
            return &g_cpu_state.profiles[i];
        }
    }
    return NULL;
}

/* Get currently active profile */
const CpuProfile* cpu_profile_get_active(void) {
    if (g_cpu_state.active_profile_idx < 0 ||
        g_cpu_state.active_profile_idx >= g_cpu_state.num_profiles) {
        return NULL;
    }
    return &g_cpu_state.profiles[g_cpu_state.active_profile_idx];
}

/* List all profiles */
int cpu_profile_list(char *buffer, size_t buf_size) {
    if (!buffer || buf_size == 0) return -1;

    int offset = 0;
    const CpuProfile *active = cpu_profile_get_active();

    for (int i = 0; i < g_cpu_state.num_profiles; i++) {
        const CpuProfile *p = &g_cpu_state.profiles[i];
        char entry[256];
        const char *marker = (active && strcmp(active->name, p->name) == 0) ? " [*]" : "    ";

        int written = snprintf(entry, sizeof(entry), "%s %s (gov=%s, min=%u Hz, max=%u Hz)\n",
                              marker, p->name, enum_to_governor(p->governor),
                              p->min_freq_hz, p->max_freq_hz);

        if (offset + written >= (int)buf_size) break;
        memcpy(buffer + offset, entry, written);
        offset += written;
    }

    return offset;
}

/* Set condition for auto-switching */
int cpu_profile_set_condition(const char *name, ConditionType type, int threshold) {
    /* Find profile directly to avoid const cast issues */
    if (!name) return -1;

    for (int i = 0; i < g_cpu_state.num_profiles; i++) {
        if (strcmp(g_cpu_state.profiles[i].name, name) == 0) {
            g_cpu_state.profiles[i].condition_type = type;
            g_cpu_state.profiles[i].condition_threshold = threshold;
            g_cpu_state.profiles[i].is_auto_switchable = 1;
            LOG_INFO("[CPU] Set condition for profile '%s': type=%d, threshold=%d",
                     name, type, threshold);
            return 0;
        }
    }
    return -1;
}

/* Clear condition from profile */
void cpu_profile_clear_condition(const char *name) {
    if (!name) return;

    for (int i = 0; i < g_cpu_state.num_profiles; i++) {
        if (strcmp(g_cpu_state.profiles[i].name, name) == 0) {
            g_cpu_state.profiles[i].condition_type = CONDITION_NONE;
            g_cpu_state.profiles[i].condition_threshold = 0;
            g_cpu_state.profiles[i].is_auto_switchable = 0;
            LOG_INFO("[CPU] Cleared condition for profile '%s'", name);
            return;
        }
    }
}

/* --- System status functions --- */

/* Check if system is on AC power */
int is_on_ac_power(void) {
    /* Try ACPI first */
    FILE *fp = fopen("/sys/class/power_supply/AC/online", "r");
    if (fp) {
        int val = 0;
        if (fscanf(fp, "%d", &val) == 1) {
            fclose(fp);
            return val;  /* 1 = on AC, 0 = on battery */
        }
        fclose(fp);
    }

    /* Fallback: check for AC adapter */
    fp = fopen("/sys/class/power_supply/ACAD/online", "r");
    if (fp) {
        int val = 0;
        if (fscanf(fp, "%d", &val) == 1) {
            fclose(fp);
            return val;
        }
        fclose(fp);
    }

    /* Fallback: enumerate power supplies */
    DIR *dir = opendir("/sys/class/power_supply");
    if (dir) {
        struct dirent *entry;
        int on_ac = 1;  /* Default to AC if no battery found */
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;

            char type_path[512];
            snprintf(type_path, sizeof(type_path),
                     "/sys/class/power_supply/%s/type", entry->d_name);

            FILE *type_fp = fopen(type_path, "r");
            if (type_fp) {
                char type[64] = {0};
                if (fgets(type, sizeof(type), type_fp) && strstr(type, "Battery")) {
                    fclose(type_fp);
                    /* Check battery status */
                    char status_path[512];
                    snprintf(status_path, sizeof(status_path),
                             "/sys/class/power_supply/%s/status", entry->d_name);
                    FILE *status_fp = fopen(status_path, "r");
                    if (status_fp) {
                        char status[64] = {0};
                        if (fgets(status, sizeof(status), status_fp)) {
                            on_ac = (strstr(status, "Charging") || strstr(status, "Full")) ? 1 : 0;
                        }
                        fclose(status_fp);
                    }
                } else {
                    fclose(type_fp);
                }
            }
        }
        closedir(dir);
        return on_ac;
    }

    /* Default: assume on AC */
    return 1;
}

/* Get CPU temperature in degrees Celsius */
int get_cpu_temperature(void) {
    /* Try thermal_zone first */
    DIR *dir = opendir("/sys/class/thermal");
    if (dir) {
        struct dirent *entry;
        int max_temp = -1;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            if (strncmp(entry->d_name, "thermal_zone", 12) != 0) continue;

            char type_path[512];
            snprintf(type_path, sizeof(type_path),
                     "/sys/class/thermal/%s/type", entry->d_name);

            FILE *type_fp = fopen(type_path, "r");
            if (type_fp) {
                char type[64] = {0};
                if (fgets(type, sizeof(type), type_fp)) {
                    /* Only use CPU thermal zones */
                    if (strstr(type, "x86_pkg_temp") || strstr(type, "cpu") ||
                        strstr(type, "CPU") || strstr(type, "k10temp") ||
                        strstr(type, "coretemp")) {
                        fclose(type_fp);

                        char temp_path[512];
                        snprintf(temp_path, sizeof(temp_path),
                                 "/sys/class/thermal/%s/temp", entry->d_name);
                        FILE *temp_fp = fopen(temp_path, "r");
                        if (temp_fp) {
                            int temp_milli = 0;
                            if (fscanf(temp_fp, "%d", &temp_milli) == 1) {
                                int temp_c = temp_milli / 1000;
                                if (temp_c > max_temp) {
                                    max_temp = temp_c;
                                }
                            }
                            fclose(temp_fp);
                        }
                    } else {
                        fclose(type_fp);
                    }
                } else {
                    fclose(type_fp);
                }
            }
        }
        closedir(dir);
        if (max_temp >= 0) return max_temp;
    }

    /* Fallback: try direct sysfs path for coretemp */
    FILE *temp_fp = fopen("/sys/devices/platform/coretemp.0/hwmon/temp1_input", "r");
    if (!temp_fp) {
        temp_fp = fopen("/sys/class/hwmon/hwmon0/temp1_input", "r");
    }
    if (temp_fp) {
        int temp_milli = 0;
        if (fscanf(temp_fp, "%d", &temp_milli) == 1) {
            fclose(temp_fp);
            return temp_milli / 1000;
        }
        fclose(temp_fp);
    }

    /* Temperature unavailable */
    LOG_WARN("[CPU] Could not read CPU temperature");
    return -1;
}

/* Get CPU load percentage (0-100) */
double get_cpu_load_percent(void) {
    /* Use /proc/loadavg for 1-minute load average */
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) {
        LOG_WARN("[CPU] Could not read /proc/loadavg");
        return 0.0;
    }

    double load_1min = 0.0;
    if (fscanf(fp, "%lf", &load_1min) != 1) {
        fclose(fp);
        return 0.0;
    }
    fclose(fp);

    /* Get number of CPUs for percentage calculation */
    int num_cpus = get_cpu_count();
    if (num_cpus <= 0) num_cpus = 1;

    double percent = (load_1min / (double)num_cpus) * 100.0;
    if (percent > 100.0) percent = 100.0;
    return percent;
}

/* Get complete CPU system status */
int get_cpu_system_status(CpuSystemStatus *status) {
    if (!status) return -1;

    memset(status, 0, sizeof(CpuSystemStatus));

    status->num_cpus = get_cpu_count();
    if (status->num_cpus <= 0) {
        LOG_ERROR("[CPU] No CPUs detected");
        return -1;
    }

    /* Cap at MAX_CPUS */
    if (status->num_cpus > MAX_CPUS) {
        status->num_cpus = MAX_CPUS;
    }

    /* Get info for each CPU */
    for (int i = 0; i < status->num_cpus; i++) {
        const CpuInfo *info = get_cpu_info(i);
        if (info) {
            memcpy(&status->cpus[i], info, sizeof(CpuInfo));
        }
    }

    /* System-level info */
    status->on_ac_power = is_on_ac_power();
    status->cpu_temp_c = get_cpu_temperature();
    status->cpu_load_percent = get_cpu_load_percent();

    return 0;
}
