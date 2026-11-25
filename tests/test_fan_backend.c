// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_fan_backend.c - EC access through the dcc_fan kernel module's hwmon
 * device (v1.36.0): detection, reading, writing, keepalive, the module
 * being reloaded or removed. Built with DCC_HWMON_DIR="hwmon", runs in an
 * empty directory with a fake hwmon tree; needs no root and no module.
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

    ec_cleanup();
    printf("\n%d failure(s)\n", fails);
    return fails != 0;
}
