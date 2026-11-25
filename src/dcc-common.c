// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc-common.c
 * Common implementation for Dudes Control Center applications
 *
 * The Embedded Controller access (ec_io_*, temperature/fan registers,
 * command 0x99, RPM formula) is based on fan-control by steiiin, a fork of
 * clevo-fan-control by agramian (GPL-2.0), see COPYRIGHT.md.
 */

#include "dcc-common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/io.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#define DAEMON_SOCKET_PATH "/run/dcc-fan.sock"

/* --- dcc_fan kernel module (1.36.0) ---
 * When the module is loaded, its hwmon device "dcc_fan" is used instead of
 * port I/O: the kernel reads the EC under the ACPI EC driver's lock (no race
 * with the firmware), no ioperm is needed (kernel lockdown, Secure Boot),
 * the real fan duty comes from Clevo WMI, and the fan speed is set through
 * WMI. Reading needs no root; writing pwm1/pwm2 does. */
#ifndef DCC_HWMON_DIR
#define DCC_HWMON_DIR "/sys/class/hwmon"
#endif
static char g_kmod[512];        /* hwmon dir of dcc_fan, "" = port I/O */
static int g_ports_ok;          /* ioperm done */
static int g_last_raw = -1;     /* last duty written (0-255), for fan_keepalive */

static int kmod_find(void) {
    DIR *d = opendir(DCC_HWMON_DIR);
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    while (!found && (de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "hwmon", 5) != 0) continue;
        char path[512], name[32] = "";
        snprintf(path, sizeof path, "%s/%s/name", DCC_HWMON_DIR, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = '\0';
        fclose(f);
        if (strcmp(name, "dcc_fan") == 0) {
            snprintf(g_kmod, sizeof g_kmod, "%s/%s", DCC_HWMON_DIR, de->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

static int ports_init(void) {
    if (g_ports_ok) return 0;
    if (ioperm(EC_DATA, 1, 1) != 0) {
        fprintf(stderr, "Error: Cannot set I/O permission for port 0x%X\n", EC_DATA);
        return -1;
    }
    if (ioperm(EC_SC, 1, 1) != 0) {
        fprintf(stderr, "Error: Cannot set I/O permission for port 0x%X\n", EC_SC);
        return -1;
    }
    g_ports_ok = 1;
    return 0;
}

/* The module went away (rmmod, package update): back to port I/O */
static void kmod_lost(void) {
    if (!g_kmod[0]) return;
    g_kmod[0] = '\0';
    if (kmod_find()) return;                  /* reloaded (package update): new hwmonN */
    fprintf(stderr, "dcc_fan module no longer available, using port I/O\n");
    ports_init();
}

static int kmod_read(const char *attr, long *val) {
    char path[600], buf[32];
    snprintf(path, sizeof path, "%s/%s", g_kmod, attr);
    FILE *f = fopen(path, "r");
    if (!f) { if (errno == ENOENT) kmod_lost(); return -1; }
    int ok = fgets(buf, sizeof buf, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    char *end = NULL;
    *val = strtol(buf, &end, 10);
    return end == buf ? -1 : 0;
}

static int kmod_write(const char *attr, long val) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", g_kmod, attr);
    FILE *f = fopen(path, "w");
    if (!f) { if (errno == ENOENT) kmod_lost(); return -1; }
    int ok = fprintf(f, "%ld\n", val) > 0;
    if (fclose(f) != 0) ok = 0;   /* the module reports errors on close/flush */
    return ok ? 0 : -1;
}

int ec_backend_module(void) { return g_kmod[0] != '\0'; }

/* --- EC I/O helper functions --- */

static int ec_io_wait(const uint32_t port, const uint32_t flag, const char value) {
    uint8_t data = inb(port);
    int i = 0;
    while ((((data >> flag) & 0x1) != value) && (i++ < 100)) {
        usleep(1000);
        data = inb(port);
    }
    /* Decide on the final state, not the counter (the last poll may succeed) */
    return (((data >> flag) & 0x1) == value) ? 0 : -1;
}

int ec_init(void) {
    if (kmod_find()) return 0;
    return ports_init();
}

void ec_cleanup(void) {
    if (g_ports_ok) {
        ioperm(EC_DATA, 1, 0);
        ioperm(EC_SC, 1, 0);
        g_ports_ok = 0;
    }
}

int ec_io_read_checked(const uint32_t port, uint8_t *value) {
    /* Every handshake step must succeed; otherwise the byte on the data
     * port is stale or garbage and must not be used. */
    if (ec_io_wait(EC_SC, IBF, 0) != 0) return -1;
    outb(EC_SC_READ_CMD, EC_SC);

    if (ec_io_wait(EC_SC, IBF, 0) != 0) return -1;
    outb(port, EC_DATA);

    if (ec_io_wait(EC_SC, OBF, 1) != 0) return -1;
    *value = inb(EC_DATA);
    return 0;
}

uint8_t ec_io_read(const uint32_t port) {
    uint8_t value = 0;
    if (ec_io_read_checked(port, &value) != 0) {
        return 0;  /* callers treat 0 as "no data" (RPM, duty) */
    }
    return value;
}

int ec_io_do(const uint32_t cmd, const uint32_t port, const uint8_t value) {
    if (ec_io_wait(EC_SC, IBF, 0) != 0) return -1;
    outb(cmd, EC_SC);

    if (ec_io_wait(EC_SC, IBF, 0) != 0) return -1;
    outb(port, EC_DATA);

    if (ec_io_wait(EC_SC, IBF, 0) != 0) return -1;
    outb(value, EC_DATA);

    return ec_io_wait(EC_SC, IBF, 0);
}

/* --- Temperature reading functions --- */

int get_cpu_temp(void) {
    uint8_t temp = 0;
    long mc;
    if (ec_backend_module() && kmod_read("temp1_input", &mc) == 0) {
        int t = (int)(mc / 1000);
        return (t < TEMP_MIN_VALID_C || t > TEMP_MAX_VALID_C) ? -1 : t;
    }
    if (ec_backend_module() || ec_io_read_checked(EC_REG_CPU_TEMP, &temp) != 0) {
        return -1; /* EC did not answer */
    }
    if (temp < TEMP_MIN_VALID_C || temp > TEMP_MAX_VALID_C) {
        return -1; /* Invalid temperature */
    }
    return (int)temp;
}

/* Helper: Check if path exists */
static int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Helper: Read GPU temp from sysfs hwmon interface */
static int gpu_temp_sysfs(void) {
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return -1;

    int best = -1;
    int rootfd = dirfd(d);
    if (rootfd < 0) { closedir(d); return -1; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "hwmon", 5) != 0) continue;

        int hwfd = openat(rootfd, de->d_name, O_RDONLY | O_DIRECTORY);
        if (hwfd < 0) continue;

        /* Read "<hwmon>/name" */
        int namefd = openat(hwfd, "name", O_RDONLY);
        if (namefd < 0) { close(hwfd); continue; }

        char namebuf[64] = {0};
        ssize_t n = read(namefd, namebuf, sizeof(namebuf) - 1);
        close(namefd);
        if (n <= 0) { close(hwfd); continue; }
        for (ssize_t i = 0; i < n; i++) if (namebuf[i] == '\n') { namebuf[i] = 0; break; }

        /* Look for GPU-related sensors */
        if (!(strstr(namebuf, "nvidia") || strstr(namebuf, "amdgpu") ||
              strstr(namebuf, "i915")   || strstr(namebuf, "xe"))) {
            close(hwfd);
            continue;
        }

        /* Try temp1_input .. temp10_input */
        for (int i = 1; i <= 10; i++) {
            char fn[32];
            int m = snprintf(fn, sizeof(fn), "temp%d_input", i);
            if (m < 0 || (size_t)m >= sizeof(fn)) continue;

            int tfd = openat(hwfd, fn, O_RDONLY);
            if (tfd < 0) continue;

            char buf[32] = {0};
            ssize_t r = read(tfd, buf, sizeof(buf) - 1);
            close(tfd);
            if (r <= 0) continue;

            int milli = atoi(buf);          /* millidegC */
            if (milli > 0) {
                int c = milli / 1000;       /* °C */
                if (c > best) best = c;     /* pick the hottest as GPU temp */
            }
        }

        close(hwfd);
    }

    closedir(d);
    return best;
}

/* Helper: Read GPU temp from nvidia-smi */
static int gpu_temp_nvidia_smi(void) {
    if (!path_exists("/usr/bin/nvidia-smi")) return -1;

    FILE *p = popen("/usr/bin/nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null", "r");
    if (!p) return -1;

    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), p)) {
        pclose(p);
        return -1;
    }
    pclose(p);

    int t = atoi(buf);
    if (t > 0 && t < 130) return t;
    return -1;
}

int get_gpu_temp(void) {
    /* Prefer driver temps for GPU */
    int temp = gpu_temp_sysfs();
    if (temp > 0 && temp <= TEMP_MAX_VALID_C) {
        return temp;
    }

    temp = gpu_temp_nvidia_smi();
    if (temp > 0 && temp <= TEMP_MAX_VALID_C) {
        return temp;
    }

    /* Fallback to EC register */
    uint8_t ec_temp = 0;
    long mc;
    if (ec_backend_module()) {
        if (kmod_read("temp2_input", &mc) == 0 && mc / 1000 >= TEMP_MIN_VALID_C && mc / 1000 <= TEMP_MAX_VALID_C)
            return (int)(mc / 1000);
        return -1;
    }
    if (ec_io_read_checked(EC_REG_GPU_TEMP, &ec_temp) == 0 &&
        ec_temp >= TEMP_MIN_VALID_C && ec_temp <= TEMP_MAX_VALID_C) {
        return (int)ec_temp;
    }

    return -1; /* No valid GPU temperature found */
}

/* --- Fan control functions --- */

static int clamp_impl(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int fan_duty_pct(uint32_t reg) {
    uint8_t raw = 0;
    if (ec_backend_module()) {     /* the duty the firmware really runs (WMI) */
        long v;
        if (kmod_read(reg == EC_REG_FAN1_DUTY ? "pwm1" : "pwm2", &v) != 0) return -1;
        return ((int)v * 100 + 127) / 255;
    }
    if (ec_io_read_checked(reg, &raw) != 0) return -1;
    return ((int)raw * 100 + 127) / 255;     /* 0-255 -> 0-100, rounded */
}

int get_cpu_fan_duty(void) { return fan_duty_pct(EC_REG_FAN1_DUTY); }
int get_gpu_fan_duty(void) { return fan_duty_pct(EC_REG_FAN2_DUTY); }
int get_fan_duty(void)     { return get_cpu_fan_duty(); }

int fan_duty_plausible(int duty, int rpm) {
    if (duty < 0 || duty > 100) return 0;
    return !(duty == 0 && rpm >= 300);   /* 0% but clearly spinning: register not used */
}

void format_fan_text(char *buf, size_t size, const char *name, int rpm, int duty) {
    if (duty >= 0 && duty <= 100)
        snprintf(buf, size, "%s: %d RPM (%d%%)", name, rpm < 0 ? 0 : rpm, duty);
    else
        snprintf(buf, size, "%s: %d RPM", name, rpm < 0 ? 0 : rpm);
}

int get_cpu_fan_rpm(void) {
    long v;
    if (ec_backend_module()) return kmod_read("fan1_input", &v) == 0 ? (int)v : 0;
    /* Read CPU fan (fan1) RPM */
    int hi = (int)ec_io_read(EC_REG_FAN1_RPM_HI);
    int lo = (int)ec_io_read(EC_REG_FAN1_RPM_LO);
    int raw = (hi << 8) | lo;

    if (raw > 0) {
        return (2156220 / raw);
    }
    return 0;
}

int get_gpu_fan_rpm(void) {
    long v;
    if (ec_backend_module()) return kmod_read("fan2_input", &v) == 0 ? (int)v : 0;
    /* Read GPU fan (fan2) RPM */
    int hi = (int)ec_io_read(EC_REG_FAN2_RPM_HI);
    int lo = (int)ec_io_read(EC_REG_FAN2_RPM_LO);
    int raw = (hi << 8) | lo;

    if (raw > 0) {
        return (2156220 / raw);
    }
    return 0;
}

int set_fan_duty(int pct) {
    pct = clamp_impl(pct, 0, 100);
    int v = (int)(pct / 100.0 * 255.0 + 0.5);

    if (ec_backend_module()) {
        if (kmod_write("pwm1", v) != 0 || kmod_write("pwm2", v) != 0) {
            if (ec_backend_module()) return -1;   /* module there, write refused */
            /* module gone: fall through to port I/O */
        } else {
            g_last_raw = v;
            usleep(500 * 1000);
            return 0;
        }
    }
    if (!g_ports_ok && ports_init() != 0) return -1;

    /* Set both fans to the same duty */
    if (ec_io_do(0x99, 0x01, (uint8_t)v) != 0) {
        return -1;
    }
    if (ec_io_do(0x99, 0x02, (uint8_t)v) != 0) {
        return -1;
    }

    /* Small delay for the change to take effect */
    usleep(500 * 1000);
    g_last_raw = v;
    return 0;
}

/* Write the last duty again, so the module's watchdog knows the controlling
 * program is alive (it sets 100% after watchdog_sec without a write) */
int fan_keepalive(void) {
    if (!ec_backend_module() || g_last_raw < 0) return 0;
    if (kmod_write("pwm1", g_last_raw) != 0 || kmod_write("pwm2", g_last_raw) != 0) return -1;
    return 0;
}

/* --- Utility functions --- */


int clamp(int v, int lo, int hi) {
    return clamp_impl(v, lo, hi);
}

/* --- Daemon socket communication --- */

int daemon_send_command(const char *cmd, char *resp, size_t resp_size) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to create socket\n");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, DAEMON_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: Failed to connect to daemon at %s (%s)\n", DAEMON_SOCKET_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    /* Send command with newline */
    char cmd_buf[256];
    snprintf(cmd_buf, sizeof(cmd_buf), "%s\n", cmd);
    ssize_t sent = write(fd, cmd_buf, strlen(cmd_buf));
    if (sent < 0) {
        fprintf(stderr, "Error: Failed to send command\n");
        close(fd);
        return -1;
    }

    /* Receive response */
    memset(resp, 0, resp_size);
    ssize_t received = read(fd, resp, resp_size - 1);
    if (received < 0) {
        fprintf(stderr, "Error: Failed to receive response\n");
        close(fd);
        return -1;
    }
    if (received == 0) {
        fprintf(stderr, "Error: Daemon closed connection unexpectedly\n");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}
