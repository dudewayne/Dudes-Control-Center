/*
 * dudes-control-center-cpu-common.h
 * Common header for Dudes Control Center CPU Speed Control subsystem
 */

#ifndef DUDES_CONTROL_CENTER_CPU_COMMON_H
#define DUDES_CONTROL_CENTER_CPU_COMMON_H

#include <stdint.h>
#include <stddef.h>

/* --- Socket configuration --- */
#define CPU_SOCKET_PATH "/run/dudes-control-center-cpu.sock"
#define CPU_BUFFER_SIZE 4096
#define CPU_MAX_CMD_LEN 256
#define CPU_MAX_RESPONSE_LEN 8192
#define CPU_MAX_CLIENTS 10

/* --- CPU and Profile configuration --- */
#define MAX_CPUS 64
#define MAX_CPU_PROFILES 32
#define MAX_CPU_CUSTOM_PROFILES 16
#define CPU_PROFILE_NAME_MAX 64
#define CPU_GOVERNOR_NAME_MAX 32
#define CPU_CONDITION_TYPE_MAX 32

/* --- Config directory --- */
/* Resolved at runtime: ~/.config/dudes-control-center/cpu/ */
#define CPU_CONFIG_DIR "dudes-control-center/cpu"

/* --- HARD SAFETY LIMIT: Maximum CPU frequency cap (3.6 GHz) --- */
#define HARD_MAX_FREQ_HZ 3600000000U  /* 3600 MHz = 3.6 GHz in Hz */

/* --- CPU Governor types --- */
typedef enum {
    CPU_GOVERNOR_UNKNOWN = -1,
    CPU_GOVERNOR_PERFORMANCE = 0,
    CPU_GOVERNOR_POWERSAVE = 1,
    CPU_GOVERNOR_USERSPACE = 2,
    CPU_GOVERNOR_ONDEMAND = 3,
    CPU_GOVERNOR_CONSERVATIVE = 4,
    CPU_GOVERNOR_SCALING_SAVINGS = 5,
    CPU_GOVERNOR_SCHEDUTIL = 6,
    CPU_GOVERNOR_EPP = 7
} CpuGovernor;

/* --- Condition types for automatic profile switching --- */
typedef enum {
    CONDITION_NONE = 0,
    CONDITION_AC_POWER = 1,      /* Switch when on AC power */
    CONDITION_BATTERY = 2,       /* Switch when on battery */
    CONDITION_TEMP_HIGH = 3,     /* Switch when CPU temp > threshold */
    CONDITION_TEMP_LOW = 4,      /* Switch when CPU temp < threshold */
    CONDITION_LOAD_HIGH = 5,     /* Switch when CPU load > threshold */
    CONDITION_LOAD_LOW = 6       /* Switch when CPU load < threshold */
} ConditionType;

/* --- Profile structure (Governor Policies) --- */
typedef struct {
    char name[CPU_PROFILE_NAME_MAX];
    CpuGovernor governor;
    uint32_t min_freq_hz;     /* Minimum frequency in Hz (0 = auto) */
    uint32_t max_freq_hz;     /* Maximum frequency in Hz (0 = auto) */

    /* Automatic switching conditions */
    ConditionType condition_type;
    int condition_threshold;  /* Threshold value for temp/load conditions */
    int is_auto_switchable;   /* Can this profile be switched automatically? */
} CpuProfile;

/* --- Custom Profile structure (per-core frequency control, JSON storage) --- */
typedef struct {
    char name[CPU_PROFILE_NAME_MAX];
    CpuGovernor governor;
    uint32_t per_core_max_freq_hz[MAX_CPUS];  /* Max freq per core in Hz (0 = no limit) */
    uint32_t per_core_min_freq_hz[MAX_CPUS];  /* Min freq per core in Hz (0 = default) */
    int apply_all_cores;          /* 1 = use single freq for all cores, 0 = per-core */
    int num_cores;                /* Number of cores this profile was created for */
} CpuCustomProfile;

/* --- CPU information structure --- */
typedef struct {
    int cpu_id;
    uint32_t current_freq_hz;
    uint32_t min_freq_hz;
    uint32_t max_freq_hz;
    char governor[CPU_GOVERNOR_NAME_MAX];
    int online;               /* 1 if CPU is online, 0 if offline */
} CpuInfo;

/* --- System status structure --- */
typedef struct {
    int num_cpus;
    CpuInfo cpus[MAX_CPUS];
    int on_ac_power;          /* 1 if on AC, 0 if on battery */
    int cpu_temp_c;           /* Current CPU temperature in °C (-1 if unavailable) */
    double cpu_load_percent;  /* Current CPU load percentage (0-100) */
} CpuSystemStatus;

/* --- Global state structure --- */
typedef struct {
    CpuProfile profiles[MAX_CPU_PROFILES];
    int num_profiles;
    int active_profile_idx;   /* Index of currently active profile */
    int auto_mode_enabled;    /* Automatic profile switching enabled? */
    char last_used_profile[CPU_PROFILE_NAME_MAX];  /* Last used profile name */

    /* Custom profiles (per-core frequency control, JSON storage) */
    CpuCustomProfile custom_profiles[MAX_CPU_CUSTOM_PROFILES];
    int num_custom_profiles;
    int active_custom_profile_idx;  /* Index of currently active custom profile */
} CpuControlState;

/* --- Function prototypes for CPU access --- */

/* Initialization and cleanup */
int cpu_control_init(void);                           /* Initialize CPU control subsystem */
void cpu_control_cleanup(void);                       /* Cleanup CPU control subsystem */

/* CPU information retrieval */
int get_cpu_count(void);                              /* Get number of CPUs */
const CpuInfo* get_cpu_info(int cpu_id);              /* Get info for specific CPU */
int get_all_governors(char **governor_list, int *count);  /* Get list of available governors */
int get_current_governor(int cpu_id, char *buffer, size_t buf_size);  /* Get current governor */

/* Frequency control */
uint32_t get_cpu_min_freq(int cpu_id);                 /* Get minimum frequency in Hz */
uint32_t get_cpu_max_freq(int cpu_id);                 /* Get maximum frequency in Hz */
uint32_t get_cpu_current_freq(int cpu_id);             /* Get current frequency in Hz */
int set_cpu_governor(int cpu_id, const char *governor);  /* Set governor for CPU */
int set_cpu_min_freq(int cpu_id, uint32_t freq_hz);    /* Set minimum frequency */
int set_cpu_max_freq(int cpu_id, uint32_t freq_hz);    /* Set maximum frequency */
int set_all_cpus_governor(const char *governor);       /* Set governor for all CPUs */
int set_all_cpus_min_freq(uint32_t freq_hz);           /* Set min freq for all CPUs */
int set_all_cpus_max_freq(uint32_t freq_hz);           /* Set max freq for all CPUs */

/* System status */
int get_cpu_system_status(CpuSystemStatus *status);    /* Get complete system status */
int is_on_ac_power(void);                              /* Check if on AC power */
int get_cpu_temperature(void);                         /* Get CPU temperature in °C (-1 if unavailable) */
double get_cpu_load_percent(void);                     /* Get CPU load percentage (0-100) */

/* Profile management */
int cpu_profile_create(const char *name, CpuGovernor governor,
                       uint32_t min_freq, uint32_t max_freq);  /* Create new profile */
int cpu_profile_delete(const char *name);               /* Delete a profile */
int cpu_profile_apply(const char *name);                /* Apply a profile to all CPUs */
int cpu_profile_set_active(const char *name);           /* Set active profile (in memory) */
const CpuProfile* cpu_profile_get(const char *name);    /* Get profile by name */
const CpuProfile* cpu_profile_get_active(void);         /* Get currently active profile */
int cpu_profile_list(char *buffer, size_t buf_size);    /* List all profiles */

/* Profile conditions (for auto-switching) */
int cpu_profile_set_condition(const char *name, ConditionType type, int threshold);  /* Set condition for profile */
void cpu_profile_clear_condition(const char *name);     /* Clear condition from profile */

/* Profile persistence */
int cpu_profiles_save_all(void);                        /* Save all profiles to disk */
int cpu_profiles_load_all(void);                        /* Load all profiles from disk */
int cpu_profile_set_last_used(const char *name);        /* Set last used profile marker */

/* Custom profile management (JSON storage, per-core frequency control) */
int cpu_custom_profile_create(const char *name, CpuGovernor governor,
                              uint32_t default_max_freq_hz, int apply_all_cores);  /* Create new custom profile */
int cpu_custom_profile_delete(int idx);                  /* Delete a custom profile by index */
int cpu_custom_profile_rename(int idx, const char *new_name);  /* Rename a custom profile by index */
int cpu_custom_profile_save_json(void);                  /* Save all custom profiles to JSON file */
int cpu_custom_profile_load_json(void);                  /* Load all custom profiles from JSON file */
int cpu_custom_profile_apply(int idx);                   /* Apply a custom profile by index (set per-core frequencies) */
int cpu_custom_profile_set_core_freq(int profile_idx, int cpu_id,
                                     uint32_t max_freq_hz, uint32_t min_freq_hz);  /* Set per-core freq in profile */
int cpu_custom_profile_list(char *buffer, size_t buf_size);  /* List all custom profiles */
const CpuCustomProfile* cpu_custom_profile_get(int idx); /* Get custom profile by index */
int cpu_custom_profile_set_active(int idx);              /* Set active custom profile (in memory) */

/* Utility functions */
int governor_to_enum(const char *governor_str, CpuGovernor *gov);  /* Convert governor string to enum */
const char* enum_to_governor(CpuGovernor gov);           /* Convert enum to governor string */
uint32_t parse_frequency(const char *str);               /* Parse "100MHz" or "2.5GHz" to Hz */
void format_frequency(uint32_t freq_hz, char *buf, size_t buf_size);  /* Format Hz to human-readable */

/* Exposed global state for read access */
extern CpuControlState g_cpu_state;

#endif /* DUDES_CONTROL_CENTER_CPU_COMMON_H */
