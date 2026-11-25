// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_fan_backend.c - EC access through the dcc_fan kernel module's hwmon
 * device (v1.36.0): detection, reading, writing, keepalive, the module
 * being reloaded or removed; NVIDIA GPU values without waking a sleeping GPU
 * (v1.37.0). Built with DCC_HWMON_DIR="hwmon", DCC_PCI_DIR="pci" and
 * DCC_NVIDIA_SMI="./nvidia-smi"; runs in an empty directory with fake trees.
 */
#include "dcc-common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("PASS  %s\n", m); else { printf("FAIL  %s\n", m); fails++; } } while (0)

static void put(const char *dir, const char *file, const char *text) {
    char p[256];
    snprintf(p, sizeof p, "hwmon/%s/%s", dir, file);
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static void put_abs(const char *p, const char *text) {
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static long get(const char *dir, const char *file) {
    char p[256], buf[32] = "";
    snprintf(p, sizeof p, "hwmon/%s/%s", dir, file);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    if (!fgets(buf, sizeof buf, f)) buf[0] = '\0';
    fclose(f);
    return strtol(buf, NULL, 10);
}

static void fake_module(const char *dir) {
    char p[64];
    mkdir("hwmon", 0755);
    snprintf(p, sizeof p, "hwmon/%s", dir);
    mkdir(p, 0755);
    put(dir, "name", "dcc_fan\n");
    put(dir, "temp1_input", "55000\n");
    put(dir, "temp2_input", "47000\n");
    put(dir, "fan1_input", "3428\n");
    put(dir, "fan2_input", "3291\n");
    put(dir, "pwm1", "135\n");
    put(dir, "pwm2", "135\n");
}

int main(void) {
    int root = geteuid() == 0;

    /* no module: port I/O (needs root, so it fails here) */
    if (!root) {
        CHECK(ec_init() != 0 && !ec_backend_module(), "without the module and root: no EC access (port I/O refused)");
    } else {
        printf("SKIP  port I/O refusal (running as root)\n");
    }

    /* another hwmon device first, then the module */
    mkdir("hwmon", 0755);
    mkdir("hwmon/hwmon0", 0755);
    put("hwmon0", "name", "coretemp\n");
    fake_module("hwmon7");
    CHECK(ec_init() == 0 && ec_backend_module(), "dcc_fan hwmon device found and used, no root needed");

    CHECK(get_cpu_temp() == 55, "CPU temperature from temp1_input");
    CHECK(get_cpu_fan_rpm() == 3428 && get_gpu_fan_rpm() == 3291, "fan RPM from fan1_input/fan2_input");
    CHECK(get_cpu_fan_duty() == 53 && get_gpu_fan_duty() == 53, "duty 135/255 from pwm1/pwm2 -> 53%");
    put("hwmon7", "temp1_input", "5000\n");
    CHECK(get_cpu_temp() == -1, "implausible CPU temperature (5 C) rejected");
    put("hwmon7", "temp1_input", "55000\n");

    CHECK(set_fan_duty(50) == 0 && get("hwmon7", "pwm1") == 128 && get("hwmon7", "pwm2") == 128,
          "set_fan_duty(50) writes 128 to pwm1 and pwm2");
    put("hwmon7", "pwm1", "0\n");
    CHECK(fan_keepalive() == 0 && get("hwmon7", "pwm1") == 128, "fan_keepalive rewrites the last duty (module watchdog)");

    /* module reloaded under another hwmon number (package update) */
    rename("hwmon/hwmon7", "hwmon/hwmon9");
    get_cpu_fan_rpm();                      /* notices the old path is gone */
    CHECK(ec_backend_module() && get_cpu_fan_rpm() == 3428, "reloaded module found again (new hwmon number)");

    /* module removed: back to port I/O */
    rename("hwmon/hwmon9", "hwmon/gone");
    get_cpu_fan_rpm();
    CHECK(!ec_backend_module(), "module removed: the backend switches back to port I/O");

    /* NVIDIA GPU (v1.37.0): nvidia-smi only while the GPU is awake */
    GpuInfo g;
    CHECK(gpu_info_get(&g) == GPU_NONE, "no NVIDIA GPU -> state none");
    mkdir("pci", 0755);
    mkdir("pci/0000:00:02.0", 0755);                 /* Intel iGPU: ignored */
    put_abs("pci/0000:00:02.0/vendor", "0x8086\n");
    put_abs("pci/0000:00:02.0/class", "0x030000\n");
    mkdir("pci/0000:02:00.0", 0755);
    mkdir("pci/0000:02:00.0/power", 0755);
    put_abs("pci/0000:02:00.0/vendor", "0x10de\n");
    put_abs("pci/0000:02:00.0/class", "0x030000\n");
    put_abs("pci/0000:02:00.0/power/runtime_status", "suspended\n");
    put_abs("nvidia-smi", "#!/bin/sh\necho x >> calls\n"
            "echo 'NVIDIA GeForce RTX 5090 Laptop GPU, 43, 26.26, 502, 14001, 2, 2487, 24463, P0'\n");
    chmod("nvidia-smi", 0755);
    sleep(4);                                        /* past the 3 s cache */
    CHECK(gpu_info_get(&g) == GPU_SLEEPING && access("calls", F_OK) != 0,
          "sleeping GPU: nvidia-smi is not run (it would wake the GPU)");
    put_abs("pci/0000:02:00.0/power/runtime_status", "active\n");
    sleep(4);
    CHECK(gpu_info_get(&g) == GPU_AWAKE && g.temp == 43 && g.power_w > 26.2 && g.power_w < 26.3 &&
          g.clock_mhz == 502 && g.mem_clock_mhz == 14001 && g.util == 2 && g.vram_used_mb == 2487 &&
          g.vram_total_mb == 24463 && !strcmp(g.pstate, "P0") && !strcmp(g.name, "NVIDIA GeForce RTX 5090 Laptop GPU"),
          "awake GPU: one nvidia-smi call gives temperature, power, clocks, load, VRAM, P-state");
    gpu_info_get(&g);
    get_gpu_temp();
    int calls = 0;
    FILE *cf = fopen("calls", "r");
    for (int ch; cf && (ch = fgetc(cf)) != EOF;) calls += ch == '\n';
    if (cf) fclose(cf);
    CHECK(calls == 1, "values are cached: further requests within 3 s run nvidia-smi no more");
    put_abs("nvidia-smi", "#!/bin/sh\necho 'GPU, [N/A], [N/A], 300, 405, 0, 10, 8000, P8'\n");
    sleep(4);
    CHECK(gpu_info_get(&g) == GPU_AWAKE && g.temp == -1 && g.power_w < 0 && g.clock_mhz == 300,
          "[N/A] fields read as unknown");

    ec_cleanup();
    printf("\n%d failure(s)\n", fails);
    return fails != 0;
}
