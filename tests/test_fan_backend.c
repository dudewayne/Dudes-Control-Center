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

    /* Direct EC access only with the opt-in (v1.41.0): DCC_EC_DIRECT_FILE = "ecdirect" */
    CHECK(!ec_direct_allowed(), "no opt-in file -> direct EC access not allowed");
    CHECK(ec_init() == EC_INIT_DISABLED && !ec_backend_module() && !strcmp(ec_backend_name(), "none") &&
          strstr(ec_init_reason(), "not enabled"),
          "no module, no opt-in -> EC_INIT_DISABLED, backend none, reason says why (ports never touched)");
    CHECK(set_fan_duty(50) == -1, "no EC access -> no fan write");
    CHECK(get_cpu_fan_rpm() == 0 && get_cpu_fan_duty() == -1, "no EC access -> fan values unknown, ports not read");
    put_abs("ecdirect", "yes\n");
    CHECK(!ec_direct_allowed() && ec_init() == EC_INIT_DISABLED, "opt-in file must say \"enabled\"");
    put_abs("ecdirect", "enabled\n");
    CHECK(ec_direct_allowed(), "opt-in file \"enabled\" -> allowed");
    if (!root) {
        CHECK(ec_init() == EC_INIT_ERROR && !strcmp(ec_backend_name(), "none"),
              "opt-in but no root -> port I/O refused (ioperm), backend none");
    } else {
        printf("SKIP  port I/O refusal (running as root)\n");
    }
    unlink("ecdirect");

    /* plausibility rules before any port write (v1.41.0) */
    char why[200];
    int ok3[3] = { 52, 53, 52 }, jump[3] = { 52, 90, 30 }, low[3] = { 0, 0, 0 }, ff[3] = { 255, 255, 255 };
    CHECK(ec_values_plausible(ok3, 55, 3400, 0, why, sizeof why), "Clevo-like values (steady temperature, a fan speed, a fan off) pass");
    CHECK(!ec_values_plausible(jump, -1, 0, 0, why, sizeof why) && strstr(why, "steady"), "jumping temperature register refused");
    CHECK(!ec_values_plausible(low, -1, 0, 0, why, sizeof why), "register reads 0 -> refused");
    CHECK(!ec_values_plausible(ff, -1, 0, 0, why, sizeof why), "register reads 255 (no such register) -> refused");
    CHECK(!ec_values_plausible(ok3, 85, 0, 0, why, sizeof why) && strstr(why, "sensor"), "far from the CPU's own sensor -> refused");
    CHECK(!ec_values_plausible(ok3, -1, 65000, 0, why, sizeof why) && strstr(why, "RPM"), "fan register not a fan speed -> refused");
    CHECK(!ec_values_plausible(ok3, -1, 3000, 90, why, sizeof why), "second fan register not a fan speed -> refused");

    /* another hwmon device first, then the module */
    mkdir("hwmon", 0755);
    mkdir("hwmon/hwmon0", 0755);
    put("hwmon0", "name", "coretemp\n");
    put("hwmon0", "temp1_input", "47000\n");
    CHECK(cpu_temp_hwmon() == 47 && get_cpu_temp() == 47, "no EC access: CPU temperature from coretemp (v1.41.0)");
    put("hwmon0", "name", "k10temp\n");             /* AMD (1.42.0): Tdie, not Tctl */
    put("hwmon0", "temp1_label", "Tctl\n");
    put("hwmon0", "temp1_input", "72000\n");
    put("hwmon0", "temp2_label", "Tdie\n");
    put("hwmon0", "temp2_input", "62000\n");
    CHECK(cpu_temp_hwmon() == 62, "k10temp: the Tdie sensor is used when it is there");
    put("hwmon0", "temp2_label", "Tccd1\n");
    CHECK(cpu_temp_hwmon() == 72, "k10temp without Tdie: temp1 (Tctl)");
    put("hwmon0", "name", "zenpower\n");
    CHECK(cpu_temp_hwmon() == 72, "zenpower: temp1");
    put("hwmon0", "name", "coretemp\n");
    put("hwmon0", "temp1_input", "47000\n");
    fake_module("hwmon7");
    CHECK(ec_init() == 0 && ec_backend_module() && !strcmp(ec_backend_name(), "module"),
          "dcc_fan hwmon device found and used, no root needed");

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

    /* module removed: port I/O only with the opt-in (v1.41.0) */
    rename("hwmon/hwmon9", "hwmon/gone");
    get_cpu_fan_rpm();
    CHECK(!ec_backend_module() && !strcmp(ec_backend_name(), "none") && set_fan_duty(50) == -1,
          "module removed without opt-in: no EC access, no writes");

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
    usleep(1200000);                                 /* past the 0.9 s cache */
    CHECK(gpu_info_get(&g) == GPU_SLEEPING && access("calls", F_OK) != 0,
          "sleeping GPU: nvidia-smi is not run (it would wake the GPU)");
    put_abs("pci/0000:02:00.0/power/runtime_status", "active\n");
    usleep(1200000);
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
    CHECK(calls == 1, "values are cached: further requests within 0.9 s run nvidia-smi no more");
    put_abs("nvidia-smi", "#!/bin/sh\necho 'GPU, [N/A], [N/A], 300, 405, 0, 10, 8000, P8'\n");
    usleep(1200000);
    CHECK(gpu_info_get(&g) == GPU_AWAKE && g.temp == -1 && g.power_w < 0 && g.clock_mhz == 300,
          "[N/A] fields read as unknown");

    /* AMD GPU (1.42.0): amdgpu sysfs, nothing read while it sleeps */
    if (system("rm -rf pci/0000:02:00.0") != 0) return 1;
    char cwd[512], lnk[700];
    if (!getcwd(cwd, sizeof cwd)) return 1;
    mkdir("pci/0000:03:00.0", 0755);                 /* discrete RX 7600S */
    mkdir("pci/0000:03:00.0/power", 0755);
    put_abs("pci/0000:03:00.0/vendor", "0x1002\n");
    put_abs("pci/0000:03:00.0/class", "0x030000\n");
    put_abs("pci/0000:03:00.0/boot_vga", "0\n");
    put_abs("pci/0000:03:00.0/device", "0x7480\n");
    put_abs("pci/0000:03:00.0/revision", "0xc0\n");
    put_abs("pci/0000:03:00.0/power/runtime_status", "suspended\n");
    if (symlink("../../drivers/amdgpu", "pci/0000:03:00.0/driver") != 0) return 1;
    mkdir("pci/0000:03:00.0/hwmon", 0755);
    mkdir("pci/0000:03:00.0/hwmon/hwmon5", 0755);
    put_abs("pci/0000:03:00.0/hwmon/hwmon5/name", "amdgpu\n");
    put_abs("pci/0000:03:00.0/hwmon/hwmon5/temp1_input", "58000\n");
    put_abs("pci/0000:03:00.0/hwmon/hwmon5/power1_average", "35500000\n");
    put_abs("pci/0000:03:00.0/hwmon/hwmon5/freq1_input", "2100000000\n");
    put_abs("pci/0000:03:00.0/hwmon/hwmon5/freq2_input", "1000000000\n");
    put_abs("pci/0000:03:00.0/gpu_busy_percent", "17\n");
    put_abs("pci/0000:03:00.0/mem_info_vram_used", "1073741824\n");
    put_abs("pci/0000:03:00.0/mem_info_vram_total", "8589934592\n");
    mkdir("pci/0000:05:00.0", 0755);                 /* integrated 780M, awake */
    put_abs("pci/0000:05:00.0/vendor", "0x1002\n");
    put_abs("pci/0000:05:00.0/class", "0x038000\n");
    put_abs("pci/0000:05:00.0/boot_vga", "1\n");
    put_abs("pci/0000:05:00.0/device", "0x15bf\n");
    put_abs("pci/0000:05:00.0/revision", "0xc4\n");
    if (symlink("../../drivers/amdgpu", "pci/0000:05:00.0/driver") != 0) return 1;
    mkdir("pci/0000:05:00.0/hwmon", 0755);
    mkdir("pci/0000:05:00.0/hwmon/hwmon6", 0755);
    put_abs("pci/0000:05:00.0/hwmon/hwmon6/temp1_input", "49000\n");
    put_abs("pci/0000:05:00.0/hwmon/hwmon6/power1_input", "9000000\n");
    put_abs("pci/0000:05:00.0/hwmon/hwmon6/freq1_input", "800000000\n");
    put_abs("pci/0000:05:00.0/gpu_busy_percent", "4\n");
    put_abs("amdgpu.ids", "# test\n7480,\tC0,\tAMD Radeon RX 7600S\n15BF,\tC1,\tAMD Radeon 780M Graphics\n");
    /* the discrete GPU's hwmon as /sys/class/hwmon shows it */
    mkdir("hwmon/hwmon20", 0755);
    put("hwmon20", "name", "amdgpu\n");
    put("hwmon20", "temp1_input", "58000\n");
    snprintf(lnk, sizeof lnk, "%s/pci/0000:03:00.0", cwd);
    if (symlink(lnk, "hwmon/hwmon20/device") != 0) return 1;
    usleep(1200000);
    CHECK(gpu_info_get(&g) == GPU_SLEEPING, "AMD: discrete GPU asleep -> sleeping (not the awake integrated one)");
    CHECK(get_gpu_temp() != 58, "AMD: a sleeping GPU's hwmon is not read for the GPU temperature");
    put_abs("pci/0000:03:00.0/power/runtime_status", "active\n");
    usleep(1200000);
    CHECK(gpu_info_get(&g) == GPU_AWAKE && !strcmp(g.name, "AMD Radeon RX 7600S") && g.temp == 58 &&
          g.power_w > 35.4 && g.power_w < 35.6 && g.clock_mhz == 2100 && g.mem_clock_mhz == 1000 &&
          g.util == 17 && g.vram_used_mb == 1024 && g.vram_total_mb == 8192 && !g.pstate[0],
          "AMD discrete awake: name from amdgpu.ids, temperature, power, clocks, load, VRAM");
    CHECK(get_gpu_temp() == 58, "AMD: awake GPU's hwmon gives the GPU temperature");
    if (system("rm -rf pci/0000:03:00.0 hwmon/hwmon20") != 0) return 1;
    usleep(1200000);
    CHECK(gpu_info_get(&g) == GPU_AWAKE && !strcmp(g.name, "AMD Radeon 780M Graphics") && g.temp == 49 &&
          g.power_w > 8.9 && g.power_w < 9.1 && g.clock_mhz == 800 && g.mem_clock_mhz == -1 && g.util == 4 &&
          g.vram_total_mb == -1,
          "AMD integrated: power1_input, no memory clock, name of another revision");
    unlink("pci/0000:05:00.0/driver");
    usleep(1200000);
    CHECK(gpu_info_get(&g) == GPU_AWAKE && !strcmp(g.name, "AMD Radeon GPU") && g.temp == -1 && g.util == -1,
          "AMD without the amdgpu driver: no readings");

    ec_cleanup();
    printf("\n%d failure(s)\n", fails);
    return fails != 0;
}
