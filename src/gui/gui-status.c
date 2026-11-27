// SPDX-License-Identifier: GPL-2.0-only
/*
 * gui-status.c
 * Status tab: temperatures, fans, GPU, power, battery, active profiles, system information.
 * Part of dcc-gui.c, which #includes it: one translation unit, so the
 * static functions and AppData stay shared (split up in 1.48.1).
 */


/* Parse STATUS response and update UI */
/* GPU_* lines of the fan daemon's STATUS (1.37.0) */
typedef struct {
    char state[16], name[64];
    double power;
    int clock, mem_clock, util, vram_used, vram_total;
    char pstate[8];
} StatusGpu;

static void status_gpu_field(StatusGpu *g, const char *kv) {
    const char *v = strchr(kv, '=');
    if (!v) return;
    v++;
    if (!strncmp(kv, "STATE=", 6)) snprintf(g->state, sizeof g->state, "%s", v);
    else if (!strncmp(kv, "NAME=", 5)) snprintf(g->name, sizeof g->name, "%s", v);
    else if (!strncmp(kv, "POWER_W=", 8)) g->power = num_c(v);
    else if (!strncmp(kv, "CLOCK_MHZ=", 10)) g->clock = atoi(v);
    else if (!strncmp(kv, "MEM_CLOCK_MHZ=", 14)) g->mem_clock = atoi(v);
    else if (!strncmp(kv, "UTIL=", 5)) g->util = atoi(v);
    else if (!strncmp(kv, "VRAM_USED_MB=", 13)) g->vram_used = atoi(v);
    else if (!strncmp(kv, "VRAM_TOTAL_MB=", 14)) g->vram_total = atoi(v);
    else if (!strncmp(kv, "PSTATE=", 7)) snprintf(g->pstate, sizeof g->pstate, "%s", v);
}

static void status_show_gpu(AppData *app, const StatusGpu *g) {
    if (!app->gpu_name_label) return;
    char a[128], b[128], c[128], d[128], pw[64];
    a[0] = b[0] = c[0] = d[0] = '\0';
    if (!strcmp(g->state, "awake")) {
        a[0] = '\0';   /* the name is in the System section (1.39.0) */
        if (g->clock >= 0 && g->mem_clock >= 0)
            snprintf(b, sizeof b, _("Clock: %d MHz, memory %d MHz"), g->clock, g->mem_clock);
        else if (g->clock >= 0)                   /* integrated AMD GPU: no memory clock (1.42.0) */
            snprintf(b, sizeof b, _("Clock: %d MHz"), g->clock);
        if (g->util >= 0)
            snprintf(c, sizeof c, g->pstate[0] ? _("Load: %d %% (power state %s)") : _("Load: %d %%"), g->util, g->pstate);
        if (g->vram_used >= 0 && g->vram_total > 0)
            snprintf(d, sizeof d, _("Video memory: %.1f / %.1f GB"), g->vram_used / 1024.0, g->vram_total / 1024.0);
        if (g->power >= 0) snprintf(pw, sizeof pw, _("GPU: %.1f W"), g->power);
        else snprintf(pw, sizeof pw, "%s", _("GPU: -- W"));
    } else if (!strcmp(g->state, "sleeping")) {
        snprintf(a, sizeof a, "%s", _("Sleeping (powered down)"));
        snprintf(b, sizeof b, "%s", _("Not woken up for readings"));
        snprintf(pw, sizeof pw, "%s", _("GPU: sleeping"));
    } else {
        snprintf(a, sizeof a, "%s", g->state[0] ? _("No NVIDIA, AMD or Intel GPU") : "--");
        snprintf(pw, sizeof pw, "%s", _("GPU: -- W"));
    }
    gtk_label_set_text(GTK_LABEL(app->gpu_name_label), a);
    gtk_widget_set_visible(app->gpu_name_label, a[0] != '\0');
    gtk_label_set_text(GTK_LABEL(app->gpu_clock_label), b);
    gtk_label_set_text(GTK_LABEL(app->gpu_load_label), c);
    gtk_label_set_text(GTK_LABEL(app->gpu_vram_label), d);
    gtk_label_set_text(GTK_LABEL(app->pw_gpu_label), pw);
}

static void parse_status(AppData *app, const char *response) {
    char buffer[BUFFER_SIZE];
    strncpy(buffer, response, BUFFER_SIZE - 1);
    buffer[BUFFER_SIZE - 1] = '\0';

    char ts[32];
    get_timestamp(ts, sizeof(ts));

    /* Parse response and update labels */
    int cpu_temp = 0, gpu_temp = 0, cpu_fan_rpm = 0, gpu_fan_rpm = 0;
    int auto_mode = 1, fan_duty = 0, emergency_auto = -1;
    int cpu_fan_duty = -1, gpu_fan_duty = -1;   /* EC duty per fan (daemons before 1.19.2: missing) */
    StatusGpu gpu = { "", "", -1, -1, -1, -1, -1, -1, "" };   /* GPU_* (1.37.0) */

    char *token = strtok(buffer, "\n");
    while (token != NULL) {
        if (strncmp(token, "CPU_TEMP=", 9) == 0) {
            cpu_temp = atoi(token + 9);
        } else if (strncmp(token, "GPU_TEMP=", 9) == 0) {
            gpu_temp = atoi(token + 9);
        } else if (strncmp(token, "CPU_FAN_RPM=", 12) == 0) {
            cpu_fan_rpm = atoi(token + 12);
        } else if (strncmp(token, "GPU_FAN_RPM=", 12) == 0) {
            gpu_fan_rpm = atoi(token + 12);
        } else if (strncmp(token, "AUTO_MODE=", 10) == 0) {
            auto_mode = atoi(token + 10);
        } else if (strncmp(token, "FAN_DUTY=", 9) == 0) {
            fan_duty = atoi(token + 9);
            app->last_known_fan_duty = fan_duty; /* Cache for manual mode */
        } else if (strncmp(token, "CPU_FAN_DUTY=", 13) == 0) {
            cpu_fan_duty = atoi(token + 13);
        } else if (strncmp(token, "GPU_FAN_DUTY=", 13) == 0) {
            gpu_fan_duty = atoi(token + 13);
        } else if (strncmp(token, "EMERGENCY_AUTO=", 15) == 0) {
            emergency_auto = atoi(token + 15);
        } else if (strncmp(token, "GPU_", 4) == 0) {
            status_gpu_field(&gpu, token + 4);
        } else if (strncmp(token, "EC_ACCESS=", 10) == 0) {
            g_strlcpy(app->ec_access, token + 10, sizeof app->ec_access);
            app->ec_reason[0] = '\0';
        } else if (strncmp(token, "EC_REASON=", 10) == 0) {
            g_strlcpy(app->ec_reason, token + 10, sizeof app->ec_reason);
        } else if (strncmp(token, "HWMON_FANS_DRIVER=", 18) == 0) {
            g_strlcpy(app->hw_driver, token + 18, sizeof app->hw_driver);
        }
        token = strtok(NULL, "\n");
    }

    /* Update labels */
    char cpu_text[32], gpu_text[32], cpu_rpm_text[64], gpu_rpm_text[64];
    snprintf(cpu_text, sizeof(cpu_text), _("CPU: %d°C"), cpu_temp);
    snprintf(gpu_text, sizeof(gpu_text), _("GPU: %d°C"), gpu_temp);
    /* Percent = duty set in the EC for each fan; RPM = measured speed */
    format_fan_text(cpu_rpm_text, sizeof(cpu_rpm_text), _("CPU Fan"), cpu_fan_rpm, cpu_fan_duty);
    format_fan_text(gpu_rpm_text, sizeof(gpu_rpm_text), _("GPU Fan"), gpu_fan_rpm, gpu_fan_duty);

    gtk_label_set_text(GTK_LABEL(app->cpu_temp_label), cpu_text);
    gtk_label_set_text(GTK_LABEL(app->gpu_temp_label), gpu_text);
    if (strcmp(app->ec_access, "none")) {           /* without EC access: "--" (status_show_ec) */
        label_set_changed(app->cpu_fan_rpm_label, cpu_rpm_text);
        label_set_changed(app->gpu_fan_rpm_label, gpu_rpm_text);
    }
    status_show_gpu(app, &gpu);
    status_show_ec(app);

    /* Sync manual mode switch state with daemon */
    if (app->manual_mode_switch != NULL && gtk_widget_get_realized(app->manual_mode_switch)) {
        gboolean current_switch_state = gtk_switch_get_active(GTK_SWITCH(app->manual_mode_switch));
        gboolean should_be_manual = (auto_mode == 0);

        g_print("%s [GUI] parse_status: AUTO_MODE=%d, current_switch_state=%d, should_be_manual=%d, app->in_manual_mode=%d, app->auto_mode_enabled=%d\n",
                ts, auto_mode, current_switch_state, should_be_manual, app->in_manual_mode, app->auto_mode_enabled);

        if (current_switch_state != should_be_manual) {
            g_print("%s [GUI] parse_status: SYNCING switch from %d to %d\n", ts, current_switch_state, should_be_manual);
            g_signal_handler_block(app->manual_mode_switch, app->manual_mode_signal_id);
            gtk_switch_set_state(GTK_SWITCH(app->manual_mode_switch), should_be_manual);
            g_signal_handler_unblock(app->manual_mode_switch, app->manual_mode_signal_id);
        }
    }

    /* Update +/- buttons and label based on mode */
    if (app->auto_mode_enabled == 0) { /* Manual mode */
        /* In manual mode, do NOT update from daemon - preserve user's manual setting */
        /* The local app->manual_current_speed is already correct from button clicks */
    } else { /* Auto mode */
        /* In auto mode, show "--" and buttons are disabled (handled in switch callback) */
    }

    /* The daemon ended manual mode after an emergency: follow it and say why */
    int ended_by_emergency = emergency_auto >= 0 && app->emergency_auto_seen >= 0 &&
                             emergency_auto > app->emergency_auto_seen;
    if (emergency_auto >= 0) app->emergency_auto_seen = emergency_auto;
    if (auto_mode && !app->auto_mode_enabled) {
        app->auto_mode_enabled = 1;
        if (app->manual_speed_label) gtk_label_set_text(GTK_LABEL(app->manual_speed_label), "--");
        if (ended_by_emergency) {
            gui_event("[GUI] Fan daemon switched to auto mode after an emergency temperature\n");
            /* xgettext:no-c-format */
            show_status(app, _("Manual fan mode ended: the CPU/GPU reached 95\302\260C - fans at 100% until it cools down, then the fan curve (auto mode)"));
        }
    }

    /* Update internal manual mode tracking */
    int was_in_manual_mode = app->in_manual_mode;
    app->in_manual_mode = (auto_mode == 0);

    /* If manual mode state changed, update the profile tab UI */
    if (was_in_manual_mode != app->in_manual_mode) {
        gui_event("[GUI] Manual mode state changed: %s -> %s\n",
                was_in_manual_mode ? "ENABLED" : "DISABLED",
                app->in_manual_mode ? "ENABLED" : "DISABLED");
        update_profile_tab_for_manual_mode(app, app->in_manual_mode);
    }

    /* Update button sensitivity based on mode */
    if (app->manual_minus_btn != NULL && app->manual_plus_btn != NULL) {
        gboolean should_be_manual = (auto_mode == 0);
        gtk_widget_set_sensitive(app->manual_minus_btn, should_be_manual);
        gtk_widget_set_sensitive(app->manual_plus_btn, should_be_manual);
    }
}

/* Create Page 1: Status & Manual Control */
static void drive_label(const char *dev, char *out, size_t size);   /* System section (1.39.0) */

/* --- Status tab: battery and NVMe drives (1.37.0), read from sysfs --- */
#ifndef DCC_POWER_SUPPLY_DIR
#define DCC_POWER_SUPPLY_DIR "/sys/class/power_supply"
#endif
#ifndef DCC_GUI_HWMON_DIR
#define DCC_GUI_HWMON_DIR "/sys/class/hwmon"
#endif

typedef struct {
    int present;                 /* a battery was found */
    int on_ac;                   /* mains adapter online (-1 unknown) */
    char status[24];             /* Charging, Discharging, Full, Not charging */
    int capacity;                /* % (-1 unknown) */
    double power_w;              /* charge/discharge rate (-1 unknown) */
    double hours;                /* to empty or to full (-1 unknown) */
    int health;                  /* full charge in % of the design (-1 unknown) */
    int cycles;                  /* (-1 unknown) */
} StatusBattery;

static long ps_read(const char *dir, const char *attr) {
    char p[512], buf[64];
    snprintf(p, sizeof p, "%s/%s", dir, attr);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int ok = fgets(buf, sizeof buf, f) != NULL;
    fclose(f);
    return ok ? strtol(buf, NULL, 10) : -1;
}

static void ps_read_str(const char *dir, const char *attr, char *out, size_t size) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, attr);
    out[0] = '\0';
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (!fgets(out, (int)size, f)) out[0] = '\0';
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
}

/* Battery values from <base>/<supply>: charge_* (uAh) with current/voltage,
 * or energy_* (uWh) with power_now - batteries report one or the other */
static void status_battery_read(const char *base, StatusBattery *b) {
    memset(b, 0, sizeof *b);
    b->on_ac = -1; b->capacity = -1; b->power_w = -1; b->hours = -1; b->health = -1; b->cycles = -1;
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char dir[512], type[32];
        snprintf(dir, sizeof dir, "%s/%s", base, de->d_name);
        ps_read_str(dir, "type", type, sizeof type);
        if (!strcmp(type, "Mains")) {
            long on = ps_read(dir, "online");
            if (on >= 0) b->on_ac = b->on_ac == 1 ? 1 : (int)on;
            continue;
        }
        if (strcmp(type, "Battery") != 0 || b->present) continue;
        if (ps_read(dir, "present") == 0) continue;
        b->present = 1;
        ps_read_str(dir, "status", b->status, sizeof b->status);
        b->capacity = (int)ps_read(dir, "capacity");
        b->cycles = (int)ps_read(dir, "cycle_count");
        double now, full, design, rate;    /* uAh and uA, or uWh and uW */
        long cur = ps_read(dir, "current_now"), volt = ps_read(dir, "voltage_now");
        long pw = ps_read(dir, "power_now");
        if (ps_read(dir, "charge_full") > 0) {
            now = ps_read(dir, "charge_now"); full = ps_read(dir, "charge_full");
            design = ps_read(dir, "charge_full_design");
            rate = cur;
            if (cur >= 0 && volt > 0) b->power_w = (double)cur * volt / 1e12;
        } else {
            now = ps_read(dir, "energy_now"); full = ps_read(dir, "energy_full");
            design = ps_read(dir, "energy_full_design");
            rate = pw;
            if (pw >= 0) b->power_w = pw / 1e6;
        }
        if (cur < 0 && pw < 0) b->power_w = -1;
        if (full > 0 && design > 0) b->health = (int)(full * 100.0 / design + 0.5);
        if (rate > 0 && now >= 0) {
            if (!strcmp(b->status, "Discharging")) b->hours = now / rate;
            else if (!strcmp(b->status, "Charging") && full > now) b->hours = (full - now) / rate;
        }
    }
    closedir(d);
}

/* "2:15 h" */
static void fmt_hours(double h, char *out, size_t size) {
    int m = (int)(h * 60 + 0.5);
    snprintf(out, size, "%d:%02d h", m / 60, m % 60);
}

/* The three battery lines of the Status tab */
static void status_battery_texts(const StatusBattery *b, char *charge, char *state, char *health, size_t size) {
    if (!b->present) {
        snprintf(charge, size, "%s", _("No battery"));
        state[0] = health[0] = '\0';
        return;
    }
    snprintf(charge, size, _("Charge: %d %%"), b->capacity < 0 ? 0 : b->capacity);
    char t[32] = "";
    if (b->hours > 0) fmt_hours(b->hours, t, sizeof t);
    if (!strcmp(b->status, "Discharging")) {
        if (b->power_w > 0 && t[0]) snprintf(state, size, _("Discharging: %.1f W, %s left"), b->power_w, t);
        else snprintf(state, size, "%s", _("Discharging"));
    } else if (!strcmp(b->status, "Charging")) {
        if (b->power_w > 0 && t[0]) snprintf(state, size, _("Charging: %.1f W, full in %s"), b->power_w, t);
        else snprintf(state, size, "%s", _("Charging"));
    } else if (!strcmp(b->status, "Full")) {
        snprintf(state, size, "%s", _("Full, on AC power"));
    } else {
        snprintf(state, size, "%s", b->on_ac == 1 ? _("Not charging, on AC power") : b->status);
    }
    if (b->health > 0 && b->cycles > 0)   /* some batteries always report 0 cycles */
        snprintf(health, size, _("Health: %d %% of design, %d cycles"), b->health, b->cycles);
    else if (b->health > 0)
        snprintf(health, size, _("Health: %d %% of design"), b->health);
    else
        health[0] = '\0';
}

static int nvme_line_cmp(const void *a, const void *b) {
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    int i = 0, j = 0;   /* "SSD 2: .." before "SSD 10: .." */
    const char *dx = strpbrk(x, "0123456789"), *dy = strpbrk(y, "0123456789");
    if (dx) i = atoi(dx);
    if (dy) j = atoi(dy);
    return i != j ? i - j : strcmp(x, y);
}

/* "SSD 1: 36°C" per NVMe drive (1.39.0: numbered like the System section), newline separated */
static void status_nvme_text(const char *base, char *out, size_t size) {
    out[0] = '\0';
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *de;
    size_t off = 0;
    char *names[16], *lines[16];
    int n = 0, nl = 0;
    while ((de = readdir(d)) != NULL && n < 16)
        if (!strncmp(de->d_name, "hwmon", 5)) names[n++] = g_strdup(de->d_name);
    closedir(d);
    for (int i = 0; i < n; i++)   /* stable order */
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[j], names[i]) < 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }
    for (int i = 0; i < n; i++) {
        char dir[512], nm[32], model[64];
        snprintf(dir, sizeof dir, "%s/%s", base, names[i]);
        ps_read_str(dir, "name", nm, sizeof nm);
        if (!strcmp(nm, "nvme")) {
            long t = ps_read(dir, "temp1_input");
            /* "SSD n" like the System section: controller nvme<n-1> (1.39.0) */
            char dev[600], *real, lab[32] = "NVMe";
            snprintf(dev, sizeof dev, "%s/device", dir);
            real = realpath(dev, NULL);
            if (real) { drive_label(strrchr(real, '/') + 1, lab, sizeof lab); free(real); }
            (void)model;
            if (t > 0 && nl < 16) lines[nl++] = g_strdup_printf("%s: %ld°C", lab, (t + 500) / 1000);
        }
        g_free(names[i]);
    }
    qsort(lines, (size_t)nl, sizeof lines[0], nvme_line_cmp);   /* SSD 1, SSD 2, ... */
    for (int i = 0; i < nl; i++) {
        if (off < size) off += (size_t)snprintf(out + off, size - off, "%s%s", off ? "\n" : "", lines[i]);
        g_free(lines[i]);
    }
}

static void status_update_local(AppData *app) {
    if (!app->bat_charge_label) return;
    StatusBattery b;
    char c[128], st[128], h[128];
    status_battery_read(DCC_POWER_SUPPLY_DIR, &b);
    status_battery_texts(&b, c, st, h, sizeof c);
    gtk_label_set_text(GTK_LABEL(app->bat_charge_label), c);
    gtk_label_set_text(GTK_LABEL(app->bat_state_label), st);
    gtk_label_set_text(GTK_LABEL(app->bat_health_label), h);
    gtk_widget_set_visible(app->bat_health_label, h[0] != '\0');
    /* whole laptop: only measurable while running on the battery */
    if (b.present && !strcmp(b.status, "Discharging") && b.power_w > 0)
        snprintf(c, sizeof c, _("Laptop (battery): %.1f W"), b.power_w);
    else
        snprintf(c, sizeof c, "%s", _("Laptop: on AC power"));
    gtk_label_set_text(GTK_LABEL(app->pw_bat_label), c);
    char nv[512];
    status_nvme_text(DCC_GUI_HWMON_DIR, nv, sizeof nv);
    gtk_label_set_text(GTK_LABEL(app->nvme_box), nv);
    gtk_widget_set_visible(app->nvme_box, nv[0] != '\0');
}

/* frame with a vertical box of left-aligned labels */
static GtkWidget *status_frame(const char *title, GtkWidget **box_out) {
    GtkWidget *frame = gtk_frame_new(title);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 15);
    gtk_container_add(GTK_CONTAINER(frame), box);
    *box_out = box;
    return frame;
}

static GtkWidget *status_label(GtkWidget *box, const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    return l;
}

/* --- Status tab: System section (1.39.0), read once at start ---
 * Everything here is readable without root. RAM type/speed and the serial
 * number would need root and are left out; so is the host name (it would
 * end up in screenshots). */
typedef struct {
    const char *dmi, *meminfo, *block, *os_release, *pci, *nvidia, *pci_ids;
} SysPaths;

typedef struct {
    char model[128], bios[128], ram[64], os[96], kernel[64];
    char gpus[256];
    char drives[640];            /* "SSD 1: model, 4 TB" per line */
} SysInfo;

static void read_line_file(const char *path, char *out, size_t size) {
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (!fgets(out, (int)size, f)) out[0] = '\0';
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
    g_strstrip(out);
}

/* 4000787030016 bytes -> "4 TB", 512110190592 -> "512 GB" (decimal, like the box) */
static void fmt_bytes(unsigned long long b, char *out, size_t size) {
    if (b >= 1000000000000ULL) {
        g_snprintf(out, size, "%.1f", b / 1e12);
        size_t n = strlen(out);   /* "4.0" or "4,0" (decimal comma) -> "4" */
        if (n > 2 && out[n - 1] == '0' && (out[n - 2] == '.' || out[n - 2] == ',')) out[n - 2] = '\0';
        g_strlcat(out, " TB", size);
    } else {
        g_snprintf(out, size, "%.0f GB", b / 1e9);
    }
}

/* "SSD n" for nvme controller k = n - 1, else the device name */
static void drive_label(const char *dev, char *out, size_t size) {
    int k;
    if (sscanf(dev, "nvme%d", &k) == 1) g_snprintf(out, size, _("SSD %d"), k + 1);
    else g_snprintf(out, size, "%s", dev);
}

static int name_cmp(const void *a, const void *b) {
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    int i, j;   /* nvme2n1 before nvme10n1 */
    if (sscanf(x, "nvme%d", &i) == 1 && sscanf(y, "nvme%d", &j) == 1 && i != j) return i - j;
    return strcmp(x, y);
}

/* "Arrow Lake-S [Intel Graphics]" -> "Intel Graphics"; from pci.ids */
static void pci_ids_name(const char *ids, unsigned vendor, unsigned device, char *out, size_t size) {
    out[0] = '\0';
    FILE *f = fopen(ids, "r");
    if (!f) return;
    char line[512], vname[128] = "";
    int in_vendor = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned id;
        if (line[0] != '\t') {
            if (in_vendor) break;
            if (sscanf(line, "%4x", &id) == 1 && id == vendor) {
                in_vendor = 1;
                g_snprintf(vname, sizeof vname, "%s", line + 6);
                vname[strcspn(vname, "\n")] = '\0';
            }
        } else if (in_vendor && line[1] != '\t' && sscanf(line + 1, "%4x", &id) == 1 && id == device) {
            char *name = line + 7;
            name[strcspn(name, "\n")] = '\0';
            char *lb = strrchr(name, '['), *rb = lb ? strchr(lb, ']') : NULL;
            if (lb && rb) { *rb = '\0'; name = lb + 1; }
            /* "NVIDIA Corporation" -> "NVIDIA" in front unless the name has it */
            char vshort[32];
            g_snprintf(vshort, sizeof vshort, "%s", vname);
            vshort[strcspn(vshort, " ")] = '\0';
            if (vshort[0] && !strstr(name, vshort)) g_snprintf(out, size, "%s %s", vshort, name);
            else g_snprintf(out, size, "%s", name);
            break;
        }
    }
    fclose(f);
}

static void sysinfo_read(const SysPaths *sp, SysInfo *si) {
    memset(si, 0, sizeof *si);
    char p[512], a[128], b[128], c[128];

    /* model: vendor + product, without the placeholders vendors leave in */
    static const char *const generic[] = { "Notebook", "To be filled by O.E.M.", "System manufacturer",
                                           "Default string", "System Product Name", "" };
    g_snprintf(p, sizeof p, "%s/sys_vendor", sp->dmi); read_line_file(p, a, sizeof a);
    g_snprintf(p, sizeof p, "%s/product_name", sp->dmi); read_line_file(p, b, sizeof b);
    g_snprintf(p, sizeof p, "%s/board_name", sp->dmi); read_line_file(p, c, sizeof c);
    int vendor_generic = 0;
    for (size_t i = 0; i < sizeof generic / sizeof generic[0]; i++) if (!strcmp(a, generic[i])) vendor_generic = 1;
    if (!b[0] || !strcmp(b, "System Product Name")) g_snprintf(b, sizeof b, "%s", c);
    if (vendor_generic) g_snprintf(si->model, sizeof si->model, "%s", b);
    else g_snprintf(si->model, sizeof si->model, "%s %s", a, b);
    if (c[0] && strcmp(c, b) != 0) {
        size_t n = strlen(si->model);
        g_snprintf(si->model + n, sizeof si->model - n, _(" (board %s)"), c);
    }

    /* BIOS: version (date, vendor) */
    g_snprintf(p, sizeof p, "%s/bios_version", sp->dmi); read_line_file(p, a, sizeof a);
    g_snprintf(p, sizeof p, "%s/bios_date", sp->dmi); read_line_file(p, b, sizeof b);
    g_snprintf(p, sizeof p, "%s/bios_vendor", sp->dmi); read_line_file(p, c, sizeof c);
    if (a[0]) {
        if (b[0] && c[0]) g_snprintf(si->bios, sizeof si->bios, "%s (%s, %s)", a, b, c);
        else if (b[0]) g_snprintf(si->bios, sizeof si->bios, "%s (%s)", a, b);
        else g_snprintf(si->bios, sizeof si->bios, "%s", a);
    }

    /* RAM: usable from MemTotal; installed = rounded up to 4 GB (the kernel
     * and the firmware keep a little) */
    FILE *f = fopen(sp->meminfo, "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        unsigned long long kb;
        if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) {
            double gib = kb / 1048576.0;
            int inst = ((int)(gib + 3.999) / 4) * 4;
            g_snprintf(si->ram, sizeof si->ram, _("%d GB (%.1f GB usable)"), inst, gib);
            break;
        }
    }
    if (f) fclose(f);

    /* drives: real disks with a model, no loop/zram/device mapper */
    DIR *d = opendir(sp->block);
    char *names[32];
    int n = 0;
    struct dirent *de;
    while (d && (de = readdir(d)) != NULL && n < 32) {
        const char *nm = de->d_name;
        if (nm[0] == '.' || !strncmp(nm, "loop", 4) || !strncmp(nm, "ram", 3) || !strncmp(nm, "zram", 4) ||
            !strncmp(nm, "dm-", 3) || !strncmp(nm, "md", 2) || !strncmp(nm, "sr", 2)) continue;
        names[n++] = g_strdup(nm);
    }
    if (d) closedir(d);
    qsort(names, (size_t)n, sizeof names[0], name_cmp);
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        char model[96], size_s[32], lab[32], sz[32];
        g_snprintf(p, sizeof p, "%s/%s/device/model", sp->block, names[i]); read_line_file(p, model, sizeof model);
        g_snprintf(p, sizeof p, "%s/%s/size", sp->block, names[i]); read_line_file(p, size_s, sizeof size_s);
        unsigned long long sectors = strtoull(size_s, NULL, 10);
        if (model[0] && sectors > 0 && off < sizeof si->drives) {
            drive_label(names[i], lab, sizeof lab);
            fmt_bytes(sectors * 512ULL, sz, sizeof sz);
            off += (size_t)g_snprintf(si->drives + off, sizeof si->drives - off, "%s%s: %s, %s",
                                    off ? "\n" : "", lab, model, sz);
        }
        g_free(names[i]);
    }

    /* OS and kernel */
    f = fopen(sp->os_release, "r");
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "PRETTY_NAME=", 12)) {
            char *v = line + 12;
            v[strcspn(v, "\n")] = '\0';
            if (*v == '"') { v++; char *q = strrchr(v, '"'); if (q) *q = '\0'; }
            g_snprintf(si->os, sizeof si->os, "%s", v);
            break;
        }
    }
    if (f) fclose(f);
    struct utsname u;
    if (uname(&u) == 0) g_snprintf(si->kernel, sizeof si->kernel, "%s", u.release);

    /* graphics: every display controller; NVIDIA's name from its driver
     * (does not wake a sleeping GPU), others from pci.ids */
    d = opendir(sp->pci);
    off = 0;
    while (d && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char cls[16], ven[16], dev[16], name[128] = "";
        g_snprintf(p, sizeof p, "%s/%s/class", sp->pci, de->d_name); read_line_file(p, cls, sizeof cls);
        if (strncmp(cls, "0x03", 4) != 0) continue;
        g_snprintf(p, sizeof p, "%s/%s/vendor", sp->pci, de->d_name); read_line_file(p, ven, sizeof ven);
        g_snprintf(p, sizeof p, "%s/%s/device", sp->pci, de->d_name); read_line_file(p, dev, sizeof dev);
        unsigned v = (unsigned)strtoul(ven, NULL, 16), dv = (unsigned)strtoul(dev, NULL, 16);
        if (v == 0x10de) {
            g_snprintf(p, sizeof p, "%s/%s/information", sp->nvidia, de->d_name);
            FILE *nf = fopen(p, "r");
            while (nf && fgets(line, sizeof line, nf)) {
                if (!strncmp(line, "Model:", 6)) {
                    g_snprintf(name, sizeof name, "%s", line + 6);
                    name[strcspn(name, "\n")] = '\0';
                    g_strstrip(name);
                    break;
                }
            }
            if (nf) fclose(nf);
        }
        if (!name[0]) pci_ids_name(sp->pci_ids, v, dv, name, sizeof name);
        if (!name[0]) g_snprintf(name, sizeof name, "%04x:%04x", v, dv);
        if (off < sizeof si->gpus)
            off += (size_t)g_snprintf(si->gpus + off, sizeof si->gpus - off, "%s%s", off ? " + " : "", name);
    }
    if (d) closedir(d);
}

static const SysInfo *sysinfo(void) {
    static SysInfo si;
    static int done;
    if (!done) {
        const char *ids = access("/usr/share/misc/pci.ids", R_OK) == 0 ? "/usr/share/misc/pci.ids" : "/usr/share/hwdata/pci.ids";
        SysPaths sp = { "/sys/class/dmi/id", "/proc/meminfo", "/sys/block", "/etc/os-release",
                        "/sys/bus/pci/devices", "/proc/driver/nvidia/gpus", ids };
        sysinfo_read(&sp, &si);
        done = 1;
    }
    return &si;
}

/* label: value rows (Active Profiles, System) */
static GtkWidget *status_grid(GtkWidget *frame) {
    GtkWidget *grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    return grid;
}

static GtkWidget *status_grid_row(GtkWidget *grid, int r, const char *name, const char *value) {
    GtkWidget *n = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(n), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(n), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), n, 0, r, 1, 1);
    GtkWidget *v = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(v), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(grid), v, 1, r, 1, 1);
    return v;
}

static GtkWidget *create_status_page(AppData *app) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 20);
    /* Layout (1.39.0): three columns of two sections each, then the per-core
     * frequencies next to the System section. Names (CPU, GPU, drives) are
     * shown once, in System; the other sections show live values. */
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_set_homogeneous(GTK_BOX(row1), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), row1, FALSE, FALSE, 0);
    GtkWidget *col[3];
    for (int i = 0; i < 3; i++) {
        col[i] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_box_pack_start(GTK_BOX(row1), col[i], TRUE, TRUE, 0);
    }
    GtkWidget *box, *f;

    /* Column 1: Temperatures, Fan Speed with the manual control */
    f = status_frame(_("Temperatures"), &box);
    app->cpu_temp_label = status_label(box, _("CPU: --°C"));
    app->gpu_temp_label = status_label(box, _("GPU: --°C"));
    app->nvme_box = status_label(box, "");   /* "SSD n: ..°C" */
    gtk_widget_set_no_show_all(app->nvme_box, TRUE);
    gtk_box_pack_start(GTK_BOX(col[0]), f, FALSE, FALSE, 0);

    f = status_frame(_("Fan Speed"), &box);
    app->cpu_fan_rpm_label = status_label(box, _("CPU Fan: -- RPM (--%)"));
    app->gpu_fan_rpm_label = status_label(box, _("GPU Fan: -- RPM (--%)"));
    app->fan_ec_label = status_label(box, "");   /* no fan control / direct EC access (1.41.0) */
    gtk_label_set_line_wrap(GTK_LABEL(app->fan_ec_label), TRUE);
    gtk_widget_set_no_show_all(app->fan_ec_label, TRUE);
    {   /* manual speed in one row (was its own section before 1.39.0) */
        GtkWidget *mrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        app->manual_row = mrow;
        gtk_widget_set_tooltip_text(mrow, _("Note: Enable Manual Mode to override automatic fan control. Changes take effect immediately."));
        gtk_box_pack_start(GTK_BOX(mrow), gtk_label_new(_("Manual Mode:")), FALSE, FALSE, 0);
        app->manual_mode_switch = GTK_WIDGET(gtk_switch_new());
        gtk_widget_set_valign(app->manual_mode_switch, GTK_ALIGN_CENTER);
        app->manual_mode_signal_id = g_signal_connect(app->manual_mode_switch, "state-set", G_CALLBACK(on_manual_mode_switched), app);
        gtk_box_pack_start(GTK_BOX(mrow), app->manual_mode_switch, FALSE, FALSE, 0);
        app->manual_minus_btn = gtk_button_new_with_label("-");
        gtk_widget_set_size_request(app->manual_minus_btn, 36, -1);
        gtk_widget_set_sensitive(app->manual_minus_btn, FALSE);
        g_signal_connect(app->manual_minus_btn, "clicked", G_CALLBACK(on_manual_speed_minus_clicked), app);
        gtk_box_pack_start(GTK_BOX(mrow), app->manual_minus_btn, FALSE, FALSE, 4);
        app->manual_speed_label = gtk_label_new("--");
        gtk_label_set_width_chars(GTK_LABEL(app->manual_speed_label), 3);
        gtk_label_set_xalign(GTK_LABEL(app->manual_speed_label), 1.0f);
        gtk_box_pack_start(GTK_BOX(mrow), app->manual_speed_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(mrow), gtk_label_new("%"), FALSE, FALSE, 0);
        app->manual_plus_btn = gtk_button_new_with_label("+");
        gtk_widget_set_size_request(app->manual_plus_btn, 36, -1);
        gtk_widget_set_sensitive(app->manual_plus_btn, FALSE);
        g_signal_connect(app->manual_plus_btn, "clicked", G_CALLBACK(on_manual_speed_plus_clicked), app);
        gtk_box_pack_start(GTK_BOX(mrow), app->manual_plus_btn, FALSE, FALSE, 4);
        gtk_box_pack_start(GTK_BOX(box), mrow, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(col[0]), f, TRUE, TRUE, 0);

    /* Column 2: GPU, Power */
    f = status_frame(_("GPU"), &box);
    app->gpu_name_label = status_label(box, "--");   /* state line: sleeping, no GPU */
    gtk_widget_set_no_show_all(app->gpu_name_label, TRUE);
    gtk_widget_show(app->gpu_name_label);
    app->gpu_clock_label = status_label(box, "");
    app->gpu_load_label = status_label(box, "");
    app->gpu_vram_label = status_label(box, "");
    gtk_box_pack_start(GTK_BOX(col[1]), f, FALSE, FALSE, 0);

    f = status_frame(_("Power"), &box);
    app->pw_cpu_label = status_label(box, _("CPU: -- W"));
    app->pw_gpu_label = status_label(box, _("GPU: -- W"));
    app->pw_bat_label = status_label(box, "");
    gtk_box_pack_start(GTK_BOX(col[1]), f, TRUE, TRUE, 0);

    /* Column 3: Battery, Active Profiles */
    f = status_frame(_("Battery"), &box);
    app->bat_charge_label = status_label(box, "--");
    app->bat_state_label = status_label(box, "");
    app->bat_health_label = status_label(box, "");
    gtk_widget_set_no_show_all(app->bat_health_label, TRUE);
    app->bat_limit_label = status_label(box, "");
    gtk_widget_set_no_show_all(app->bat_limit_label, TRUE);
    gtk_box_pack_start(GTK_BOX(col[2]), f, FALSE, FALSE, 0);

    f = gtk_frame_new(_("Active Profiles"));
    {
        GtkWidget *grid = status_grid(f);
        const char *names[4] = { _("Fan profile:"), _("CPU profile:"), _("Power mode:"), _("Keyboard profile:") };
        GtkWidget **vals[4] = { &app->active_fan_label, &app->active_cpu_label, &app->active_mode_label, &app->active_kbd_label };
        for (int r = 0; r < 4; r++) *vals[r] = status_grid_row(grid, r, names[r], "--");
    }
    gtk_box_pack_start(GTK_BOX(col[2]), f, TRUE, TRUE, 0);

    /* Row 2: Per-Core CPU Frequencies | System */
    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_set_homogeneous(GTK_BOX(row2), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), row2, TRUE, TRUE, 0);
    {
        GtkWidget *core_frame = gtk_frame_new(_("Per-Core CPU Frequencies"));
        GtkWidget *core_scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(core_scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(core_scrolled), 400);
        gtk_container_set_border_width(GTK_CONTAINER(core_scrolled), 5);
        gtk_container_add(GTK_CONTAINER(core_frame), core_scrolled);
        gtk_box_pack_start(GTK_BOX(row2), core_frame, TRUE, TRUE, 0);
        app->cpu_core_grid = gtk_grid_new();   /* 4 columns */
        gtk_grid_set_row_spacing(GTK_GRID(app->cpu_core_grid), 2);
        gtk_grid_set_column_spacing(GTK_GRID(app->cpu_core_grid), 10);
        gtk_container_add(GTK_CONTAINER(core_scrolled), app->cpu_core_grid);
        app->cpu_core_count = 0;
        for (int i = 0; i < 32; i++) app->cpu_core_freq_labels[i] = NULL;
    }
    {
        const SysInfo *si = sysinfo();
        GtkWidget *sf = gtk_frame_new(_("System"));
        GtkWidget *grid = status_grid(sf);
        char cpu[160];
        cpu_ident_text(cpu_ident(), cpu, sizeof cpu);
        int r = 0;
        if (si->model[0]) status_grid_row(grid, r++, _("Model:"), si->model);
        if (si->bios[0]) status_grid_row(grid, r++, _("BIOS:"), si->bios);
        status_grid_row(grid, r++, _("Processor:"), cpu);
        if (si->gpus[0]) status_grid_row(grid, r++, _("Graphics:"), si->gpus);
        if (si->ram[0]) status_grid_row(grid, r++, _("Memory:"), si->ram);
        if (si->drives[0]) status_grid_row(grid, r++, _("Drives:"), si->drives);
        if (si->os[0]) status_grid_row(grid, r++, _("Operating system:"), si->os);
        if (si->kernel[0]) status_grid_row(grid, r++, _("Kernel:"), si->kernel);
        gtk_box_pack_start(GTK_BOX(row2), sf, TRUE, TRUE, 0);
    }

    return vbox;
}

static void status_show_ec(AppData *app) {
    if (!app->fan_ec_label) return;
    gboolean none = !strcmp(app->ec_access, "none");
    if (none) {
        label_set_changed(app->fan_ec_label, _("No fan control on this laptop: the firmware controls the fans."));
        tooltip_set_changed(app->fan_ec_label, app->ec_reason);
        widget_set_shown(app->fan_ec_label, TRUE);
        label_set_changed(app->cpu_fan_rpm_label, _("CPU Fan: --"));
        label_set_changed(app->gpu_fan_rpm_label, _("GPU Fan: --"));
    } else if (!strcmp(app->ec_access, "ports")) {
        label_set_changed(app->fan_ec_label, _("Direct EC access (switched on in Settings; only safe on Clevo laptops)"));
        tooltip_set_changed(app->fan_ec_label, NULL);
        widget_set_shown(app->fan_ec_label, TRUE);
    } else if (!strcmp(app->ec_access, "hwmon")) {
        char t[160];
        snprintf(t, sizeof t, _("Generic fan control: fans of the %s driver (switched on in Settings)"), app->hw_driver);
        label_set_changed(app->fan_ec_label, t);
        tooltip_set_changed(app->fan_ec_label, NULL);
        widget_set_shown(app->fan_ec_label, TRUE);
    } else {
        widget_set_shown(app->fan_ec_label, FALSE);
    }
    if (app->manual_row && gtk_widget_get_visible(app->manual_row) == none) {
        gtk_widget_set_no_show_all(app->manual_row, none);
        if (none) gtk_widget_hide(app->manual_row); else gtk_widget_show_all(app->manual_row);
    }
    settings_update_ec(app);
}

/* Refresh timer callback - fetch STATUS and update UI every 2 seconds */
/* Status tab: the profiles in use right now (1.30.0) */
static void status_set(GtkWidget *label, const char *text, int grey) {
    if (!label) return;
    if (grey) {
        gchar *m = g_markup_printf_escaped("<span foreground='#888888'>%s</span>", text);
        gtk_label_set_markup(GTK_LABEL(label), m);
        g_free(m);
    } else {
        gtk_label_set_text(GTK_LABEL(label), text);
    }
}

static void status_update_active_profiles(AppData *app) {
    if (!app->active_fan_label) return;
    char buf[256];
    /* fan */
    if (dcc_component_disabled(COMP_FAN)) status_set(app->active_fan_label, _("Fan control switched off"), 1);
    else if (!app->connected) status_set(app->active_fan_label, _("Fan daemon not connected"), 1);
    else {
        const char *name = (app->profile_names && app->active_profile_idx >= 0 && app->active_profile_idx < app->num_profiles)
                         ? app->profile_names[app->active_profile_idx] : "--";
        if (app->in_manual_mode) snprintf(buf, sizeof buf, _("%s (manual mode: %d%%)"), name, app->manual_current_speed);
        else snprintf(buf, sizeof buf, "%s", name);
        status_set(app->active_fan_label, buf, 0);
    }
    /* CPU profile and power mode */
    if (dcc_component_disabled(COMP_CPU)) {
        status_set(app->active_cpu_label, _("CPU control switched off"), 1);
        status_set(app->active_mode_label, _("CPU control switched off"), 1);
    } else if (!app->cpu_connected) {
        status_set(app->active_cpu_label, _("CPU daemon not connected"), 1);
        status_set(app->active_mode_label, _("CPU daemon not connected"), 1);
    } else {
        const char *cpu = "--";
        gchar *disp = NULL;
        GtkTreeIter it;
        if (app->cpu_running_profile_idx >= 0 && app->cpu_custom_profile_store &&
            gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(app->cpu_custom_profile_store), &it, NULL, app->cpu_running_profile_idx)) {
            gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &it, 0, &disp, -1);
            if (disp) cpu = cpu_profile_name_from_display(disp);
        }
        status_set(app->active_cpu_label, cpu, 0);
        g_free(disp);
        const char *mode = app->cpu_page_governor_label ? gtk_label_get_text(GTK_LABEL(app->cpu_page_governor_label)) : NULL;
        if (app->cpu_pp_current[0]) {          /* firmware power profile (1.43.0) */
            snprintf(buf, sizeof buf, _("%s (firmware: %s)"), mode && *mode ? mode : "--", app->cpu_pp_current);
            status_set(app->active_mode_label, buf, 0);
        } else {
            status_set(app->active_mode_label, mode && *mode ? mode : "--", 0);
        }
    }
    /* keyboard: asked over an existing connection only (no reconnect here) */
    if (dcc_component_disabled(COMP_KEYBOARD)) status_set(app->active_kbd_label, _("Keyboard backlight switched off"), 1);
    else if (!app->kbd_connected || app->kbd_socket_fd < 0) status_set(app->active_kbd_label, _("Keyboard daemon not connected"), 1);
    else {
        const char *r = send_keyboard_command_once(app, "LIST_PROFILES");
        const char *act = r ? strstr(r, "[*] ") : NULL;
        if (act) {
            act += 4;
            size_t n = strcspn(act, "\n");
            snprintf(buf, sizeof buf, "%.*s", (int)MIN(n, sizeof buf - 1), act);
            status_set(app->active_kbd_label, buf, 0);
        } else {
            status_set(app->active_kbd_label, "--", 0);
        }
    }
}
