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
#include "dudes-control-center-secure.h"

/* --- Global state --- */
CpuControlState g_cpu_state = {0};

/* --- Runtime hard frequency limit (configurable) --- */
static uint64_t g_hard_max_freq_hz = HARD_MAX_FREQ_HZ_DEFAULT;

uint64_t get_hard_max_freq_hz(void) {
    return g_hard_max_freq_hz;
}

void set_hard_max_freq_hz(uint64_t freq_hz) {
    g_hard_max_freq_hz = freq_hz;
    LOG_INFO("[CPU] Hard max frequency limit changed to %" PRIu64 " Hz (%.2f GHz)", freq_hz, freq_hz / 1e9);
}

/* --- CPU frequency sysfs paths --- */
#ifndef CPUSYSFS   /* overridable for tests */
#define CPUSYSFS "/sys/devices/system/cpu"
#endif
#define CPUFREQ_SUBDIR "cpufreq"

/* --- Config path helpers --- */
/* Daemons run as root (HOME=/root): profiles.json of the desktop user,
 * see dcc_profiles_path() in secure.h. CONTROL_CENTER_CONFIG_DIR forces a
 * directory (tests). */
static const char* get_main_profiles_path(void) {
#ifdef CONTROL_CENTER_CONFIG_DIR
    static char path[512] = {0};
    if (path[0] == '\0') {
        snprintf(path, sizeof(path), "%s/profiles.json", CONTROL_CENTER_CONFIG_DIR);
    }
    return path;
#else
    const char *p = dcc_profiles_path();
    return p ? p : "/nonexistent/profiles.json";
#endif
}



/* --- Helper: Convert governor string to enum --- */
int governor_to_enum(const char *governor_str, CpuGovernor *gov) {
    if (!governor_str || !gov) return -1;

    if (strcmp(governor_str, "performance") == 0 || strcmp(governor_str, "perf") == 0) *gov = CPU_GOVERNOR_PERFORMANCE;
    else if (strcmp(governor_str, "powersave") == 0 || strcmp(governor_str, "pwr") == 0) *gov = CPU_GOVERNOR_POWERSAVE;
    else if (strcmp(governor_str, "userspace") == 0 || strcmp(governor_str, "userspace") == 0) *gov = CPU_GOVERNOR_USERSPACE;
    else if (strcmp(governor_str, "ondemand") == 0 || strcmp(governor_str, "ond") == 0) *gov = CPU_GOVERNOR_ONDEMAND;
    else if (strcmp(governor_str, "conservative") == 0 || strcmp(governor_str, "con") == 0) *gov = CPU_GOVERNOR_CONSERVATIVE;
    else if (strcmp(governor_str, "scaling_savings") == 0) *gov = CPU_GOVERNOR_SCALING_SAVINGS;
    else if (strcmp(governor_str, "schedutil") == 0 || strcmp(governor_str, "sched") == 0) *gov = CPU_GOVERNOR_SCHEDUTIL;
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
uint64_t parse_frequency(const char *str) {
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
        return (uint64_t)(value * 1e9);
    } else if (strcmp(unit, "MHz") == 0 || strcmp(unit, "mhz") == 0) {
        return (uint64_t)(value * 1e6);
    } else if (strcmp(unit, "kHz") == 0 || strcmp(unit, "khz") == 0) {
        return (uint64_t)(value * 1e3);
    } else {
        return (uint64_t)value;
    }
}

/* --- Helper: Format frequency to human-readable string --- */
void format_frequency(uint64_t freq_hz, char *buf, size_t buf_size) {
    if (freq_hz >= 1e9) {
        snprintf(buf, buf_size, "%.2f GHz", freq_hz / 1e9);
    } else if (freq_hz >= 1e6) {
        snprintf(buf, buf_size, "%.2f MHz", freq_hz / 1e6);
    } else if (freq_hz >= 1e3) {
        snprintf(buf, buf_size, "%.2f kHz", freq_hz / 1e3);
    } else {
        snprintf(buf, buf_size, "%" PRIu64 " Hz", freq_hz);
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
static uint64_t sysfs_read_freq(int cpu_id, const char *filename) {
    char path[512];
    snprintf(path, sizeof(path), "%s/cpu%d/%s", CPUSYSFS, cpu_id, filename);

    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    uint64_t val_khz = 0;
    if (fscanf(fp, "%" SCNu64 "", &val_khz) != 1) {
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

    int ret = fprintf(fp, "%s", value);
    if (ret < 0) {
        LOG_ERROR("[CPU] Failed to write to %s: %s", path, strerror(errno));
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

/* Write an unsigned integer (as kHz) to a sysfs file */
static int sysfs_write_freq(int cpu_id, const char *filename, uint64_t freq_hz) {
    if (!freq_hz) return -1;

    char path[512];
    snprintf(path, sizeof(path), "%s/cpu%d/%s", CPUSYSFS, cpu_id, filename);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR("[CPU] Failed to open %s for writing: %s", path, strerror(errno));
        return -1;
    }

    /* sysfs expects kHz */
    uint64_t freq_khz = freq_hz / 1000;
    int ret = fprintf(fp, "%" PRIu64 "", freq_khz);
    if (ret < 0) {
        LOG_ERROR("[CPU] Failed to write to %s: %s", path, strerror(errno));
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

/* --- Governor availability ---
 * The cpufreq driver decides which governors exist. intel_pstate in active
 * mode (common on Intel laptops) only offers "performance" and "powersave";
 * there "powersave" scales dynamically with load and is the counterpart of
 * schedutil. Profiles may still name governors that do not exist here (e.g.
 * created on another system or by older versions that always offered
 * "Balanced"/schedutil), so every apply/save resolves to an available one. */
int cpu_governor_available(const char *gov) {
    if (!gov || !*gov) return 0;
    char storage[MAX_CPUS][CPU_GOVERNOR_NAME_MAX];
    char *list[MAX_CPUS];
    for (int i = 0; i < MAX_CPUS; i++) list[i] = storage[i];
    int count = 0;
    if (get_all_governors(list, &count) != 0) return 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i], gov) == 0) return 1;
    }
    return 0;
}

const char *cpu_governor_resolve(const char *wanted) {
    if (!wanted) return NULL;
    if (cpu_governor_available(wanted)) return wanted;

    static const char *perf_order[]    = {"performance", "schedutil", "ondemand", "powersave", NULL};
    static const char *save_order[]    = {"powersave", "schedutil", "conservative", "ondemand", NULL};
    static const char *dynamic_order[] = {"schedutil", "powersave", "ondemand", "conservative", "performance", NULL};
    const char **order = dynamic_order;
    if (strcmp(wanted, "performance") == 0) order = perf_order;
    else if (strcmp(wanted, "powersave") == 0) order = save_order;

    for (int i = 0; order[i]; i++) {
        if (cpu_governor_available(order[i])) return order[i];
    }
    return NULL;
}

CpuGovernor cpu_governor_resolve_enum(CpuGovernor gov, const char *context) {
    const char *wanted = enum_to_governor(gov);
    const char *resolved = cpu_governor_resolve(wanted);
    if (!resolved || strcmp(resolved, wanted) == 0) return gov;
    CpuGovernor out = gov;
    if (governor_to_enum(resolved, &out) != 0) return gov;
    LOG_WARN("[CPU] %s: governor '%s' is not available on this system - using '%s'",
             context ? context : "governor", wanted, resolved);
    return out;
}

/* --- Power modes: governor + EPP ---------------------------------------- */
int cpu_epp_available(const char *epp) {
    if (!epp || !*epp) return 0;
    char buf[256];
    if (sysfs_read_string(0, "cpufreq/energy_performance_available_preferences", buf, sizeof(buf)) != 0) {
        return 0;   /* driver without EPP support */
    }
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
        if (strcmp(tok, epp) == 0) return 1;
    }
    return 0;
}

/* The EPP that expresses what a governor name means: dynamic governors are
 * "balanced", powersave is "power saving". performance needs none (the
 * performance governor pins EPP itself). */
const char *cpu_default_epp_for(const char *requested_governor) {
    if (!requested_governor) return NULL;
    if (strcmp(requested_governor, "schedutil") == 0 || strcmp(requested_governor, "ondemand") == 0 ||
        strcmp(requested_governor, "conservative") == 0) {
        return "balance_performance";
    }
    if (strcmp(requested_governor, "powersave") == 0) return "power";
    return NULL;
}

void cpu_parse_mode(const char *arg, char *gov, size_t gov_size, char *epp, size_t epp_size) {
    gov[0] = '\0';
    epp[0] = '\0';
    if (!arg) return;
    const char *colon = strchr(arg, ':');
    size_t glen = colon ? (size_t)(colon - arg) : strlen(arg);
    if (glen >= gov_size) glen = gov_size - 1;
    memcpy(gov, arg, glen);
    gov[glen] = '\0';
    if (colon && colon[1] && strcmp(colon + 1, "-") != 0) {
        snprintf(epp, epp_size, "%s", colon + 1);
    }
}

/* Turn a requested "gov[:epp]" into what is stored in a profile: an
 * available governor and an explicit EPP (or "" where none applies). */
void cpu_store_mode(CpuGovernor *gov_out, char *epp_out, const char *mode_arg, const char *context) {
    char gov[CPU_GOVERNOR_NAME_MAX], epp[CPU_EPP_NAME_MAX];
    cpu_parse_mode(mode_arg, gov, sizeof(gov), epp, sizeof(epp));
    CpuGovernor g;
    if (governor_to_enum(gov, &g) != 0) return;   /* unknown name: leave unchanged */
    CpuGovernor resolved = cpu_governor_resolve_enum(g, context);
    *gov_out = resolved;

    epp_out[0] = '\0';
    if (resolved == CPU_GOVERNOR_PERFORMANCE) return;          /* EPP fixed by the governor */
    const char *want = epp[0] ? epp : cpu_default_epp_for(gov);
    if (want && cpu_epp_available(want)) {
        snprintf(epp_out, CPU_EPP_NAME_MAX, "%s", want);
    }
}

/* Apply governor (resolved to an available one) and the matching EPP.
 * Returns -1 if the governor could not be set. */
int cpu_apply_governor_and_epp(const char *requested_governor, const char *epp, const char *context) {
    const char *gov = cpu_governor_resolve(requested_governor);
    if (!gov) gov = requested_governor;
    if (strcmp(gov, requested_governor) != 0) {
        LOG_WARN("[CPU] %s: governor '%s' is not available - using '%s' (re-save the profile to store it)",
                 context, requested_governor, gov);
    }
    int ret = 0;
    if (set_all_cpus_governor(gov) != 0) {
        LOG_WARN("[CPU] %s: failed to set governor '%s'", context, gov);
        ret = -1;
    }

    /* EPP after the governor (switching governors can reset it) */
    const char *want = (epp && *epp) ? epp : cpu_default_epp_for(requested_governor);
    if (strcmp(gov, "performance") == 0) {
        want = NULL;   /* the performance governor pins EPP itself */
    }
    if (want) {
        if (cpu_epp_available(want)) {
            set_all_cpus_epp(want);
        } else {
            LOG_INFO("[CPU] %s: EPP '%s' not supported here - left unchanged", context, want);
        }
    }
    LOG_INFO("[CPU] %s: governor=%s epp=%s", context, gov, want ? want : "(unchanged)");
    return ret;
}

static void cpu_profile_set_epp(const char *name, const char *epp) {
    CpuProfile *p = (CpuProfile *)cpu_profile_get(name);
    if (p) snprintf(p->epp, sizeof(p->epp), "%s", epp ? epp : "");
}

/* --- Initialize CPU control subsystem --- */
int cpu_control_init(void) {
    LOG_INFO("[CPU] Initializing CPU control subsystem");

    /* Built-in power modes (Performance / Balanced / Power Save); they are not
     * stored on disk. User profiles are loaded below from profiles.json. */
    {
        /* Create the power modes, but only for governors this system offers
         * (if the list cannot be read at all, keep all three) */
        int have_perf  = cpu_governor_available("performance");
        int have_sched = cpu_governor_available("schedutil");
        int have_save  = cpu_governor_available("powersave");
        int have_bal_epp   = cpu_epp_available("balance_performance");
        int have_power_epp = cpu_epp_available("power");
        int unknown    = !have_perf && !have_sched && !have_save;

        if (have_perf || unknown) cpu_profile_create("Performance", CPU_GOVERNOR_PERFORMANCE, 0, 0);

        /* Balanced: schedutil where it exists, otherwise (intel_pstate active)
         * powersave with EPP balance_performance */
        if (have_sched || unknown) {
            cpu_profile_create("Balanced", CPU_GOVERNOR_SCHEDUTIL, 0, 0);
        } else if (have_save && have_bal_epp) {
            cpu_profile_create("Balanced", CPU_GOVERNOR_POWERSAVE, 0, 0);
            cpu_profile_set_epp("Balanced", "balance_performance");
        } else {
            LOG_INFO("[CPU] Neither schedutil nor EPP balance_performance available - no 'Balanced' policy");
        }

        if (have_save || unknown) {
            cpu_profile_create("Power Save", CPU_GOVERNOR_POWERSAVE, 0, 0);
            if (have_power_epp) cpu_profile_set_epp("Power Save", "power");
        }

        /* Default: Balanced if available, otherwise Power Save, otherwise the first one */
        if (cpu_profile_get("Balanced")) cpu_profile_set_active("Balanced");
        else if (cpu_profile_get("Power Save")) cpu_profile_set_active("Power Save");
        else if (g_cpu_state.num_profiles > 0) cpu_profile_set_active(g_cpu_state.profiles[0].name);
        LOG_INFO("[CPU] %d built-in power modes available", g_cpu_state.num_profiles);
    }

    /* Load custom profiles (also loads hard limit from profiles.json) */
    cpu_custom_profile_load_json();

    /* Apply HARD SAFETY LIMIT immediately on startup */
    uint64_t hard_limit = get_hard_max_freq_hz();
    LOG_INFO("[CPU] Applying hard safety limit: %" PRIu64 " Hz (%.1f GHz)",
             hard_limit, hard_limit / 1e9);
    set_all_cpus_max_freq(hard_limit);

    /* EPP and intel_pstate min_perf_pct are no longer forced here: the active
     * power mode decides EPP. Versions before 1.17.1 set min_perf_pct=100,
     * which kept every core near its maximum frequency regardless of load.
     * Undo that once; the driver clamps 0 to its own hardware minimum. */
    {
        char pct_buf[16] = "";
        FILE *pf = fopen(CPUSYSFS "/intel_pstate/min_perf_pct", "r");
        if (pf) {
            if (!fgets(pct_buf, sizeof(pct_buf), pf)) pct_buf[0] = '\0';
            fclose(pf);
        }
        if (atoi(pct_buf) == 100) {
            LOG_INFO("[CPU] intel_pstate min_perf_pct is 100 (set by an older version) - restoring the driver minimum");
            set_intel_pstate_min_perf_pct(0);
        }
    }

    return 0;
}

/* --- Cleanup CPU control subsystem --- */
void cpu_control_cleanup(void) {
    LOG_INFO("[CPU] Cleaning up CPU control subsystem");
    /* Profiles are saved by the GUI, not the daemon */
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
    FILE *fp = NULL;

    /* Try global path first (newer kernels) */
    snprintf(path, sizeof(path), "%s/cpufreq/available_governors", CPUSYSFS);
    fp = fopen(path, "r");

    /* Fallback: try per-policy path */
    if (!fp) {
        snprintf(path, sizeof(path), "%s/cpufreq/policy0/scaling_available_governors", CPUSYSFS);
        fp = fopen(path, "r");
    }

    if (!fp) {
        LOG_WARN("[CPU] Could not read available governors from any path");
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
uint64_t get_cpu_min_freq(int cpu_id) {
    return sysfs_read_freq(cpu_id, "cpufreq/scaling_min_freq");
}

uint64_t get_cpu_max_freq(int cpu_id) {
    return sysfs_read_freq(cpu_id, "cpufreq/scaling_max_freq");
}

uint64_t get_cpu_current_freq(int cpu_id) {
    uint64_t freq = sysfs_read_freq(cpu_id, "cpufreq/scaling_cur_freq");
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

int set_cpu_min_freq(int cpu_id, uint64_t freq_hz) {
    return sysfs_write_freq(cpu_id, "cpufreq/scaling_min_freq", freq_hz);
}

int set_cpu_max_freq(int cpu_id, uint64_t freq_hz) {
    return sysfs_write_freq(cpu_id, "cpufreq/scaling_max_freq", freq_hz);
}

/* Set Energy Performance Preference for a single CPU */
int set_cpu_epp(int cpu_id, const char *epp) {
    if (!epp) return -1;
    return sysfs_write_string(cpu_id, "cpufreq/energy_performance_preference", epp);
}

/* Set Energy Performance Preference for all CPUs */
int set_all_cpus_epp(const char *epp) {
    int cpu_count = get_cpu_count();
    int errors = 0;

    for (int i = 0; i < cpu_count; i++) {
        if (set_cpu_epp(i, epp) != 0) {
            errors++;
        }
    }
    if (errors == 0) {
        LOG_INFO("[CPU] Set EPP to '%s' on all %d cores", epp, cpu_count);
    } else {
        LOG_WARN("[CPU] Set EPP to '%s' on %d/%d cores (%d errors)", epp, cpu_count - errors, cpu_count, errors);
    }
    return (errors == 0) ? 0 : -1;
}

/* --- Intel pstate min_perf_pct control ---
 * On Intel HWP (Speed Shift) mode, this tells the firmware what % of max frequency
 * to use as the minimum performance target. Setting to 100 helps the CPU stay
 * near scaling_max_freq during sustained load.
 */
int set_intel_pstate_min_perf_pct(int pct) {
    if (pct < 0 || pct > 100) return -1;

    const char *path = CPUSYSFS "/intel_pstate/min_perf_pct";
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR("[CPU] Failed to open %s for writing: %s", path, strerror(errno));
        return -1;
    }

    int ret = fprintf(fp, "%d", pct);
    if (ret < 0) {
        LOG_ERROR("[CPU] Failed to write to %s: %s", path, strerror(errno));
        fclose(fp);
        return -1;
    }
    fclose(fp);
    LOG_INFO("[CPU] Set intel_pstate min_perf_pct to %d", pct);
    return 0;
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

int set_all_cpus_min_freq(uint64_t freq_hz) {
    int cpu_count = get_cpu_count();
    int errors = 0;

    for (int i = 0; i < cpu_count; i++) {
        if (set_cpu_min_freq(i, freq_hz) != 0) {
            errors++;
        }
    }
    return (errors == 0) ? 0 : -1;
}

int set_all_cpus_max_freq(uint64_t freq_hz) {
    int cpu_count = get_cpu_count();
    int errors = 0;

    for (int i = 0; i < cpu_count; i++) {
        if (set_cpu_max_freq(i, freq_hz) != 0) {
            errors++;
        }
    }
    return (errors == 0) ? 0 : -1;
}

/* Clamp all CPUs to the current hard limit without increasing frequencies */
int clamp_cpus_to_hard_limit(void) {
    uint64_t hard_limit = get_hard_max_freq_hz();
    int cpu_count = get_cpu_count();
    int clamped = 0;

    for (int i = 0; i < cpu_count; i++) {
        uint64_t current_max = get_cpu_max_freq(i);
        if (current_max > hard_limit) {
            if (set_cpu_max_freq(i, hard_limit) == 0) {
                LOG_INFO("[CPU] Clamped core %d max freq from %" PRIu64 " Hz to %" PRIu64 " Hz (hard limit)",
                         i, current_max, hard_limit);
                clamped++;
            } else {
                LOG_WARN("[CPU] Failed to clamp core %d to hard limit %" PRIu64 " Hz",
                         i, hard_limit);
            }
        }
    }
    LOG_INFO("[CPU] Clamped %d/%d cores to hard limit %" PRIu64 " Hz", clamped, cpu_count, hard_limit);
    return 0;
}

/* --- Profile management functions --- */

/* Create a new profile */
int cpu_profile_create(const char *name, CpuGovernor governor,
                       uint64_t min_freq, uint64_t max_freq) {
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
    if (max_freq > get_hard_max_freq_hz()) {
        max_freq = get_hard_max_freq_hz();
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

    LOG_INFO("[CPU] Applying policy '%s'", name);
    if (cpu_apply_governor_and_epp(enum_to_governor(p->governor), p->epp, name) != 0) {
        LOG_ERROR("[CPU] Failed to apply policy '%s'", name);
        return -1;
    }

    /* Set min freq if specified */
    if (p->min_freq_hz > 0) {
        if (set_all_cpus_min_freq(p->min_freq_hz) != 0) {
            LOG_WARN("[CPU] Failed to set min freq to %" PRIu64 " Hz", p->min_freq_hz);
        }
    }

    /* Set max freq if specified (always apply safety limit) */
    uint64_t hard_limit = get_hard_max_freq_hz();
    uint64_t max_freq = p->max_freq_hz > 0 ? p->max_freq_hz : hard_limit;
    if (max_freq > hard_limit) max_freq = hard_limit;
    if (set_all_cpus_max_freq(max_freq) != 0) {
        LOG_WARN("[CPU] Failed to set max freq to %" PRIu64 " Hz", max_freq);
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

        int written = snprintf(entry, sizeof(entry), "%s %s (gov=%s, epp=%s, min=%" PRIu64 " Hz, max=%" PRIu64 " Hz)\n",
                              marker, p->name, enum_to_governor(p->governor),
                              p->epp[0] ? p->epp : "-",
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



/* --- Profile persistence (JSON) --- */


/* The power modes (Performance, Balanced, Power Save) are built in and created
 * by cpu_control_init(); they are not stored on disk. */

/* Set last used profile marker */
int cpu_profile_set_last_used(const char *name) {
    if (!name) return -1;

    strncpy(g_cpu_state.last_used_profile, name, CPU_PROFILE_NAME_MAX - 1);
    g_cpu_state.last_used_profile[CPU_PROFILE_NAME_MAX - 1] = '\0';
    LOG_INFO("[CPU] Last used profile set to '%s'", name);
    return 0;
}

/* --- Custom profile management (in-memory) --- */

/* Create a new custom profile */
int cpu_custom_profile_create(const char *name, CpuGovernor governor,
                              uint64_t default_max_freq_hz, int apply_all_cores) {
    if (!name || g_cpu_state.num_custom_profiles >= MAX_CPU_CUSTOM_PROFILES) {
        return -1;
    }

    /* Check for duplicate name */
    for (int i = 0; i < g_cpu_state.num_custom_profiles; i++) {
        if (strcmp(g_cpu_state.custom_profiles[i].name, name) == 0) {
            LOG_WARN("[CPU] Custom profile '%s' already exists", name);
            return -1;
        }
    }

    /* Apply safety limit */
    if (default_max_freq_hz > get_hard_max_freq_hz()) {
        default_max_freq_hz = get_hard_max_freq_hz();
    }

    CpuCustomProfile *p = &g_cpu_state.custom_profiles[g_cpu_state.num_custom_profiles];
    memset(p, 0, sizeof(CpuCustomProfile));
    strncpy(p->name, name, CPU_PROFILE_NAME_MAX - 1);
    p->governor = governor;
    p->apply_all_cores = apply_all_cores;
    p->num_cores = get_cpu_count();

    /* Set default frequencies for all cores */
    for (int i = 0; i < p->num_cores && i < MAX_CPUS; i++) {
        p->per_core_max_freq_hz[i] = default_max_freq_hz;
        p->per_core_min_freq_hz[i] = 0;  /* 0 = default */
    }

    g_cpu_state.num_custom_profiles++;
    LOG_INFO("[CPU] Created custom profile '%s' (cores=%d, gov=%s)",
             name, p->num_cores, enum_to_governor(governor));
    return 0;
}

/* Delete a custom profile by index */
int cpu_custom_profile_delete(int idx) {
    if (idx < 0 || idx >= g_cpu_state.num_custom_profiles) return -1;

    /* Shift remaining profiles down */
    for (int i = idx; i < g_cpu_state.num_custom_profiles - 1; i++) {
        g_cpu_state.custom_profiles[i] = g_cpu_state.custom_profiles[i + 1];
    }
    g_cpu_state.num_custom_profiles--;

    /* Reset active index if needed */
    if (g_cpu_state.active_custom_profile_idx >= g_cpu_state.num_custom_profiles) {
        g_cpu_state.active_custom_profile_idx = g_cpu_state.num_custom_profiles - 1;
    }

    LOG_INFO("[CPU] Deleted custom profile at index %d", idx);
    return 0;
}

/* Rename a custom profile by index */
int cpu_custom_profile_rename(int idx, const char *new_name) {
    if (idx < 0 || idx >= g_cpu_state.num_custom_profiles) return -1;
    if (new_name == NULL || strlen(new_name) == 0) return -1;

    /* Check for duplicate name */
    for (int i = 0; i < g_cpu_state.num_custom_profiles; i++) {
        if (i != idx && strcmp(g_cpu_state.custom_profiles[i].name, new_name) == 0) {
            LOG_WARN("[CPU] Rename failed: profile '%s' already exists", new_name);
            return -1;
        }
    }

    CpuCustomProfile *p = &g_cpu_state.custom_profiles[idx];
    char old_name[64];
    strncpy(old_name, p->name, sizeof(old_name) - 1);
    old_name[sizeof(old_name) - 1] = '\0';

    strncpy(p->name, new_name, CPU_PROFILE_NAME_MAX - 1);
    p->name[CPU_PROFILE_NAME_MAX - 1] = '\0';

    LOG_INFO("[CPU] Renamed custom profile '%s' to '%s'", old_name, p->name);
    return 0;
}

/* Set per-core frequency in a custom profile */
/* A per-core value below the core's hardware minimum cannot be a real
 * setting (e.g. a number cut off by the old 256-byte GUI command limit).
 * Writing it to sysfs would pin the core at its lowest frequency, so treat
 * it as "not set" (0) instead. */
static uint64_t sanitize_core_freq(int cpu_id, uint64_t hz, const char *what, const char *profile) {
    if (hz == 0) return 0;
    uint64_t hw_min = sysfs_read_freq(cpu_id, "cpufreq/cpuinfo_min_freq");
    if (hw_min == 0) hw_min = 100000000ULL;   /* 100 MHz if unknown */
    if (hz < hw_min) {
        LOG_WARN("[CPU] Profile '%s' core %d: invalid %s frequency %" PRIu64 " Hz "
                 "(below hardware minimum %" PRIu64 " Hz) - ignored, please re-save this profile",
                 profile, cpu_id, what, hz, hw_min);
        return 0;
    }
    return hz;
}

int cpu_custom_profile_set_core_freq(int profile_idx, int cpu_id,
                                     uint64_t max_freq_hz, uint64_t min_freq_hz) {
    if (profile_idx < 0 || profile_idx >= g_cpu_state.num_custom_profiles) return -1;
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) return -1;

    CpuCustomProfile *p = &g_cpu_state.custom_profiles[profile_idx];

    max_freq_hz = sanitize_core_freq(cpu_id, max_freq_hz, "max", p->name);
    min_freq_hz = sanitize_core_freq(cpu_id, min_freq_hz, "min", p->name);

    /* Apply safety limit */
    if (max_freq_hz > get_hard_max_freq_hz()) max_freq_hz = get_hard_max_freq_hz();

    p->per_core_max_freq_hz[cpu_id] = max_freq_hz;
    p->per_core_min_freq_hz[cpu_id] = min_freq_hz;

    LOG_INFO("[CPU] Set core %d freq in profile '%s': max=%" PRIu64 " Hz, min=%" PRIu64 " Hz",
             cpu_id, p->name, max_freq_hz, min_freq_hz);
    return 0;
}

/* Get custom profile by index */
const CpuCustomProfile* cpu_custom_profile_get(int idx) {
    if (idx < 0 || idx >= g_cpu_state.num_custom_profiles) return NULL;
    return &g_cpu_state.custom_profiles[idx];
}

/* Set active custom profile (in memory only) */
int cpu_custom_profile_set_active(int idx) {
    if (idx < 0 || idx >= g_cpu_state.num_custom_profiles) return -1;

    g_cpu_state.active_custom_profile_idx = idx;
    LOG_INFO("[CPU] Active custom profile set to index %d ('%s')",
             idx, g_cpu_state.custom_profiles[idx].name);
    return 0;
}

/* List all custom profiles */
int cpu_custom_profile_list(char *buffer, size_t buf_size) {
    if (!buffer || buf_size == 0) return -1;

    int offset = 0;
    for (int i = 0; i < g_cpu_state.num_custom_profiles; i++) {
        const CpuCustomProfile *p = &g_cpu_state.custom_profiles[i];
        char entry[256];
        const char *marker = (i == g_cpu_state.active_custom_profile_idx) ? " [*]" : "    ";

        int written = snprintf(entry, sizeof(entry), "%s %s (gov=%s, cores=%d, all_cores=%d)\n",
                              marker, p->name, enum_to_governor(p->governor),
                              p->num_cores, p->apply_all_cores);

        if (offset + written >= (int)buf_size) break;
        memcpy(buffer + offset, entry, (size_t)written);
        offset += written;
    }

    return offset;
}

/* Forward declaration for helper below */
static cJSON* cpu_custom_profile_to_json(const CpuCustomProfile *p);

/* Return all custom profiles as a JSON array string */
char* cpu_custom_profile_list_json(void) {
    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < g_cpu_state.num_custom_profiles; i++) {
        const CpuCustomProfile *p = &g_cpu_state.custom_profiles[i];
        cJSON *profile_obj = cpu_custom_profile_to_json(p);
        cJSON_AddItemToArray(array, profile_obj);
    }
    char *json_str = cJSON_Print(array);
    cJSON_Delete(array);
    return json_str; /* Caller must free */
}

/* --- Custom profile JSON persistence (merged into main profiles.json) --- */

/* Helper: build a cJSON object for a single custom profile */
static cJSON* cpu_custom_profile_to_json(const CpuCustomProfile *p) {
    cJSON *profile_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(profile_obj, "name", p->name);
    cJSON_AddStringToObject(profile_obj, "governor", enum_to_governor(p->governor));
    cJSON_AddStringToObject(profile_obj, "epp", p->epp);
    cJSON_AddNumberToObject(profile_obj, "apply_all_cores", p->apply_all_cores);
    cJSON_AddNumberToObject(profile_obj, "num_cores", p->num_cores);

    /* Per-core max frequencies */
    cJSON *max_freq_array = cJSON_CreateArray();
    for (int j = 0; j < p->num_cores && j < MAX_CPUS; j++) {
        cJSON_AddItemToArray(max_freq_array, cJSON_CreateNumber(p->per_core_max_freq_hz[j]));
    }
    cJSON_AddItemToObject(profile_obj, "per_core_max_freq_hz", max_freq_array);

    /* Per-core min frequencies */
    cJSON *min_freq_array = cJSON_CreateArray();
    for (int j = 0; j < p->num_cores && j < MAX_CPUS; j++) {
        cJSON_AddItemToArray(min_freq_array, cJSON_CreateNumber(p->per_core_min_freq_hz[j]));
    }
    cJSON_AddItemToObject(profile_obj, "per_core_min_freq_hz", min_freq_array);

    return profile_obj;
}

/* Save all custom profiles to the main profiles.json (cpu_freq_profiles section) */
int cpu_custom_profile_save_json(void) {
    const char *path = get_main_profiles_path();

    /* Read existing file if it exists, otherwise create empty root */
    /* Read as the file's owner, never through a symlink (see secure.h) */
    cJSON *root = NULL;
    char *file_content = dcc_user_file_read(path, 1024 * 1024, NULL);
    if (file_content) {
        root = cJSON_Parse(file_content);
        free(file_content);
        if (!root) {
            /* Never replace an unparsable file: that would wipe the other sections */
            LOG_ERROR("%s: existing %s is not valid JSON - not saving", __func__, path);
            return -1;
        }
    } else if (errno != ENOENT) {
        LOG_ERROR("%s: cannot read %s - not saving", __func__, path);
        return -1;
    }

    if (!root) {
        root = cJSON_CreateObject();
        if (!root) return -1;
        /* Preserve fan profile version if file didn't exist */
        cJSON_AddStringToObject(root, "version", "1.0");
    }

    /* Remove existing cpu_freq_profiles section if present */
    cJSON_DetachItemFromObject(root, "cpu_freq_profiles");

    /* Build cpu_freq_profiles section */
    cJSON *cpu_section = cJSON_CreateObject();
    cJSON_AddNumberToObject(cpu_section, "active_profile_index", g_cpu_state.active_custom_profile_idx);
    cJSON_AddNumberToObject(cpu_section, "cpu_hard_limit_hz", (double)get_hard_max_freq_hz());

    cJSON *profiles_array = cJSON_CreateArray();
    for (int i = 0; i < g_cpu_state.num_custom_profiles; i++) {
        CpuCustomProfile *p = &g_cpu_state.custom_profiles[i];
        cJSON *profile_obj = cpu_custom_profile_to_json(p);
        cJSON_AddItemToArray(profiles_array, profile_obj);
    }
    cJSON_AddItemToObject(cpu_section, "profiles", profiles_array);
    cJSON_AddItemToObject(root, "cpu_freq_profiles", cpu_section);

    /* Write to file */
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        return -1;
    }

    if (dcc_user_file_write(path, json_str, strlen(json_str)) == 0) {
        LOG_INFO("[CPU] Saved %d custom profiles to %s (cpu_freq_profiles section)",
                 g_cpu_state.num_custom_profiles, path);
    } else {
        LOG_ERROR("[CPU] Failed to write custom profiles to %s: %s", path, strerror(errno));
        cJSON_free(json_str);
        cJSON_Delete(root);
        return -1;
    }

    cJSON_free(json_str);
    cJSON_Delete(root);
    return 0;
}

/* Load all custom profiles from the main profiles.json (cpu_freq_profiles section) */
int cpu_custom_profile_load_json(void) {
    /* Clear any existing profiles to avoid duplicates on reload/restart */
    g_cpu_state.num_custom_profiles = 0;

    const char *path = get_main_profiles_path();

    /* Read as the file's owner, never through a symlink (see secure.h) */
    char *file_content = dcc_user_file_read(path, 1024 * 1024, NULL);
    if (!file_content) {
        LOG_INFO("[CPU] No readable profiles file at %s", path);
        return -1;
    }

    cJSON *root = cJSON_Parse(file_content);
    free(file_content);

    if (!root) {
        LOG_ERROR("[CPU] Failed to parse profiles JSON from %s", path);
        return -1;
    }

    /* Find the cpu_freq_profiles section */
    cJSON *cpu_section = cJSON_GetObjectItem(root, "cpu_freq_profiles");
    if (!cpu_section) {
        LOG_INFO("[CPU] No cpu_freq_profiles section in %s", path);
        cJSON_Delete(root);
        return -1;
    }

    /* Read active index and hard limit from cpu_freq_profiles section (nested format only) */
    if (cJSON_IsObject(cpu_section)) {
        /* Try new key name first, then legacy key name */
        cJSON *active_item = cJSON_GetObjectItem(cpu_section, "active_profile_index");
        if (!active_item) {
            active_item = cJSON_GetObjectItem(cpu_section, "active_custom_profile_index");
        }
        if (active_item) {
            g_cpu_state.active_custom_profile_idx = active_item->valueint;
        }

        /* Read hard limit from JSON */
        cJSON *hard_limit_item = cJSON_GetObjectItem(cpu_section, "cpu_hard_limit_hz");
        if (hard_limit_item && hard_limit_item->valuedouble > 0) {
            uint64_t loaded_limit = (uint64_t)hard_limit_item->valuedouble;
            set_hard_max_freq_hz(loaded_limit);
            LOG_INFO("[CPU] Loaded hard limit from profiles.json: %" PRIu64 " Hz (%.2f GHz)",
                     loaded_limit, loaded_limit / 1e9);
        }
    }

    /* Load profiles from cpu_freq_profiles section
     * Support both nested format (object with inner "profiles" array) and flat format (direct array) */
    cJSON *profiles_array = NULL;
    if (cJSON_IsArray(cpu_section)) {
        /* Flat format: cpu_freq_profiles is directly an array */
        profiles_array = cpu_section;
        LOG_INFO("[CPU] Using flat cpu_freq_profiles format");
    } else if (cJSON_IsObject(cpu_section)) {
        /* Nested format: try new "profiles" key first, then legacy "cpu_freq_profiles" key */
        profiles_array = cJSON_GetObjectItem(cpu_section, "profiles");
        if (!profiles_array) {
            profiles_array = cJSON_GetObjectItem(cpu_section, "cpu_freq_profiles");
        }
    }
    if (profiles_array && cJSON_IsArray(profiles_array)) {
        int count = cJSON_GetArraySize(profiles_array);
        for (int i = 0; i < count && i < MAX_CPU_CUSTOM_PROFILES; i++) {
            cJSON *profile_obj = cJSON_GetArrayItem(profiles_array, i);
            if (!profile_obj) continue;

            CpuCustomProfile p = {0};

            cJSON *name_item = cJSON_GetObjectItem(profile_obj, "name");
            if (name_item && name_item->valuestring) {
                strncpy(p.name, name_item->valuestring, CPU_PROFILE_NAME_MAX - 1);
            } else {
                continue;
            }

            cJSON *gov_item = cJSON_GetObjectItem(profile_obj, "governor");
            if (gov_item && gov_item->valuestring) {
                governor_to_enum(gov_item->valuestring, &p.governor);
            }
            cJSON *epp_item = cJSON_GetObjectItem(profile_obj, "epp");
            if (cJSON_IsString(epp_item) && epp_item->valuestring) {
                snprintf(p.epp, sizeof(p.epp), "%s", epp_item->valuestring);
            }

            cJSON *apply_all_item = cJSON_GetObjectItem(profile_obj, "apply_all_cores");
            if (apply_all_item) p.apply_all_cores = apply_all_item->valueint;

            cJSON *num_cores_item = cJSON_GetObjectItem(profile_obj, "num_cores");
            if (num_cores_item) p.num_cores = num_cores_item->valueint;

            /* Load per-core max frequencies */
            cJSON *max_freq_array = cJSON_GetObjectItem(profile_obj, "per_core_max_freq_hz");
            if (max_freq_array) {
                int max_count = cJSON_GetArraySize(max_freq_array);
                for (int j = 0; j < max_count && j < MAX_CPUS; j++) {
                    cJSON *freq_item = cJSON_GetArrayItem(max_freq_array, j);
                    if (freq_item) {
                        p.per_core_max_freq_hz[j] = (uint64_t)freq_item->valuedouble;
                    }
                }
            }

            /* Load per-core min frequencies */
            cJSON *min_freq_array = cJSON_GetObjectItem(profile_obj, "per_core_min_freq_hz");
            if (min_freq_array) {
                int min_count = cJSON_GetArraySize(min_freq_array);
                for (int j = 0; j < min_count && j < MAX_CPUS; j++) {
                    cJSON *freq_item = cJSON_GetArrayItem(min_freq_array, j);
                    if (freq_item) {
                        p.per_core_min_freq_hz[j] = (uint64_t)freq_item->valuedouble;
                    }
                }
            }

            if (!cpu_governor_available(enum_to_governor(p.governor))) {
                const char *alt = cpu_governor_resolve(enum_to_governor(p.governor));
                LOG_WARN("[CPU] Profile '%s' uses governor '%s', which this system does not offer - "
                         "'%s' will be used; re-save the profile to store it",
                         p.name, enum_to_governor(p.governor), alt ? alt : "none");
            }

            /* Drop impossible per-core values saved by older versions */
            for (int j = 0; j < p.num_cores && j < MAX_CPUS; j++) {
                p.per_core_max_freq_hz[j] = sanitize_core_freq(j, p.per_core_max_freq_hz[j], "max", p.name);
                p.per_core_min_freq_hz[j] = sanitize_core_freq(j, p.per_core_min_freq_hz[j], "min", p.name);
            }

            g_cpu_state.custom_profiles[g_cpu_state.num_custom_profiles] = p;
            g_cpu_state.num_custom_profiles++;
        }
    }

    cJSON_Delete(root);
    LOG_INFO("[CPU] Loaded %d custom profiles from %s (cpu_freq_profiles section)",
             g_cpu_state.num_custom_profiles, path);
    return 0;
}

/* Apply a custom profile by index (set per-core frequencies) */
int cpu_custom_profile_apply(int idx) {
    if (idx < 0 || idx >= g_cpu_state.num_custom_profiles) return -1;

    CpuCustomProfile *p = &g_cpu_state.custom_profiles[idx];
    LOG_INFO("[CPU] Applying custom profile '%s' (index %d)", p->name, idx);

    /* Set governor (non-fatal: continue with frequencies even if governor fails).
     * A governor this system does not offer is replaced by the closest one. */
    if (cpu_apply_governor_and_epp(enum_to_governor(p->governor), p->epp, p->name) != 0) {
        LOG_WARN("[CPU] Continuing with frequencies for profile '%s' despite governor error", p->name);
    }

    /* Set per-core frequencies */
    int errors = 0;
    for (int i = 0; i < p->num_cores && i < MAX_CPUS; i++) {
        if (p->per_core_max_freq_hz[i] > 0) {
            uint64_t max_freq = p->per_core_max_freq_hz[i];
            if (max_freq > get_hard_max_freq_hz()) max_freq = get_hard_max_freq_hz();
            if (set_cpu_max_freq(i, max_freq) != 0) {
                errors++;
            }
        }

        if (p->per_core_min_freq_hz[i] > 0) {
            if (set_cpu_min_freq(i, p->per_core_min_freq_hz[i]) != 0) {
                errors++;
            }
        }
    }

    /* Set as active */
    cpu_custom_profile_set_active(idx);

    if (errors > 0) {
        LOG_WARN("[CPU] Applied custom profile '%s' with %d errors", p->name, errors);
    } else {
        LOG_INFO("[CPU] Successfully applied custom profile '%s'", p->name);
    }

    return (errors == 0) ? 0 : -1;
}

