/*
 * dudes-control-center-common.h
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
#define CPU_FAN_RPM_AT_100_PERCENT    6022   /* CPU fan max RPM */
#define GPU_FAN_RPM_AT_100_PERCENT    5859   /* GPU fan max RPM */

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
int get_fan_duty(void);           /* Returns current fan duty 0-100%, or -1 on error */
int get_cpu_fan_rpm(void);        /* Returns CPU fan RPM, or 0 on error */
int get_gpu_fan_rpm(void);        /* Returns GPU fan RPM, or 0 on error */
int set_fan_duty(int pct);        /* Set fan duty 0-100%, returns 0 on success, -1 on error */

/* --- Utility functions --- */
int rpm_to_percentage(int rpm, int is_gpu);  /* Convert RPM to percentage (0-100), is_gpu=1 for GPU fan */
int clamp(int v, int lo, int hi);

/* --- Shared config path (used by all daemons and GUI) --- */
#ifndef MAIN_PROFILES_PATH
#define MAIN_PROFILES_PATH "/home/dude/.config/dudes-control-center/profiles.json"
#endif
#define CPU_CONFIG_DIR "dudes-control-center/cpu"

/* --- Daemon socket communication --- */
/* Send a command to the daemon and receive a response.
 * Returns 0 on success, -1 on failure.
 * Response is written to resp (up to resp_size bytes). */
int daemon_send_command(const char *cmd, char *resp, size_t resp_size);

#endif /* DUDES_CONTROL_CENTER_COMMON_H */
