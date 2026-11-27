// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc-hwmon.h - CPU package temperature from the standard hwmon drivers,
 * shared by the fan daemon/CLI (dcc-common.c) and the CPU daemon
 * (dcc-cpu-common.c), 1.42.0.
 *
 * Intel: coretemp temp1 ("Package id 0"). AMD: k10temp "Tdie" if it is
 * there (Tctl can carry an offset on some models), otherwise temp1 ("Tctl");
 * zenpower temp1 ("Tdie").
 */
#ifndef DCC_HWMON_H
#define DCC_HWMON_H

#include <dirent.h>
#include <stdio.h>
#include <string.h>

/* degC, or -1 if no such driver / no value in min..max */
static inline int dcc_cpu_temp_hwmon(const char *hwmon_dir, int min, int max) {
    DIR *d = opendir(hwmon_dir);
    if (!d) return -1;
    struct dirent *de;
    int t = -1;
    while (t < 0 && (de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "hwmon", 5) != 0) continue;
        char path[600], name[32] = "";
        snprintf(path, sizeof path, "%s/%s/name", hwmon_dir, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = '\0';
        fclose(f);
        if (strcmp(name, "coretemp") && strcmp(name, "k10temp") && strcmp(name, "zenpower")) continue;
        int idx = 1;
        if (!strcmp(name, "k10temp")) {
            for (int i = 1; i <= 10; i++) {
                char label[32] = "";
                snprintf(path, sizeof path, "%s/%s/temp%d_label", hwmon_dir, de->d_name, i);
                if (!(f = fopen(path, "r"))) continue;
                if (fgets(label, sizeof label, f)) label[strcspn(label, "\n")] = '\0';
                fclose(f);
                if (!strcmp(label, "Tdie")) { idx = i; break; }
            }
        }
        snprintf(path, sizeof path, "%s/%s/temp%d_input", hwmon_dir, de->d_name, idx);
        if (!(f = fopen(path, "r"))) continue;
        long mc;
        if (fscanf(f, "%ld", &mc) == 1 && mc / 1000 >= min && mc / 1000 <= max) t = (int)(mc / 1000);
        fclose(f);
    }
    closedir(d);
    return t;
}

#endif
