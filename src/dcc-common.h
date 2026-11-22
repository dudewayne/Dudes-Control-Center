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
int ec_init(void);
void ec_cleanup(void);
uint8_t ec_io_read(const uint32_t port);                       /* returns 0 if the EC does not answer */
int ec_io_read_checked(const uint32_t port, uint8_t *value);  /* 0 on success, -1 on EC timeout */
int ec_io_do(const uint32_t cmd, const uint32_t port, const uint8_t value);

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
