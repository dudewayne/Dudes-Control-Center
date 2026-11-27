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
#include "dcc-hwmon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
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
static int g_ports_ok;          /* ioperm done and the EC looked plausible */
static char g_reason[200];      /* why there is no direct access (1.41.0) */
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

static int ports_plausible(char *why, size_t n);

/* Opt-in for direct port I/O (1.41.0) */
int ec_direct_allowed(void) {
    char buf[32] = "";
    FILE *f = fopen(DCC_EC_DIRECT_FILE, "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof buf, f)) buf[0] = '\0';
    fclose(f);
    buf[strcspn(buf, " \t\r\n")] = '\0';
    return strcmp(buf, "enabled") == 0;
}

/* Port I/O only with the opt-in and an EC that answers like a Clevo EC */
static int ports_start(void) {
    if (g_ports_ok) return EC_INIT_OK;
    if (!ec_direct_allowed()) {
        snprintf(g_reason, sizeof g_reason, "no dcc_fan module (no Clevo WMI interface) and direct EC access is not enabled");
        return EC_INIT_DISABLED;
    }
    if (ports_init() != 0) {
        snprintf(g_reason, sizeof g_reason, "no I/O permission for the EC ports (not root, or kernel lockdown)");
        return EC_INIT_ERROR;
    }
    char why[160] = "";
    if (!ports_plausible(why, sizeof why)) {
        ec_cleanup();
        snprintf(g_reason, sizeof g_reason, "direct EC access refused: %s", why);
        return EC_INIT_IMPLAUSIBLE;
    }
    g_reason[0] = '\0';
    return EC_INIT_OK;
}

/* The module went away (rmmod, package update): port I/O if allowed */
static void kmod_lost(void) {
    if (!g_kmod[0]) return;
    g_kmod[0] = '\0';
    if (kmod_find()) return;                  /* reloaded (package update): new hwmonN */
    if (ports_start() == EC_INIT_OK)
        fprintf(stderr, "dcc_fan module no longer available, using direct port I/O (enabled by the administrator)\n");
    else
        fprintf(stderr, "dcc_fan module no longer available, no fan control: %s\n", g_reason);
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
const char *ec_backend_name(void) { return g_kmod[0] ? "module" : g_ports_ok ? "ports" : "none"; }
const char *ec_init_reason(void) { return g_reason; }

/* CPU package temperature from the standard drivers (coretemp, k10temp,
 * zenpower): used when there is no EC access (1.41.0); see dcc-hwmon.h */
int cpu_temp_hwmon(void) {
    return dcc_cpu_temp_hwmon(DCC_HWMON_DIR, TEMP_MIN_VALID_C, TEMP_MAX_VALID_C);
}

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
    if (kmod_find()) { g_reason[0] = '\0'; return EC_INIT_OK; }
    return ports_start();
}

void ec_cleanup(void) {
    if (g_ports_ok) {
        ioperm(EC_DATA, 1, 0);
        ioperm(EC_SC, 1, 0);
        g_ports_ok = 0;
    }
}

int ec_io_read_checked(const uint32_t port, uint8_t *value) {
    if (!g_ports_ok) return -1;   /* no direct access (1.41.0): never touch the ports */
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
    if (!g_ports_ok) return -1;   /* no direct access (1.41.0) */
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
    if (!ec_backend_module() && !g_ports_ok) return cpu_temp_hwmon();   /* no EC access (1.41.0) */
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
    DIR *d = opendir(DCC_HWMON_DIR);
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

        /* A runtime-suspended GPU (discrete AMD) is not read: older
         * amdgpu kernels wake it for a hwmon read (1.42.0) */
        int rfd = openat(hwfd, "device/power/runtime_status", O_RDONLY);
        if (rfd >= 0) {
            char rs[16] = {0};
            ssize_t rn = read(rfd, rs, sizeof(rs) - 1);
            close(rfd);
            if (rn > 0 && strncmp(rs, "suspend", 7) == 0) { close(hwfd); continue; }
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

/* --- GPU: NVIDIA (1.37.0), AMD (1.42.0) ---
 * nvidia-smi wakes a GPU that the driver has powered down (runtime D3,
 * "on-demand" mode on hybrid laptops). It used to run for the GPU
 * temperature on every control step and STATUS request, so the GPU could
 * never sleep. Now the PCI runtime state is checked first; nvidia-smi runs
 * only while the GPU is awake anyway, at most every GPU_CACHE_SEC, and gives
 * temperature, power, clocks, load and VRAM in one call. */
#ifndef DCC_PCI_DIR
#define DCC_PCI_DIR "/sys/bus/pci/devices"
#endif
#ifndef DCC_NVIDIA_SMI
#define DCC_NVIDIA_SMI "/usr/bin/nvidia-smi"
#endif
#define GPU_CACHE_SEC 0.9   /* below the shortest GUI refresh interval (1 s, 1.40.0) */

#ifndef DCC_AMDGPU_IDS
#define DCC_AMDGPU_IDS "/usr/share/libdrm/amdgpu.ids"
#endif

enum { GPU_VENDOR_NONE = 0, GPU_VENDOR_NVIDIA, GPU_VENDOR_AMD };

static int read_line_at(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = '\0'; return -1; }
    int ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    if (!ok) { buf[0] = '\0'; return -1; }
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static long read_long_at(const char *path) {
    char buf[32];
    if (read_line_at(path, buf, sizeof buf) != 0) return -1;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    return end == buf ? -1 : v;
}

/* The GPU the Status tab shows (1.42.0): an NVIDIA GPU first, then a
 * discrete AMD GPU, then an integrated AMD GPU (boot_vga 1). Intel GPUs have
 * no readings here. Returns GPU_NONE, GPU_SLEEPING (runtime-suspended:
 * nothing is read, reading would wake it) or GPU_AWAKE; dev gets the PCI
 * device directory. */
static int gpu_find(char *dev, size_t n, int *vendor) {
    DIR *d = opendir(DCC_PCI_DIR);
    *vendor = GPU_VENDOR_NONE;
    dev[0] = '\0';
    if (!d) return GPU_NONE;
    int best = 0, state = GPU_NONE;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char p[512], buf[32];
        snprintf(p, sizeof p, "%s/%s/class", DCC_PCI_DIR, de->d_name);
        if (read_line_at(p, buf, sizeof buf) != 0 || strncmp(buf, "0x03", 4) != 0) continue;  /* display controller */
        snprintf(p, sizeof p, "%s/%s/vendor", DCC_PCI_DIR, de->d_name);
        if (read_line_at(p, buf, sizeof buf) != 0) continue;
        int v, prio;
        if (!strncmp(buf, "0x10de", 6)) { v = GPU_VENDOR_NVIDIA; prio = 6; }
        else if (!strncmp(buf, "0x1002", 6)) {
            v = GPU_VENDOR_AMD;
            snprintf(p, sizeof p, "%s/%s/boot_vga", DCC_PCI_DIR, de->d_name);
            prio = read_long_at(p) == 1 ? 2 : 4;          /* integrated : discrete */
        } else continue;
        /* "suspended"/"suspending": asleep; "active", "resuming" or no
         * runtime PM: awake */
        snprintf(p, sizeof p, "%s/%s/power/runtime_status", DCC_PCI_DIR, de->d_name);
        read_line_at(p, buf, sizeof buf);
        int st = strncmp(buf, "suspend", 7) == 0 ? GPU_SLEEPING : GPU_AWAKE;
        prio += st == GPU_AWAKE;                          /* same kind: the awake one */
        if (prio <= best) continue;
        best = prio;
        state = st;
        *vendor = v;
        snprintf(dev, n, "%s/%s", DCC_PCI_DIR, de->d_name);
    }
    closedir(d);
    return state;
}

/* Marketing name of an AMD GPU from libdrm's amdgpu.ids
 * ("15BF,\tC1,\tAMD Radeon 780M Graphics") */
static void amdgpu_name(const char *dev, char *name, size_t n) {
    char p[600], dbuf[16], rbuf[16];
    snprintf(name, n, "AMD Radeon GPU");
    snprintf(p, sizeof p, "%s/device", dev);
    if (read_line_at(p, dbuf, sizeof dbuf) != 0) return;
    snprintf(p, sizeof p, "%s/revision", dev);
    if (read_line_at(p, rbuf, sizeof rbuf) != 0) rbuf[0] = '\0';
    unsigned did = (unsigned)strtoul(dbuf, NULL, 16), rev = (unsigned)strtoul(rbuf, NULL, 16);
    FILE *f = fopen(DCC_AMDGPU_IDS, "r");
    if (!f) return;
    char line[256], any[64] = "";
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *save = NULL, *a = strtok_r(line, ",", &save), *b = strtok_r(NULL, ",", &save),
             *c = strtok_r(NULL, "\n", &save);
        if (!a || !b || !c) continue;
        if ((unsigned)strtoul(a, NULL, 16) != did) continue;
        while (*c == ' ' || *c == '\t') c++;
        if ((unsigned)strtoul(b, NULL, 16) == rev) { snprintf(name, n, "%s", c); fclose(f); return; }
        if (!any[0]) snprintf(any, sizeof any, "%s", c);    /* same chip, other revision */
    }
    fclose(f);
    if (any[0]) snprintf(name, n, "%s", any);
}

/* amdgpu values from sysfs, read only while the GPU is awake: hwmon
 * (temperature, power, clocks), gpu_busy_percent, mem_info_vram_* */
static void amdgpu_read(const char *dev, GpuInfo *g) {
    char p[1000], hd[600], hw[900] = "";
    amdgpu_name(dev, g->name, sizeof g->name);
    snprintf(hd, sizeof hd, "%s/hwmon", dev);
    DIR *d = opendir(hd);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL)
            if (!strncmp(de->d_name, "hwmon", 5)) { snprintf(hw, sizeof hw, "%s/%s", hd, de->d_name); break; }
        closedir(d);
    }
    if (hw[0]) {
        long v;
        snprintf(p, sizeof p, "%s/temp1_input", hw);           /* "edge" */
        if ((v = read_long_at(p)) > 0 && v / 1000 <= 130) g->temp = (int)(v / 1000);
        snprintf(p, sizeof p, "%s/power1_average", hw);        /* microwatts */
        if ((v = read_long_at(p)) < 0) { snprintf(p, sizeof p, "%s/power1_input", hw); v = read_long_at(p); }
        if (v >= 0) g->power_w = v / 1e6;
        snprintf(p, sizeof p, "%s/freq1_input", hw);           /* sclk, Hz */
        if ((v = read_long_at(p)) > 0) g->clock_mhz = (int)(v / 1000000);
        snprintf(p, sizeof p, "%s/freq2_input", hw);           /* mclk, Hz (discrete only) */
        if ((v = read_long_at(p)) > 0) g->mem_clock_mhz = (int)(v / 1000000);
    }
    long v;
    snprintf(p, sizeof p, "%s/gpu_busy_percent", dev);
    if ((v = read_long_at(p)) >= 0 && v <= 100) g->util = (int)v;
    snprintf(p, sizeof p, "%s/mem_info_vram_used", dev);
    if ((v = read_long_at(p)) >= 0) g->vram_used_mb = (int)(v / (1024 * 1024));
    snprintf(p, sizeof p, "%s/mem_info_vram_total", dev);
    if ((v = read_long_at(p)) > 0) g->vram_total_mb = (int)(v / (1024 * 1024));
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* "12.5" -> 12.5; "[N/A]", "N/A", "" -> -1 */
static double csv_num(const char *f) {
    while (*f == ' ') f++;
    char *end = NULL;
    double v = strtod(f, &end);
    return end == f ? -1.0 : v;
}

int gpu_info_get(GpuInfo *out) {
    static GpuInfo cache;
    static double cache_time = -1e9;
    double now = now_sec();
    if (now - cache_time < GPU_CACHE_SEC) { *out = cache; return cache.state; }

    GpuInfo g;
    memset(&g, 0, sizeof g);
    g.temp = g.clock_mhz = g.mem_clock_mhz = g.util = g.vram_used_mb = g.vram_total_mb = -1;
    g.power_w = -1;
    char dev[512];
    int vendor;
    g.state = gpu_find(dev, sizeof dev, &vendor);
    if (g.state == GPU_AWAKE && vendor == GPU_VENDOR_AMD) {
        char p[600], drv[600] = "";
        snprintf(p, sizeof p, "%s/driver", dev);
        ssize_t l = readlink(p, drv, sizeof drv - 1);
        if (l > 0) drv[l] = '\0';
        const char *base = strrchr(drv, '/');
        if (base && !strcmp(base + 1, "amdgpu")) amdgpu_read(dev, &g);
        else snprintf(g.name, sizeof g.name, "AMD Radeon GPU");   /* no amdgpu driver: no readings */
    } else if (g.state == GPU_AWAKE && path_exists(DCC_NVIDIA_SMI)) {
        FILE *p = popen(DCC_NVIDIA_SMI " --query-gpu=name,temperature.gpu,power.draw,clocks.gr,clocks.mem,"
                        "utilization.gpu,memory.used,memory.total,pstate --format=csv,noheader,nounits 2>/dev/null", "r");
        char line[512] = "";
        if (p) {
            if (!fgets(line, sizeof line, p)) line[0] = '\0';
            pclose(p);
        }
        line[strcspn(line, "\n")] = '\0';
        char *field[9] = { 0 };
        int n = 0;
        for (char *save = NULL, *t = strtok_r(line, ",", &save); t && n < 9; t = strtok_r(NULL, ",", &save))
            field[n++] = t;
        if (n == 9) {
            while (*field[0] == ' ') field[0]++;
            snprintf(g.name, sizeof g.name, "%s", field[0]);
            g.temp = (int)csv_num(field[1]);
            g.power_w = csv_num(field[2]);
            g.clock_mhz = (int)csv_num(field[3]);
            g.mem_clock_mhz = (int)csv_num(field[4]);
            g.util = (int)csv_num(field[5]);
            g.vram_used_mb = (int)csv_num(field[6]);
            g.vram_total_mb = (int)csv_num(field[7]);
            char *ps = field[8];
            while (*ps == ' ') ps++;
            snprintf(g.pstate, sizeof g.pstate, "%s", ps);
            if (g.temp < 1 || g.temp > 130) g.temp = -1;
        }
    }
    cache = g;
    cache_time = now;
    *out = g;
    return g.state;
}

int get_gpu_temp(void) {
    /* Prefer driver temps for GPU */
    int temp = gpu_temp_sysfs();
    if (temp > 0 && temp <= TEMP_MAX_VALID_C) {
        return temp;
    }

    /* NVIDIA: only while the GPU is awake (asleep: the EC's sensor below) */
    GpuInfo gi;
    if (gpu_info_get(&gi) == GPU_AWAKE && gi.temp > 0 && gi.temp <= TEMP_MAX_VALID_C)
        return gi.temp;

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
    if (!g_ports_ok) return -1;   /* no module, no (allowed and plausible) direct access */

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

/* Before any write through the ports (1.41.0): the EC must answer every
 * handshake and its Clevo registers must hold sensible values - a CPU
 * temperature that is steady over three reads (and close to the CPU's own
 * sensor, if there is one) and fan speeds that are 0 or in a fan's range.
 * Only reads; on another EC layout these registers hold other data and the
 * check fails. */
/* The rules (testable without an EC): t[3] = three reads of the CPU
 * temperature register, ref = the CPU's own sensor (-1 none), r1/r2 = fan
 * speeds from the RPM registers */
int ec_values_plausible(const int t[3], int ref, int r1, int r2, char *why, size_t n) {
    int lo = t[0], hi = t[0];
    for (int i = 1; i < 3; i++) { if (t[i] < lo) lo = t[i]; if (t[i] > hi) hi = t[i]; }
    if (lo < TEMP_MIN_VALID_C || hi > TEMP_MAX_VALID_C || hi - lo > 10) {
        snprintf(why, n, "the CPU temperature register reads %d, %d, %d (not a steady temperature)", t[0], t[1], t[2]);
        return 0;
    }
    if (ref > 0 && abs(ref - t[2]) > 20) {
        snprintf(why, n, "the CPU temperature register reads %d degC, the CPU's sensor %d degC", t[2], ref);
        return 0;
    }
    if ((r1 != 0 && (r1 < 200 || r1 > 10000)) || (r2 != 0 && (r2 < 200 || r2 > 10000))) {
        snprintf(why, n, "the fan speed registers read %d and %d RPM (not a fan speed)", r1, r2);
        return 0;
    }
    return 1;
}

static int ports_plausible(char *why, size_t n) {
    int t[3];
    g_ports_ok = 1;   /* the read helpers need it */
    for (int i = 0; i < 3; i++) {
        uint8_t v;
        if (ec_io_read_checked(EC_REG_CPU_TEMP, &v) != 0) {
            g_ports_ok = 0;
            snprintf(why, n, "the EC at ports 0x62/0x66 does not answer");
            return 0;
        }
        t[i] = v;
        if (i < 2) usleep(150 * 1000);
    }
    int ok = ec_values_plausible(t, cpu_temp_hwmon(), get_cpu_fan_rpm(), get_gpu_fan_rpm(), why, n);
    if (!ok) g_ports_ok = 0;
    return ok;
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
