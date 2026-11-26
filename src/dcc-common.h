// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc-common.h
 * Common header for Dudes Control Center applications
 */

#ifndef DUDES_CONTROL_CENTER_COMMON_H
#define DUDES_CONTROL_CENTER_COMMON_H

#include <stdint.h>
#include <stddef.h>

/* --- EC I/O ports and commands --- */
#define EC_SC 0x66
#define EC_DATA 0x62
#define IBF 1
#define OBF 0
#define EC_SC_READ_CMD 0x80

/* --- Common EC register layout (typical Clevo) --- */
#define EC_REG_CPU_TEMP       0x07
#define EC_REG_GPU_TEMP       0xCD
#define EC_REG_FAN1_DUTY      0xCE
#define EC_REG_FAN2_DUTY      0xCF
#define EC_REG_FAN1_RPM_HI    0xD0
#define EC_REG_FAN1_RPM_LO    0xD1
#define EC_REG_FAN2_RPM_HI    0xD2
#define EC_REG_FAN2_RPM_LO    0xD3

/* --- Plausibility rules --- */
#define TEMP_MIN_VALID_C      10
#define TEMP_MAX_VALID_C      110
#define GPU_CPU_MAX_DIFF_C     40

/* --- RPM to percentage conversion --- */

/* --- Function prototypes for EC access --- */
/* EC access: the dcc_fan kernel module's hwmon device when it is loaded
 * (no root needed for reading), else port I/O 0x62/0x66 (root) */
/* Direct port I/O only after the opt-in (1.41.0): on a laptop with another
 * EC layout, writes to 0x62/0x66 can hit registers for charging, power or
 * thermal limits. The opt-in is the file DCC_EC_DIRECT_FILE containing
 * "enabled", written by "dcc-component ec-direct enable" (root). */
#ifndef DCC_EC_DIRECT_FILE
#define DCC_EC_DIRECT_FILE "/etc/dudes-control-center/ec-direct-access"
#endif
enum { EC_INIT_OK = 0, EC_INIT_ERROR = -1, EC_INIT_DISABLED = -2, EC_INIT_IMPLAUSIBLE = -3 };
int ec_init(void);                 /* EC_INIT_*: module, or opt-in + plausible ports */
void ec_cleanup(void);
int ec_backend_module(void);       /* 1: dcc_fan module, 0: port I/O or none */
const char *ec_backend_name(void); /* "module", "ports" or "none" */
const char *ec_init_reason(void);  /* why direct access is not used ("" if it is) */
int ec_direct_allowed(void);       /* the opt-in file says "enabled" */
int cpu_temp_hwmon(void);          /* coretemp/k10temp package temperature, -1 if none */
int ec_values_plausible(const int t[3], int ref, int r1, int r2, char *why, size_t n);   /* 1: Clevo-like */
int fan_keepalive(void);           /* module: rewrite the last duty (watchdog) */
uint8_t ec_io_read(const uint32_t port);                       /* returns 0 if the EC does not answer */
int ec_io_read_checked(const uint32_t port, uint8_t *value);  /* 0 on success, -1 on EC timeout */
int ec_io_do(const uint32_t cmd, const uint32_t port, const uint8_t value);

/* --- NVIDIA GPU (1.37.0): never wakes a powered-down GPU --- */
enum { GPU_NONE = 0, GPU_SLEEPING = 1, GPU_AWAKE = 2 };
typedef struct {
    int state;                 /* GPU_NONE, GPU_SLEEPING, GPU_AWAKE */
    char name[64];
    int temp;                  /* degC, -1 unknown */
    double power_w;            /* -1 unknown */
    int clock_mhz, mem_clock_mhz, util, vram_used_mb, vram_total_mb;   /* -1 unknown */
    char pstate[8];
} GpuInfo;
int gpu_info_get(GpuInfo *out);   /* returns state; values only when awake (cached 3 s) */

/* --- Temperature reading functions --- */
int get_cpu_temp(void);           /* Returns CPU temperature in °C, or -1 on error */
int get_gpu_temp(void);           /* Returns GPU temperature in °C, or -1 on error */

/* --- Fan control functions --- */
/* Fan duty as set in the EC (registers 0xCE/0xCF, 0-255) in percent 0-100,
 * or -1 if the EC does not answer. This is what the fan was told to run at;
 * exact on every model, unlike a percentage computed from RPM. */
int get_cpu_fan_duty(void);
int get_gpu_fan_duty(void);
int get_fan_duty(void);           /* = get_cpu_fan_duty() (both fans get the same duty) */
int get_cpu_fan_rpm(void);        /* Returns CPU fan RPM, or 0 on error */
int get_gpu_fan_rpm(void);        /* Returns GPU fan RPM, or 0 on error */
int set_fan_duty(int pct);        /* Set fan duty 0-100%, returns 0 on success, -1 on error */

/* --- Utility functions --- */
/* Is an EC duty reading believable? Some ECs (e.g. ones driven through
 * command 0x99) leave 0xCE/0xCF at 0 while the fan spins: then it is not. */
int fan_duty_plausible(int duty, int rpm);
/* "CPU Fan: 3011 RPM (50%)" - RPM measured, percent = EC duty (duty < 0: unknown) */
void format_fan_text(char *buf, size_t size, const char *name, int rpm, int duty);
int clamp(int v, int lo, int hi);

/* --- Shared config path (used by all daemons and GUI) --- */
/* profiles.json of the desktop user: dcc_profiles_path() (secure.h).
 * MAIN_PROFILES_PATH can be defined at build time to force a path (tests). */
#define CPU_CONFIG_DIR "dudes-control-center/cpu"

/* --- Daemon socket communication --- */
/* Send a command to the daemon and receive a response.
 * Returns 0 on success, -1 on failure.
 * Response is written to resp (up to resp_size bytes). */
int daemon_send_command(const char *cmd, char *resp, size_t resp_size);

#endif /* DUDES_CONTROL_CENTER_COMMON_H */
