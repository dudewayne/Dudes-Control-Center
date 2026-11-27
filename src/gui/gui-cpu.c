// SPDX-License-Identifier: GPL-2.0-only
/*
 * gui-cpu.c
 * CPU Profiles tab: power modes, per-core frequency profiles, hard limit.
 * Part of dcc-gui.c, which #includes it: one translation unit, so the
 * static functions and AppData stay shared (split up in 1.48.1).
 */

/* Firmware power profile (1.43.0): the choices the laptop offers fill the
 * CPU profile editor's combo; without any the row stays hidden */
static void cpu_pp_set_choices(AppData *app, const char *choices) {
    if (!app->cpu_pp_combo || !strcmp(app->cpu_pp_choices, choices)) return;
    g_strlcpy(app->cpu_pp_choices, choices, sizeof app->cpu_pp_choices);
    gchar *keep = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->cpu_pp_combo)));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->cpu_pp_combo));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->cpu_pp_combo), "", _("Unchanged"));
    gchar **v = g_strsplit(choices, " ", -1);
    for (int i = 0; v[i]; i++)
        if (v[i][0]) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->cpu_pp_combo), v[i], v[i]);
    g_strfreev(v);
    if (!keep || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->cpu_pp_combo), keep))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->cpu_pp_combo), "");
    g_free(keep);
    gtk_widget_set_no_show_all(app->cpu_pp_row, choices[0] == '\0');
    if (choices[0]) gtk_widget_show_all(app->cpu_pp_row); else gtk_widget_hide(app->cpu_pp_row);
}

static const char *cpu_pp_selected(const AppData *app) {
    const char *id = app->cpu_pp_combo ? gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->cpu_pp_combo)) : NULL;
    return id ? id : "";
}

/* Idle callback: populate CPU core frequency grid (defer until GTK realizes widgets on the page) */
static gboolean populate_cpu_core_grid_idle(gpointer data) {
    AppData *app = (AppData *)data;
    g_print("[GUI] populate_cpu_core_grid_idle: cpu_core_count=%d, active_idx=%d\n",
            app->cpu_core_count, app->cpu_active_custom_profile_idx);

    if (app->cpu_core_count > 0 && app->cpu_active_custom_profile_idx >= 0) {
        build_cpu_core_freq_grid(app, app->cpu_core_count);
        update_cpu_core_freq_spinners(app, app->cpu_active_custom_profile_idx);

        /* Restore checkbox state and enforce spinner disabled/enabled state */
        int idx = app->cpu_active_custom_profile_idx;
        char info_cmd[64];
        snprintf(info_cmd, sizeof(info_cmd), "GET_CUSTOM_PROFILE_INFO|%d", idx);
        const char *info_response = send_cpu_command(app, info_cmd);
        int all_cores = 1; /* Default: apply to all cores */
        if (info_response) {
            const char *aac_ptr = strstr(info_response, "APPLY_ALL:");
            if (aac_ptr) {
                all_cores = atoi(aac_ptr + 10);
            }
        }

        g_signal_handler_block(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox), all_cores != 0);
        g_signal_handler_unblock(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);

        if (all_cores) {
            sync_core0_to_all_cores(app);
            for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], FALSE);
                gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], FALSE);
            }
        } else {
            for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], TRUE);
                gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], TRUE);
            }
        }
    }
    return FALSE; /* Don't repeat */
}

/* Parse CPU STATUS response and update UI */
/* --- Processor name and core types (1.38.0), read once ---
 * Name from /proc/cpuinfo without "(R)", "(TM)" and Intel's " CPU @ x GHz".
 * Hybrid Intel CPUs list their performance and efficiency cores in
 * /sys/devices/cpu_core/cpus and cpu_atom/cpus; physical cores are counted
 * with the first CPU of each topology/core_cpus_list (Hyper-Threading). */
#ifndef DCC_CPUINFO
#define DCC_CPUINFO "/proc/cpuinfo"
#endif
#ifndef DCC_SYSDEV_DIR
#define DCC_SYSDEV_DIR "/sys/devices"
#endif
#define CPU_IDENT_MAX 256

typedef struct {
    char name[96];
    int hybrid;                         /* P and E cores listed */
    char type[CPU_IDENT_MAX];           /* 'P', 'E' or 0 per logical CPU */
    int p_cores, e_cores;               /* physical cores */
} CpuIdent;

static void cpu_name_clean(const char *raw, char *out, size_t size) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s", raw);
    char *at = strstr(buf, " CPU @");
    if (!at) at = strstr(buf, " @ ");
    if (at) *at = '\0';
    static const char *const drop[] = { "(R)", "(r)", "(TM)", "(tm)" };
    for (size_t d = 0; d < sizeof drop / sizeof drop[0]; d++)
        for (char *q; (q = strstr(buf, drop[d])) != NULL;)
            memmove(q, q + strlen(drop[d]), strlen(q + strlen(drop[d])) + 1);
    /* single spaces, no leading/trailing blanks */
    size_t w = 0;
    for (size_t r = 0; buf[r] && w + 1 < size; r++) {
        if (isspace((unsigned char)buf[r])) {
            if (w > 0 && out[w - 1] != ' ') out[w++] = ' ';
        } else {
            out[w++] = buf[r];
        }
    }
    while (w > 0 && out[w - 1] == ' ') w--;
    out[w] = '\0';
}

/* "0-7,9,12-13" -> mark[cpu] = val; returns the number of CPUs marked */
static int cpulist_mark(const char *list, char *mark, int max, char val) {
    int n = 0;
    const char *q = list;
    while (*q) {
        char *end;
        long a = strtol(q, &end, 10), b;
        if (end == q) break;
        b = a;
        if (*end == '-') { q = end + 1; b = strtol(q, &end, 10); }
        for (long c = a; c <= b && c < max; c++) if (c >= 0) { mark[c] = val; n++; }
        q = end;
        if (*q == ',') q++;
        else break;
    }
    return n;
}

static void cpu_ident_read(const char *cpuinfo, const char *sysdev, CpuIdent *ci) {
    memset(ci, 0, sizeof *ci);
    FILE *f = fopen(cpuinfo, "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "model name", 10)) {
            char *c = strchr(line, ':');
            if (c) {
                c[strcspn(c, "\n")] = '\0';
                cpu_name_clean(c + 1, ci->name, sizeof ci->name);
            }
            break;
        }
    }
    if (f) fclose(f);
    char p[512], list[256];
    static const char *const kinds[2] = { "cpu_core", "cpu_atom" };
    int found = 0;
    for (int k = 0; k < 2; k++) {
        snprintf(p, sizeof p, "%s/%s/cpus", sysdev, kinds[k]);
        FILE *lf = fopen(p, "r");
        if (!lf) continue;
        if (fgets(list, sizeof list, lf) && cpulist_mark(list, ci->type, CPU_IDENT_MAX, k ? 'E' : 'P') > 0) found |= 1 << k;
        fclose(lf);
    }
    ci->hybrid = found == 3;
    if (!ci->hybrid) { memset(ci->type, 0, sizeof ci->type); return; }
    for (int c = 0; c < CPU_IDENT_MAX; c++) {
        if (!ci->type[c]) continue;
        int first = c;                    /* no topology file: count every CPU */
        snprintf(p, sizeof p, "%s/system/cpu/cpu%d/topology/core_cpus_list", sysdev, c);
        FILE *tf = fopen(p, "r");
        if (tf) {
            if (fgets(list, sizeof list, tf)) first = atoi(list);
            fclose(tf);
        }
        if (first == c) { if (ci->type[c] == 'P') ci->p_cores++; else ci->e_cores++; }
    }
}

static const CpuIdent *cpu_ident(void) {
    static CpuIdent ci;
    static int done;
    if (!done) { cpu_ident_read(DCC_CPUINFO, DCC_SYSDEV_DIR, &ci); done = 1; }
    return &ci;
}

/* " (P)", " (E)" or "" after "Core N" */
static const char *cpu_core_tag(int cpu) {
    const CpuIdent *ci = cpu_ident();
    if (cpu < 0 || cpu >= CPU_IDENT_MAX || !ci->hybrid) return "";
    return ci->type[cpu] == 'P' ? " (P)" : ci->type[cpu] == 'E' ? " (E)" : "";
}

/* "Intel Core Ultra 9 275HX (8 P-cores, 16 E-cores)" */
static void cpu_ident_text(const CpuIdent *ci, char *out, size_t size) {
    if (!ci->name[0]) { snprintf(out, size, "%s", _("N/A")); return; }
    if (ci->hybrid && ci->p_cores > 0 && ci->e_cores > 0)
        snprintf(out, size, _("%s (%d P-cores, %d E-cores)"), ci->name, ci->p_cores, ci->e_cores);
    else
        snprintf(out, size, "%s", ci->name);
}

static void parse_cpu_status(AppData *app, const char *response) {
    char buffer[8192];
    strncpy(buffer, response, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';



    int cpu_temp = 0;
    float cpu_load = 0.0f;
    int num_cpus = 0;
    double cpu_power = -1;          /* CPU daemons since 1.37.0 */
    int cpu_power_limit = -1;
    char active_governor[64] = "N/A";
    char cur_gov[64] = "", cur_epp[64] = "";   /* raw values (daemons since 1.20.3) */
    char freq_values[256] = "N/A";
    char min_max_freq[128] = "N/A";

    /* Per-core frequency storage */
    char core_freqs[32][64];
    int core_count = 0;
    for (int i = 0; i < 32; i++) {
        strncpy(core_freqs[i], "N/A", sizeof(core_freqs[i]) - 1);
        core_freqs[i][sizeof(core_freqs[i]) - 1] = '\0';
    }

    char *saveptr = NULL;
    char *line = strtok_r(buffer, "\n", &saveptr);
    while (line != NULL) {
        if (strncmp(line, "CPU_TEMP=", 9) == 0) {
            cpu_temp = atoi(line + 9);
        } else if (strncmp(line, "CPU_POWER_W=", 12) == 0) {
            cpu_power = num_c(line + 12);
        } else if (strncmp(line, "CPU_POWER_LIMIT_W=", 18) == 0) {
            cpu_power_limit = atoi(line + 18);
        } else if (strncmp(line, "PLATFORM_PROFILE_CHOICES=", 25) == 0) {
            cpu_pp_set_choices(app, line + 25);
        } else if (strncmp(line, "PLATFORM_PROFILE=", 17) == 0) {
            g_strlcpy(app->cpu_pp_current, line + 17, sizeof app->cpu_pp_current);
        } else if (strncmp(line, "BATTERY_LIMIT=", 14) == 0) {
            app->bat_limit = atoi(line + 14);
            app->bat_limit_seen = 1;
        } else if (strncmp(line, "BATTERY_LIMIT_DEVICE=", 21) == 0) {
            g_strlcpy(app->bat_limit_dev, line + 21, sizeof app->bat_limit_dev);
            settings_update_battery(app);      /* the last of the three lines */
        } else if (strncmp(line, "CPU_LOAD=", 9) == 0) {
            cpu_load = num_c(line + 9);
        } else if (strncmp(line, "NUM_CPUS=", 9) == 0) {
            num_cpus = atoi(line + 9);
        } else if (strncmp(line, "ACTIVE_GOVERNOR=", 16) == 0) {
            strncpy(active_governor, line + 16, sizeof(active_governor) - 1);
        } else if (strncmp(line, "GOVERNOR=", 9) == 0) {
            snprintf(cur_gov, sizeof cur_gov, "%s", line + 9);
        } else if (strncmp(line, "EPP=", 4) == 0) {
            snprintf(cur_epp, sizeof cur_epp, "%s", line + 4);
        } else if (strncmp(line, "MIN_FREQ=", 9) == 0) {
            strncpy(min_max_freq, line + 9, sizeof(min_max_freq) - 1);
        } else if (strncmp(line, "MAX_FREQ=", 9) == 0) {
            strncat(min_max_freq, " / ", sizeof(min_max_freq) - strlen(min_max_freq) - 1);
            strncat(min_max_freq, line + 9, sizeof(min_max_freq) - strlen(min_max_freq) - 1);
        } else {
            /* Parse per-core frequencies: CPU_N_FREQ=value (value may contain spaces like "2.40 GHz") */
            int core_idx = -1;
            char freq_tmp[64] = {0};
            char *freq_marker = strstr(line, "_FREQ=");
            if (freq_marker != NULL && strncmp(line, "CPU_", 4) == 0) {
                core_idx = atoi(line + 4);
                if (core_idx >= 0 && core_idx < 32) {
                    snprintf(freq_tmp, sizeof(freq_tmp), "%s", freq_marker + 6);
                    snprintf(core_freqs[core_idx], sizeof(core_freqs[core_idx]), "%s", freq_tmp);
                    if (core_idx >= core_count) {
                        core_count = core_idx + 1;
                    }
                }
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* Set current frequency from first core if available */
    if (core_count > 0) {
        strncpy(freq_values, core_freqs[0], sizeof(freq_values) - 1);
    }

    /* Update labels */
    char temp_text[32], load_text[32], num_text[32];
    snprintf(temp_text, sizeof(temp_text), "%d°C", cpu_temp);
    snprintf(load_text, sizeof(load_text), "%.1f%%", cpu_load);
    snprintf(num_text, sizeof(num_text), "%d", num_cpus);

    gtk_label_set_text(GTK_LABEL(app->cpu_page_temp_label), temp_text);
    gtk_label_set_text(GTK_LABEL(app->cpu_page_load_label), load_text);
    gtk_label_set_text(GTK_LABEL(app->cpu_page_num_label), num_text);
    if (app->pw_cpu_label) {   /* Status tab, Power (1.37.0) */
        char pw[96];
        if (cpu_power >= 0 && cpu_power_limit > 0)
            snprintf(pw, sizeof pw, _("CPU: %.1f W (sustained limit %d W)"), cpu_power, cpu_power_limit);
        else if (cpu_power >= 0)
            snprintf(pw, sizeof pw, _("CPU: %.1f W"), cpu_power);
        else
            snprintf(pw, sizeof pw, "%s", _("CPU: -- W"));
        gtk_label_set_text(GTK_LABEL(app->pw_cpu_label), pw);
    }
    /* "Balanced (powersave, balance_performance)" */
    gchar *mode_text = cur_gov[0]
        ? (cur_epp[0] && strcmp(cur_gov, "performance") != 0
               ? g_strdup_printf("%s (%s, %s)", active_governor, cur_gov, cur_epp)
               : g_strdup_printf("%s (%s)", active_governor, cur_gov))
        : g_strdup(active_governor);
    gtk_label_set_text(GTK_LABEL(app->cpu_page_governor_label), mode_text);
    g_free(mode_text);
    gtk_label_set_text(GTK_LABEL(app->cpu_page_freq_label), freq_values);
    gtk_label_set_text(GTK_LABEL(app->cpu_page_min_max_freq_label), min_max_freq);

    /* Populate per-core frequency labels on first call, update on subsequent calls */

    if (app->cpu_core_count == 0 && core_count > 0) {
        /* First time: create labels in a 4-column grid */
        app->cpu_core_count = core_count;

        for (int i = 0; i < core_count && i < 32; i++) {
            gchar *label_text = g_strdup_printf(_("Core %d%s: %s"), i, cpu_core_tag(i), core_freqs[i]);
            app->cpu_core_freq_labels[i] = gtk_label_new(label_text);
            g_free(label_text);
            gtk_label_set_xalign(GTK_LABEL(app->cpu_core_freq_labels[i]), 0.0);

            /* 4 columns layout: row = i / 4, col = i % 4 */
            int row = i / 4;
            int col = i % 4;
            gtk_grid_attach(GTK_GRID(app->cpu_core_grid), app->cpu_core_freq_labels[i], col, row, 1, 1);
            gtk_widget_show(app->cpu_core_freq_labels[i]);
        }
        gtk_widget_show_all(GTK_WIDGET(app->cpu_core_grid));

    } else if (app->cpu_core_count > 0) {
        /* Subsequent calls: just update label text */
        for (int i = 0; i < app->cpu_core_count && i < 32; i++) {
            if (app->cpu_core_freq_labels[i] != NULL) {
                char label_text[128];
                snprintf(label_text, sizeof(label_text), _("Core %d%s: %s"), i, cpu_core_tag(i), core_freqs[i]);
                gtk_label_set_text(GTK_LABEL(app->cpu_core_freq_labels[i]), label_text);
            }
        }
    }
}

/* CPU refresh timer callback - fetch CPU STATUS and update UI every 5 seconds */
/* Load what the CPU tab needs after connecting to the CPU daemon: power
 * modes, core count (STATUS) and the hard limit. Used at startup and when
 * the CPU refresh timer reconnects. */
static void cpu_load_initial_data(AppData *app) {
    /* Load governors into combo box */
    load_cpu_governors(app);

    /* Send an immediate STATUS query so cpu_core_count is available before the user switches tabs */
    {
        const char *immediate_status = send_cpu_command(app, "STATUS");
        if (immediate_status != NULL && strncmp(immediate_status, "ERROR", 5) != 0) {
            parse_cpu_status(app, immediate_status);
        }
    }

    /* Load current hard limit from daemon */
    {
        const char *hard_resp = send_cpu_command(app, "GET_HARD_LIMIT");
        if (hard_resp && strncmp(hard_resp, "OK", 2) == 0) {
            const char *eq = strstr(hard_resp, "HARD_LIMIT=");
            if (eq) {
                guint64 hz = g_ascii_strtoull(eq + 11, NULL, 10);
                double ghz = (double)hz / 1000000000.0;
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->cpu_hard_limit_spinner), ghz);
                g_print("[GUI] Loaded hard limit from daemon: %.2f GHz (%" G_GUINT64_FORMAT " Hz)\n", ghz, hz);
                /* Spinner upper bounds are updated in build_cpu_core_freq_grid() */
            }
        }
    }
}

static gboolean cpu_refresh_timer_callback(gpointer user_data) {
    AppData *app = (AppData *)user_data;

    if (!app->cpu_connected) {
        /* Reconnect after the CPU daemon was restarted or updated (the GUI
         * used to stay disconnected until it was reopened) */
        if (app->cpu_socket_fd >= 0) { close(app->cpu_socket_fd); app->cpu_socket_fd = -1; }
        connect_to_cpu_daemon(app);
        update_daemon_status_labels(app);
        if (!app->cpu_connected) return G_SOURCE_CONTINUE;
        cpu_load_initial_data(app);
        load_cpu_custom_profiles_select(app,
            gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo)));
        update_tray_menu(app);
    }

    const char *response = send_cpu_command(app, "STATUS");
    if (response != NULL && strncmp(response, "ERROR", 5) != 0) {
        parse_cpu_status(app, response);
    }

    return G_SOURCE_CONTINUE;
}

/* Load governor profiles into the CPU combo box */
/* Power-mode list rows: column 0 = name (Performance/Balanced/Power Save),
 * column 1 = key sent to the daemon, "governor" or "governor:epp".
 * Returns the row for a profile's governor + EPP, or -1. Exact key first,
 * then by meaning, so profiles saved before EPP support (e.g. "schedutil"
 * on a system without it) still show the mode they were meant to use. */
static int gui_find_power_mode_row(GtkListStore *store, const char *gov, const char *epp) {
    if (!store || !gov || !*gov) return -1;
    char key[96];
    if (epp && *epp) snprintf(key, sizeof key, "%s:%s", gov, epp);
    else snprintf(key, sizeof key, "%s", gov);

    const char *meaning = NULL;
    if (strcmp(gov, "performance") == 0) meaning = "Performance";
    else if (strcmp(gov, "schedutil") == 0 || strcmp(gov, "ondemand") == 0 || strcmp(gov, "conservative") == 0)
        meaning = "Balanced";
    else if (strcmp(gov, "powersave") == 0)
        meaning = (epp && strncmp(epp, "balance_performance", 19) == 0) ? "Balanced" : "Power Save";

    int by_meaning = -1, idx = 0;
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it);
    while (ok) {
        gchar *name = NULL, *row_key = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &it, 0, &name, 1, &row_key, -1);
        int exact = row_key && strcmp(row_key, key) == 0;
        if (by_meaning < 0 && meaning && name && strcmp(name, meaning) == 0) by_meaning = idx;
        g_free(name);
        g_free(row_key);
        if (exact) return idx;
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &it);
        idx++;
    }
    return by_meaning;
}

static void load_cpu_governors(AppData *app) {
    if (!app->cpu_connected) {
        g_print("[GUI] CPU not connected, skipping governor load\n");
        return;
    }

    const char *response = send_cpu_command(app, "LIST_GOVERNORS");
    if (response == NULL || strncmp(response, "ERROR", 5) == 0) {
        g_print("[GUI] Failed to load governors: %s\n", response);
        return;
    }

    /* Clear existing list */
    gtk_list_store_clear(app->cpu_profile_store);

    /* Parse response: "GOVERNORS\n    Name (gov=..., min=..., max=...)\n[*] Name (gov=..., min=..., max=...)\n" */
    char buffer[8192];
    strncpy(buffer, response, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char *saveptr = NULL;
    char *line = strtok_r(buffer, "\n", &saveptr);
    int active_idx = 0;
    int count = 0;

    while (line != NULL) {
        /* Skip header line */
        if (strncmp(line, "GOVERNORS", 9) == 0) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* Skip empty lines */
        if (line[0] == '\0' || strlen(line) == 0) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* Extract profile name: skip leading markers ([*] or spaces), stop at '(' */
        char *name_start = line;
        while (*name_start == ' ' || *name_start == '[' || *name_start == ']' || *name_start == '*') {
            name_start++;
        }

        /* Extract governor string from "gov=xxx" BEFORE truncating the line */
        char gov_str[80] = "schedutil";
        char *gov_ptr = strstr(line, "gov=");
        if (gov_ptr) {
            char *gov_start = gov_ptr + 4;
            char *gov_end = strchr(gov_start, ',');
            if (gov_end) {
                int gov_len = gov_end - gov_start;
                if (gov_len > 0 && gov_len < (int)sizeof(gov_str)) {
                    memcpy(gov_str, gov_start, gov_len);
                    gov_str[gov_len] = '\0';
                }
            }
        }

        /* EPP of the policy ("epp=balance_performance," or "epp=-,") */
        char epp_str[32] = "";
        char *epp_ptr = strstr(line, "epp=");
        if (epp_ptr) {
            char *epp_start = epp_ptr + 4;
            size_t epp_len = strcspn(epp_start, ",)");
            if (epp_len > 0 && epp_len < sizeof(epp_str) && strncmp(epp_start, "-", epp_len) != 0) {
                memcpy(epp_str, epp_start, epp_len);
                epp_str[epp_len] = '\0';
            }
        }
        if (epp_str[0]) {
            /* key sent to the daemon when a profile is saved: governor:epp */
            size_t gl = strlen(gov_str);
            if (gl + 1 + strlen(epp_str) < sizeof(gov_str)) {
                gov_str[gl] = ':';
                memcpy(gov_str + gl + 1, epp_str, strlen(epp_str) + 1);
            }
        }

        char *paren = strchr(name_start, '(');
        if (paren) {
            *paren = '\0';
        }

        /* Trim trailing spaces */
        int len = strlen(name_start);
        while (len > 0 && name_start[len - 1] == ' ') {
            name_start[len - 1] = '\0';
            len--;
        }

        if (strlen(name_start) > 0) {
            /* Check if this is the active profile */
            if (strstr(line, "[*]") != NULL) {
                active_idx = count;
            }

            GtkTreeIter iter;
            gtk_list_store_append(app->cpu_profile_store, &iter);
            gtk_list_store_set(app->cpu_profile_store, &iter, 0, name_start, 1, gov_str, -1);
            g_print("[GUI]   Added governor row: name='%s' gov='%s'\n", name_start, gov_str);
            count++;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* Select the active profile (block signal to prevent callback during init) */
    if (active_idx < count) {
        g_signal_handler_block(app->cpu_profile_combo, app->cpu_profile_combo_signal_id);
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->cpu_profile_combo), active_idx);
        g_signal_handler_unblock(app->cpu_profile_combo, app->cpu_profile_combo_signal_id);
    }

    g_print("[GUI] Loaded %d governor profiles into combo box (active idx=%d)\n", count, active_idx);
}

/* Callback: Apply governor from combo box selection */
static void on_cpu_governor_changed(GtkWidget *widget, gpointer user_data) {
    AppData *app = (AppData *)user_data;

    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &iter)) {
        return;
    }

    gchar *governor_name = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_profile_store), &iter, 0, &governor_name, -1);
    if (!governor_name) {
        return;
    }

    /* With a per-core profile loaded, the power mode belongs to that profile
     * and is stored on Save. If that profile is the active one, the mode is
     * also applied right away (governor + EPP only, its frequencies stay);
     * for another profile it takes effect when that profile is activated. */
    if (app->cpu_active_custom_profile_idx >= 0) {
        mark_cpu_freq_profiles_modified(app);
        if (app->cpu_active_custom_profile_idx == app->cpu_running_profile_idx) {
            char command[256];
            snprintf(command, sizeof(command), "SET_POWER_MODE|%s", governor_name);
            const char *response = send_cpu_command(app, command);
            int ok = response && strncmp(response, "OK", 2) == 0;
            gui_event("[GUI] Power mode '%s' for the active profile: %s\n", governor_name, response ? response : "NULL");
            show_status(app, ok ? _("Power mode applied - click Save to keep it in the profile")
                                : _("Failed to apply the power mode"));
        } else {
            gui_event("[GUI] Power mode '%s' selected for an inactive profile (saved with Save, applied with Activate)\n",
                    governor_name);
            show_status(app, _("Power mode changed - click Save to store it in the profile, Activate to apply it"));
        }
        g_free(governor_name);
        return;
    }

    g_print("[GUI] Applying governor: %s\n", governor_name);

    /* Build command: APPLY_GOVERNOR <name> */
    char command[256];
    snprintf(command, sizeof(command), "APPLY_GOVERNOR|%s", governor_name);

    const char *response = send_cpu_command(app, command);
    if (response && strncmp(response, "OK", 2) == 0) {
        show_status(app, _("Governor applied successfully"));
        gui_event("[GUI] Governor '%s' applied successfully\n", governor_name);
    } else {
        show_status(app, _("Failed to apply governor"));
        g_print("[GUI] Failed to apply governor: %s\n", response);
    }

    g_free(governor_name);
}

/* ============================================================
 * CPU Custom Frequency Profile Management callbacks
 * ============================================================ */

/* Load custom frequency profiles from daemon into combo box */
/* Reload the profile list and select the active profile */
static void load_cpu_custom_profiles(AppData *app) {
    load_cpu_custom_profiles_select(app, -1);
}

/* Reload the profile list and select select_idx (e.g. the profile just
 * saved or renamed); -1 or an invalid index selects the active profile.
 * Before 1.20.2 a reload always jumped to the active profile. */
static void load_cpu_custom_profiles_select(AppData *app, int select_idx) {
    if (!app->cpu_connected) {
        g_print("[GUI] CPU not connected, skipping custom profile load\n");
        return;
    }

    const char *response = send_cpu_command(app, "LIST_CUSTOM_PROFILES");
    if (response == NULL || strncmp(response, "ERROR", 5) == 0) {
        g_print("[GUI] Failed to load custom profiles: %s\n", response ? response : "(null)");
        return;
    }

    gtk_list_store_clear(app->cpu_custom_profile_store);

    /* Parse response: lines like " [*] ProfileName (gov=schedutil, cores=24, all_cores=0)"
     * or "     ProfileName (gov=performance, cores=24, all_cores=1)"
     * Strip the metadata in parentheses, keep marker + profile name as display name. */
    char *resp_copy = g_strdup(response);
    char *line = strtok(resp_copy, "\n");
    int profile_count = 0;
    int active_idx = -1;
    int active_all_cores = 1;  /* Default: apply to all cores */
    int sel_all_cores = 1;
    while (line != NULL) {
        /* Skip empty lines */
        if (strlen(line) == 0) {
            line = strtok(NULL, "\n");
            continue;
        }

        /* Check for active marker "[*]" */
        int is_active = (strstr(line, "[*]") != NULL);

        /* Extract all_cores value from metadata before stripping */
        int line_all_cores = 0;
        char *ac_ptr = strstr(line, "all_cores=");
        if (ac_ptr) {
            line_all_cores = atoi(ac_ptr + 10);
        }

        /* Strip metadata: truncate at '(' to remove "(gov=..., cores=..., all_cores=...)" */
        char *paren = strchr(line, '(');
        if (paren) *paren = '\0';
        g_strchomp(line);  /* Remove trailing whitespace */

        GtkTreeIter iter;
        gtk_list_store_append(app->cpu_custom_profile_store, &iter);
        gtk_list_store_set(app->cpu_custom_profile_store, &iter, 0, line, -1);
        if (is_active) {
            active_idx = profile_count;
            active_all_cores = line_all_cores;
        }
        if (profile_count == select_idx) sel_all_cores = line_all_cores;
        profile_count++;

        line = strtok(NULL, "\n");
    }
    g_free(resp_copy);
    app->cpu_running_profile_idx = active_idx;   /* [*] */

    /* Select the requested profile, otherwise the active one */
    if (select_idx >= 0 && select_idx < profile_count) {
        active_idx = select_idx;          /* below: "the profile to show" */
        active_all_cores = sel_all_cores;
    }
    if (active_idx >= 0) {
        g_signal_handler_block(app->cpu_custom_profile_combo, app->cpu_custom_profile_combo_signal_id);
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo), active_idx);
        g_signal_handler_unblock(app->cpu_custom_profile_combo, app->cpu_custom_profile_combo_signal_id);
        app->cpu_active_custom_profile_idx = active_idx;

        /* Build the grid immediately so the user can edit on first visit */
        if (app->cpu_core_count > 0) {
            build_cpu_core_freq_grid(app, app->cpu_core_count);
            update_cpu_core_freq_spinners(app, active_idx);
        }

        /* Enable the action buttons so the user can edit immediately */
        gtk_widget_set_sensitive(app->cpu_freq_profile_delete_btn, TRUE);
        gtk_widget_set_sensitive(app->cpu_freq_profile_rename_btn, TRUE);
        gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, TRUE);
        gtk_widget_set_sensitive(app->cpu_freq_profile_activate_btn, TRUE);
        gtk_widget_set_sensitive(app->cpu_apply_all_cores_checkbox, TRUE);

        /* Restore the Apply to All Cores checkbox state from the active profile.
         * Block the toggled callback and manually enforce spinner state, since
         * gtk_toggle_button_set_active() doesn't fire the callback if value is unchanged. */
        g_signal_handler_block(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox), active_all_cores != 0);
        g_signal_handler_unblock(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);

        /* Manually enforce spinner disabled/enabled state */
        if (active_all_cores) {
            sync_core0_to_all_cores(app);
            for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], FALSE);
                gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], FALSE);
            }
        } else {
            for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], TRUE);
                gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], TRUE);
            }
        }
    }

    if (active_idx >= 0) cpu_take_snapshot(app);   /* the values as stored */
    g_print("[GUI] Loaded %d custom frequency profiles (active idx=%d)\n", profile_count, active_idx);

}

/* Callback: Custom profile combo box changed */
static void on_cpu_custom_profile_changed(GtkWidget *widget, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int active = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
    if (active < 0) return;

    /* Unsaved changes in the profile shown so far (the widgets still show it) */
    if (app->cpu_snap_valid && app->cpu_snap_profile != active && cpu_profile_has_unsaved_changes(app)) {
        int left = app->cpu_snap_profile;
        int r = cpu_check_unsaved_changes(app);
        g_signal_handler_block(widget, app->cpu_custom_profile_combo_signal_id);
        if (r < 0) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(widget), left);   /* stay */
            g_signal_handler_unblock(widget, app->cpu_custom_profile_combo_signal_id);
            return;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(widget), active);     /* Save reselected the old one */
        g_signal_handler_unblock(widget, app->cpu_custom_profile_combo_signal_id);
    }

    app->cpu_active_custom_profile_idx = active;
    g_print("[GUI] Selected custom profile index %d\n", active);

    /* Enable delete, rename, save, and activate buttons */
    gtk_widget_set_sensitive(app->cpu_freq_profile_delete_btn, TRUE);
    gtk_widget_set_sensitive(app->cpu_freq_profile_rename_btn, TRUE);
    gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, TRUE);
    gtk_widget_set_sensitive(app->cpu_freq_profile_activate_btn, TRUE);
    gtk_widget_set_sensitive(app->cpu_apply_all_cores_checkbox, TRUE);

    /* Build the per-core freq grid if we have core count info */
    if (app->cpu_core_count > 0) {
        build_cpu_core_freq_grid(app, app->cpu_core_count);
        update_cpu_core_freq_spinners(app, active);

        /* Query the new profile's all_cores value and restore checkbox state */
        char info_cmd[64];
        snprintf(info_cmd, sizeof(info_cmd), "GET_CUSTOM_PROFILE_INFO|%d", active);
        const char *info_response = send_cpu_command(app, info_cmd);
        int all_cores = 1; /* Default: apply to all cores */
        if (info_response) {
            const char *aac_ptr = strstr(info_response, "APPLY_ALL:");
            if (aac_ptr) {
                all_cores = atoi(aac_ptr + 10);
            }
        }
        g_print("[GUI] Profile %d all_cores=%d, setting checkbox\n", active, all_cores);

        /* Block the toggled callback to avoid double-firing during manual enforcement */
        g_signal_handler_block(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox), all_cores != 0);
        g_signal_handler_unblock(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);

        /* Manually enforce spinner disabled/enabled state */
        if (all_cores) {
            sync_core0_to_all_cores(app);
            for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], FALSE);
                gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], FALSE);
            }
        } else {
            for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], TRUE);
                gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], TRUE);
            }
        }
    } else {
        g_print("[GUI] Core count not yet available, skipping grid build\n");
    }

    cpu_take_snapshot(app);   /* the values as stored */
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Profile selected - click Activate to use it"));
}

/* Extract clean profile name from stored display string.
 * Stored format: " [*] ProfileName" or "     ProfileName"
 * Returns: pointer into display_name (not a new allocation). */
static const char *cpu_profile_name_from_display(const char *display_name) {
    const char *p = display_name;
    /* Skip leading whitespace */
    while (*p == ' ' || *p == '\t') p++;
    /* Skip "[*] " marker if present */
    if (strncmp(p, "[*] ", 4) == 0) p += 4;
    return p;
}

/* Callback: Create new frequency profile */
/* "Balanced" for a new CPU profile, as "governor[:epp]" for the daemon:
 * schedutil where the cpufreq driver offers it, otherwise (intel_pstate in
 * active mode: only performance and powersave) powersave with the EPP
 * balance_performance, like the built-in Balanced mode. reply is the
 * answer to GET_AVAILABLE_GOVERNORS ("GOVERNORS OK\nCOUNT=n\nname\n...").
 * Asking for schedutil where it does not exist made the daemon log a
 * warning for every new profile (1.22.2). */
static const char *cpu_balanced_mode(const char *reply) {
    if (reply && strncmp(reply, "GOVERNORS OK", 12) == 0) {
        for (const char *l = strchr(reply, '\n'); l; l = strchr(l, '\n')) {
            l++;
            if (strncmp(l, "schedutil", 9) == 0 && (l[9] == '\n' || l[9] == '\0'))
                return "schedutil";
        }
        return "powersave:balance_performance";
    }
    return "schedutil";   /* unknown: the daemon resolves it to an available governor */
}

static void on_cpu_create_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    if (cpu_check_unsaved_changes(app) < 0) return;
    g_print("[GUI] Create frequency profile clicked\n");

    /* Show dialog to enter profile name */
    GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Create CPU Frequency Profile"),
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                    _("_Create"), GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);

    GtkWidget *label = gtk_label_new(_("Enter profile name:"));
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(content), entry, TRUE, TRUE, 0);

    gtk_widget_show_all(content);

    /* Set Accept as default so Enter key triggers profile creation */
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const gchar *name = gtk_entry_get_text(GTK_ENTRY(entry));

        if (strlen(name) > 0) {
            /* Check for duplicate profile name */
            int duplicate = 0;
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter)) {
                do {
                    gchar *existing_name = NULL;
                    gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter, 0, &existing_name, -1);
                    if (existing_name && strcmp(cpu_profile_name_from_display(existing_name), name) == 0) {
                        duplicate = 1;
                        g_free(existing_name);
                        break;
                    }
                    g_free(existing_name);
                } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter));
            }

            if (duplicate) {
                GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                                 GTK_MESSAGE_ERROR,
                                                                 GTK_BUTTONS_OK,
                                                                 _("A CPU frequency profile with the name '%s' already exists."),
                                                                 name);
                gtk_dialog_run(GTK_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
                gtk_widget_destroy(dialog);
                return;
            }

            /* Send CREATE_CUSTOM_PROFILE command to daemon; a new profile
             * starts in the Balanced mode of this system */
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "CREATE_CUSTOM_PROFILE|%s|%s|%u|0", name,
                     cpu_balanced_mode(send_cpu_command(app, "GET_AVAILABLE_GOVERNORS")),
                     HARD_MAX_FREQ_HZ_DEFAULT);
            const char *response = send_cpu_command(app, cmd);
            gui_event("[GUI] CREATE_CUSTOM_PROFILE response: %s\n", response ? response : "NULL");

            /* Reload the profile list */
            load_cpu_custom_profiles(app);

            /* Save profiles to disk (GUI writes directly) */
            gui_save_profiles_to_disk(app);

            /* Find and select the newly created profile */
            int new_idx = -1;
            if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter)) {
                int i = 0;
                do {
                    gchar *existing_name = NULL;
                    gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter, 0, &existing_name, -1);
                    if (existing_name && strcmp(cpu_profile_name_from_display(existing_name), name) == 0) {
                        new_idx = i;
                        g_free(existing_name);
                        break;
                    }
                    g_free(existing_name);
                    i++;
                } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter));
            }

            if (new_idx >= 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo), new_idx);
                app->cpu_active_custom_profile_idx = new_idx;

                /* Default: set APPLY_ALL_CORES to 1 for the new profile */
                {
                    char apply_cmd[64];
                    snprintf(apply_cmd, sizeof(apply_cmd), "SET_APPLY_ALL_CORES|%d|1", new_idx);
                    send_cpu_command(app, apply_cmd);
                }

                /* Load the new profile data into the editor grid */
                if (app->cpu_core_count > 0) {
                    build_cpu_core_freq_grid(app, app->cpu_core_count);
                    update_cpu_core_freq_spinners(app, new_idx);

                    /* Restore checkbox state and enforce spinner disabled/enabled state */
                    char info_cmd[64];
                    snprintf(info_cmd, sizeof(info_cmd), "GET_CUSTOM_PROFILE_INFO|%d", new_idx);
                    const char *info_response = send_cpu_command(app, info_cmd);
                    int all_cores = 1; /* Default: apply to all cores */
                    if (info_response) {
                        const char *aac_ptr = strstr(info_response, "APPLY_ALL:");
                        if (aac_ptr) {
                            all_cores = atoi(aac_ptr + 10);
                        }
                    }

                    g_signal_handler_block(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox), all_cores != 0);
                    g_signal_handler_unblock(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);

                    if (all_cores) {
                        sync_core0_to_all_cores(app);
                        for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                            gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], FALSE);
                            gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], FALSE);
                        }
                    } else {
                        for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                            gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], TRUE);
                            gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], TRUE);
                        }
                    }
                }
            }

            gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label),
                              g_strconcat(_("Created profile: "), name, NULL));
        }
    }

    gtk_widget_destroy(dialog);
}

/* Callback: Delete selected frequency profile */
static void on_cpu_delete_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Delete frequency profile clicked\n");

    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo));
    if (idx < 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("No profile selected"));
        return;
    }

    /* Get profile name for confirmation */
    gchar *profile_name = NULL;
    GtkTreeIter iter;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->cpu_custom_profile_combo), &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter, 0, &profile_name, -1);
    }

    if (profile_name == NULL) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Could not read profile name"));
        return;
    }

    /* Show confirmation dialog */
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_QUESTION,
                                               GTK_BUTTONS_YES_NO,
                                               _("Are you sure you want to delete the CPU frequency profile '%s'?"),
                                               profile_name);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES) {
        /* Send DELETE_CUSTOM_PROFILE command */
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "DELETE_CUSTOM_PROFILE|%d", idx);
        const char *response = send_cpu_command(app, cmd);
        gui_event("[GUI] DELETE_CUSTOM_PROFILE response: %s\n", response ? response : "NULL");

        /* Reload the profile list */
        load_cpu_custom_profiles(app);

        /* Save profiles to disk (GUI writes directly) */
        gui_save_profiles_to_disk(app);

        /* Defer grid rebuild until the next main loop iteration so the widget
         * hierarchy is stable after the profile list reload. */
        if (app->cpu_core_count > 0 && app->cpu_active_custom_profile_idx >= 0) {
            g_idle_add(populate_cpu_core_grid_idle, app);
        } else {
            /* No profiles left, clear the grid */
            GList *children = gtk_container_get_children(GTK_CONTAINER(app->cpu_core_freq_grid));
            GList *child = children;
            while (child != NULL) {
                gtk_widget_destroy(GTK_WIDGET(child->data));
                child = child->next;
            }
            g_list_free(children);
        }

        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label),
                          g_strconcat(_("Deleted profile: "), profile_name, NULL));
    }

    g_free(profile_name);
    gtk_widget_destroy(dialog);
}

/* Callback: Rename selected frequency profile */
static void on_cpu_rename_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Rename frequency profile clicked\n");

    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo));
    if (idx < 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("No profile selected"));
        return;
    }

    /* Get current profile name */
    gchar *current_name = NULL;
    GtkTreeIter iter;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->cpu_custom_profile_combo), &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter, 0, &current_name, -1);
    }

    if (current_name == NULL) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Could not read profile name"));
        return;
    }

    /* Extract clean profile name from display string */
    const char *clean_name = cpu_profile_name_from_display(current_name);

    /* Show dialog to enter new name */
    GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Rename CPU Frequency Profile"),
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                    _("_Rename"), GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);

    GtkWidget *label = gtk_label_new(_("Enter new profile name:"));
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), clean_name);
    gtk_box_pack_start(GTK_BOX(content), entry, TRUE, TRUE, 0);

    gtk_widget_show_all(content);

    /* Set Accept as default so Enter key triggers rename */
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const gchar *new_name = gtk_entry_get_text(GTK_ENTRY(entry));

        if (strlen(new_name) > 0) {
            /* Send RENAME_CUSTOM_PROFILE command */
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "RENAME_CUSTOM_PROFILE|%d|%s", idx, new_name);
            const char *response = send_cpu_command(app, cmd);
            gui_event("[GUI] RENAME_CUSTOM_PROFILE response: %s\n", response ? response : "NULL");

            /* Reload the profile list, staying on the renamed profile */
            load_cpu_custom_profiles_select(app, idx);

            /* Keep selection on the renamed profile */
            g_signal_handler_block(app->cpu_custom_profile_combo, app->cpu_custom_profile_combo_signal_id);
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo), idx);
            g_signal_handler_unblock(app->cpu_custom_profile_combo, app->cpu_custom_profile_combo_signal_id);

            /* Save profiles to disk (GUI writes directly) */
            gui_save_profiles_to_disk(app);

            gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label),
                              g_strconcat(_("Renamed profile to: "), new_name, NULL));
        } else {
            gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Name cannot be empty"));
        }
    }

    g_free(current_name);
    gtk_widget_destroy(dialog);
}

/* Callback: Save current profile values to disk */
/* Context struct for deferred save work */
typedef struct {
    AppData *app;
    int profile_idx;
    int num_cores;
    int apply_all_cores;
    char governor[64];
    char pp[32];                  /* firmware power profile, "" = unchanged (1.43.0) */
    int pp_offered;
    double core_min_mhz[MAX_CPUS];
    double core_max_mhz[MAX_CPUS];
} CpuSaveContext;

/* Idle callback that performs the actual save work.
 * Decoupled from the button clicked signal so GTK can release
 * the button's pressed visual state before we destroy/recreate widgets.
 * Uses a single batch command (SAVE_CUSTOM_PROFILE) instead of ~50 round-trips. */
static gboolean cpu_save_freq_profile_idle(gpointer user_data) {
    CpuSaveContext *ctx = (CpuSaveContext *)user_data;
    AppData *app = ctx->app;
    int idx = ctx->profile_idx;
    int num_cores = ctx->num_cores;

    /* Build core data: comma-separated min0,max0,min1,max1,... */
    char core_data[MAX_CPUS * 48] = {0};   /* "min,max," per core, 64-bit Hz */
    size_t pos = 0;
    for (int i = 0; i < num_cores && i < MAX_CPUS; i++) {
        gulong min_hz = (gulong)(ctx->core_min_mhz[i] * 1000000);
        gulong max_hz = (gulong)(ctx->core_max_mhz[i] * 1000000);
        if (i > 0) {
            pos += snprintf(core_data + pos, sizeof(core_data) - pos, ",");
        }
        pos += snprintf(core_data + pos, sizeof(core_data) - pos, "%lu,%lu", min_hz, max_hz);
    }

    /* Send single batch command */
    char batch_cmd[MAX_CPUS * 48 + 256];
    snprintf(batch_cmd, sizeof(batch_cmd),
        "SAVE_CUSTOM_PROFILE|%d|%s|%d|%s",
        idx, ctx->governor, ctx->apply_all_cores, core_data);

    LOG_INFO("Sending batch save for profile %d", idx);
    const char *resp = send_cpu_command(app, batch_cmd);
    LOG_INFO("SAVE_CUSTOM_PROFILE response: %s", resp ? resp : "(NULL)");
    if (ctx->pp_offered) {
        char pp_cmd[96];
        snprintf(pp_cmd, sizeof pp_cmd, "SET_CUSTOM_PROFILE_PLATFORM|%d|%s", idx, ctx->pp[0] ? ctx->pp : "-");
        resp = send_cpu_command(app, pp_cmd);
        LOG_INFO("SET_CUSTOM_PROFILE_PLATFORM response: %s", resp ? resp : "(NULL)");
    }

    /* Mark profiles modified and save to disk */
    mark_cpu_freq_profiles_modified(app);
    gui_save_profiles_to_disk(app);
    LOG_INFO("Profiles saved to disk");

    /* Reload profile list, staying on the profile just saved */
    load_cpu_custom_profiles_select(app, idx);
    LOG_INFO("Profile list reloaded");

    char status_msg[128];
    snprintf(status_msg, sizeof(status_msg), _("Saved profile"));
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), status_msg);

    /* Re-enable the save button */
    gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, TRUE);
    LOG_INFO("<<< Save frequency profile completed");

    g_free(ctx);
    return G_SOURCE_REMOVE;
}

/* Capture the shown profile values (widgets) for SAVE_CUSTOM_PROFILE */
static CpuSaveContext *cpu_save_ctx_from_ui(AppData *app, int idx) {
    CpuSaveContext *ctx = g_new0(CpuSaveContext, 1);
    ctx->app = app;
    ctx->profile_idx = idx;
    ctx->num_cores = app->cpu_core_count;
    ctx->apply_all_cores = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)) ? 1 : 0;

    GtkTreeIter gov_iter;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->cpu_profile_combo), &gov_iter)) {
        gchar *gov_name = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_profile_store), &gov_iter, 1, &gov_name, -1);
        snprintf(ctx->governor, sizeof(ctx->governor), "%s", gov_name ? gov_name : "");
        g_free(gov_name);
    }
    for (int i = 0; i < ctx->num_cores && i < MAX_CPUS; i++) {
        ctx->core_min_mhz[i] = gtk_adjustment_get_value(app->cpu_core_min_adjustments[i]);
        ctx->core_max_mhz[i] = gtk_adjustment_get_value(app->cpu_core_max_adjustments[i]);
    }
    ctx->pp_offered = app->cpu_pp_choices[0] != '\0';
    g_strlcpy(ctx->pp, cpu_pp_selected(app), sizeof ctx->pp);
    return ctx;
}

/* --- Unsaved CPU profile changes (1.23.0) ---
 * A snapshot of the shown profile is taken whenever it is loaded or saved;
 * "unsaved" means the widgets differ from it. Values the GUI sets itself
 * (loading, syncing core 0 to all cores) are part of the snapshot, so only
 * the user's edits count. */
static void cpu_take_snapshot(AppData *app) {
    app->cpu_snap_valid = 0;
    int idx = app->cpu_custom_profile_combo
            ? gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo)) : -1;
    int n = app->cpu_core_count < MAX_CPUS ? app->cpu_core_count : MAX_CPUS;
    if (idx < 0 || n <= 0) return;
    for (int i = 0; i < n; i++)
        if (!app->cpu_core_min_adjustments[i] || !app->cpu_core_max_adjustments[i]) return;
    app->cpu_snap_profile = idx;
    app->cpu_snap_mode_row = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_profile_combo));
    app->cpu_snap_all_cores = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)) ? 1 : 0;
    g_strlcpy(app->cpu_snap_pp, cpu_pp_selected(app), sizeof app->cpu_snap_pp);
    app->cpu_snap_cores = n;
    for (int i = 0; i < n; i++) {
        app->cpu_snap_min[i] = gtk_adjustment_get_value(app->cpu_core_min_adjustments[i]);
        app->cpu_snap_max[i] = gtk_adjustment_get_value(app->cpu_core_max_adjustments[i]);
    }
    app->cpu_snap_valid = 1;
}

static int cpu_profile_has_unsaved_changes(const AppData *app) {
    if (!app->cpu_snap_valid || app->cpu_core_count != app->cpu_snap_cores) return 0;
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_profile_combo)) != app->cpu_snap_mode_row) return 1;
    if (strcmp(cpu_pp_selected(app), app->cpu_snap_pp)) return 1;
    if ((gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)) ? 1 : 0)
        != app->cpu_snap_all_cores) return 1;
    for (int i = 0; i < app->cpu_snap_cores; i++) {
        if (!app->cpu_core_min_adjustments[i] || !app->cpu_core_max_adjustments[i]) return 0;
        if (gtk_adjustment_get_value(app->cpu_core_min_adjustments[i]) != app->cpu_snap_min[i] ||
            gtk_adjustment_get_value(app->cpu_core_max_adjustments[i]) != app->cpu_snap_max[i]) return 1;
    }
    return 0;
}

/* Like the fan curve editor: Cancel / Discard Changes / Save Changes.
 * Returns 1 when there was nothing to save or it was saved, 0 when the
 * changes were discarded (widgets back to the stored values), -1 when the
 * user cancelled (the caller stops). */
static int cpu_check_unsaved_changes(AppData *app) {
    if (!cpu_profile_has_unsaved_changes(app)) return 1;

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, NULL);
    gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(dialog),
        _("<b>Warning:</b> You have unsaved changes to the CPU profile.\n\nWhat would you like to do?"));
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Cancel"), -102);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Discard Changes"), -100);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Save Changes"), -101);
    gtk_widget_show_all(dialog);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    int idx = app->cpu_snap_profile;
    if (response == -101) {
        gui_event("[GUI] Unsaved CPU profile changes: saving profile %d\n", idx);
        cpu_save_freq_profile_idle(cpu_save_ctx_from_ui(app, idx));   /* now, not deferred */
        return 1;
    }
    if (response == -100) {
        gui_event("[GUI] Unsaved CPU profile changes: discarded (profile %d)\n", idx);
        /* A power mode change on the running profile was applied right
         * away: put the stored mode back */
        int row = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_profile_combo));
        if (row != app->cpu_snap_mode_row && idx == app->cpu_running_profile_idx) {
            GtkTreeIter it;
            if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(app->cpu_profile_store), &it, NULL,
                                              app->cpu_snap_mode_row)) {
                gchar *mode = NULL;
                gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_profile_store), &it, 0, &mode, -1);
                if (mode) {
                    char cmd[256];
                    snprintf(cmd, sizeof(cmd), "SET_POWER_MODE|%s", mode);
                    send_cpu_command(app, cmd);
                    g_free(mode);
                }
            }
        }
        load_cpu_custom_profiles_select(app, idx);   /* widgets back to the stored values */
        return 0;
    }
    return -1;
}

static void on_cpu_save_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    LOG_INFO(">>> Save frequency profile clicked");

    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo));
    if (idx < 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("No profile selected"));
        LOG_INFO("<<< Save aborted: no profile selected");
        return;
    }

    int num_cores = app->cpu_core_count;
    if (num_cores <= 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Core count not available"));
        LOG_INFO("<<< Save aborted: core count not available");
        return;
    }

    CpuSaveContext *ctx = cpu_save_ctx_from_ui(app, idx);

    /* Immediately disable the save button to clear its pressed visual state,
     * then re-enable it after the save completes in the idle callback. */
    gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Saving..."));

    /* Defer the heavy save work so GTK can process the button state change */
    g_idle_add((GSourceFunc)cpu_save_freq_profile_idle, ctx);
    LOG_INFO("Save deferred to idle callback, button disabled");
}

/* Idle callback struct for activate profile */
typedef struct {
    AppData *app;
    int profile_idx;
} ActivateIdleData;

static gboolean activate_profile_idle_callback(gpointer user_data) {
    ActivateIdleData *data = (ActivateIdleData *)user_data;
    AppData *app = data->app;
    int idx = data->profile_idx;
    g_free(data);

    /* Apply the profile */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "APPLY_CUSTOM_PROFILE|%d", idx);
    const char *response = send_cpu_command(app, cmd);
    gui_event("[GUI] APPLY_CUSTOM_PROFILE response: %s\n", response ? response : "NULL");

    /* Reload profile list so the [*] marker updates to the newly active profile */
    load_cpu_custom_profiles(app);

    /* Persist the active_profile_index to profiles.json */
    gui_save_profiles_to_disk(app);

    char status_msg[128];
    snprintf(status_msg, sizeof(status_msg), _("Activated profile"));
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), status_msg);

    return FALSE; /* One-shot */
}

/* Callback: Activate (apply) the selected frequency profile to the CPU */
static void on_cpu_activate_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Activate frequency profile clicked\n");
    if (cpu_check_unsaved_changes(app) < 0) return;

    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo));
    if (idx < 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("No profile selected"));
        return;
    }

    /* Defer the apply + reload + save to an idle callback so GTK can finish
     * processing the button click before we destroy/rebuild the grid widgets.
     * This prevents race conditions with pending signal handlers. */
    ActivateIdleData *data = g_new0(ActivateIdleData, 1);
    data->app = app;
    data->profile_idx = idx;
    g_idle_add(activate_profile_idle_callback, data);
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Activating..."));
}

/* Callback: Apply hard frequency limit */
static void on_cpu_hard_limit_apply_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;

    double ghz = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->cpu_hard_limit_spinner));
    guint64 hz = (guint64)(ghz * 1000000000.0 + 0.5);

    g_print("[GUI] Applying hard limit: %.2f GHz (%" G_GUINT64_FORMAT " Hz)\n", ghz, hz);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "SET_HARD_LIMIT|%" G_GUINT64_FORMAT, hz);
    const char *response = send_cpu_command(app, cmd);
    gui_event("[GUI] SET_HARD_LIMIT response: %s\n", response ? response : "NULL");

    if (response && strncmp(response, "OK", 2) == 0) {
        char status_msg[256];
        snprintf(status_msg, sizeof(status_msg), _("Hard limit applied: %.2f GHz"), ghz);
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), status_msg);

        /* Update core freq spinner upper bounds to match new hard limit */
        int max_mhz = (int)(hz / 1000000);
        for (int i = 0; i < app->cpu_core_count && i < 32; i++) {
            if (app->cpu_core_min_adjustments[i]) {
                gtk_adjustment_set_upper(GTK_ADJUSTMENT(app->cpu_core_min_adjustments[i]), max_mhz);
            }
            if (app->cpu_core_max_adjustments[i]) {
                gtk_adjustment_set_upper(GTK_ADJUSTMENT(app->cpu_core_max_adjustments[i]), max_mhz);
            }
        }
    } else {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label),
            response ? response : _("Failed to apply hard limit"));
    }
}

/* Helper: copy core 0 min/max to all other cores when checkbox is active */
static void sync_core0_to_all_cores(AppData *app) {
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)))
        return;
    if (app->cpu_core_count <= 0) return;

    double first_min = gtk_adjustment_get_value(app->cpu_core_min_adjustments[0]);
    double first_max = gtk_adjustment_get_value(app->cpu_core_max_adjustments[0]);

    for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
        gtk_adjustment_set_value(app->cpu_core_min_adjustments[i], first_min);
        gtk_adjustment_set_value(app->cpu_core_max_adjustments[i], first_max);
    }
}

/* Sync core 0 values to all cores by reading Entry text directly (for live sync during typing)
 * Called from idle callback, so GTK has processed the text */
static void sync_core0_live_to_all_cores(AppData *app) {
    /* Guard: check if widgets still exist (grid may have been rebuilt) */
    if (!GTK_IS_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox))
        return;
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)))
        return;
    if (app->cpu_core_count <= 0) return;
    if (!GTK_IS_SPIN_BUTTON(app->cpu_core_min_spinners[0]))
        return;
    if (!GTK_IS_SPIN_BUTTON(app->cpu_core_max_spinners[0]))
        return;

    /* Read text directly from core 0 spinners (GtkSpinButton IS a GtkEntry in GTK3) */
    const gchar *min_text = gtk_entry_get_text(GTK_ENTRY(app->cpu_core_min_spinners[0]));
    const gchar *max_text = gtk_entry_get_text(GTK_ENTRY(app->cpu_core_max_spinners[0]));
    if (!min_text || !max_text)
        return;

    /* Skip empty text */
    if (min_text[0] == '\0' || max_text[0] == '\0')
        return;

    /* Parse - skip non-numeric */
    char *end1 = NULL, *end2 = NULL;
    double first_min = strtod(min_text, &end1);
    double first_max = strtod(max_text, &end2);
    if (end1 == min_text || end2 == max_text)
        return;

    for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
        if (!GTK_IS_SPIN_BUTTON(app->cpu_core_min_spinners[i]))
            return;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->cpu_core_min_spinners[i]), first_min);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->cpu_core_max_spinners[i]), first_max);
    }
}

/* Idle callback: defer sync until GTK has finished processing the text */
typedef struct {
    AppData *app;
} SyncIdleData;

static gboolean sync_core0_idle_callback(gpointer user_data) {
    SyncIdleData *data = (SyncIdleData *)user_data;
    sync_core0_live_to_all_cores(data->app);
    g_free(data);
    return FALSE; /* One-shot */
}

/* Callback: core 0 Entry changed signal - fires on every keystroke */
static void on_core0_entry_changed(GtkEntry *entry, gpointer user_data) {
    (void)entry;
    AppData *app = (AppData *)user_data;

    /* Defer the sync so GTK finishes processing first */
    SyncIdleData *data = g_new0(SyncIdleData, 1);
    data->app = app;
    g_idle_add(sync_core0_idle_callback, data);
}

/* Callback: Apply to All Cores checkbox toggled */
static void on_cpu_apply_all_cores_toggled(GtkCheckButton *checkbox, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checkbox));
    g_print("[GUI] Apply to All Cores: %s\n", active ? "ON" : "OFF");

    if (app->cpu_core_count <= 0) return;

    if (active) {
        /* Copy first core's min/max values to all cores and disable other spinners */
        sync_core0_to_all_cores(app);

        for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
            gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], FALSE);
            gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], FALSE);
        }

        g_print("[GUI] Copied core 0 values (min=%.0f MHz, max=%.0f MHz) to all %d cores\n",
                gtk_adjustment_get_value(app->cpu_core_min_adjustments[0]),
                gtk_adjustment_get_value(app->cpu_core_max_adjustments[0]),
                app->cpu_core_count);
    } else {
        /* Re-enable all spinners */
        for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
            gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], TRUE);
            gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], TRUE);
        }
    }
}

/* Build the per-core frequency grid with spinners */
static void build_cpu_core_freq_grid(AppData *app, int num_cores) {
    g_print("[GUI] build_cpu_core_freq_grid called with %d cores\n", num_cores);

    /* Clear existing grid contents */
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->cpu_core_freq_grid));
    GList *child = children;
    while (child != NULL) {
        gtk_widget_destroy(GTK_WIDGET(child->data));
        child = child->next;
    }
    g_list_free(children);

    /* Grid columns: Core Label | Min Freq (MHz) | Max Freq (MHz) */
    int col_label = 0;
    int col_min = 1;
    int col_max = 2;

    for (int i = 0; i < num_cores && i < MAX_CPUS; i++) {
        int row = i;

        /* Core label */
        char core_name[32];
        snprintf(core_name, sizeof(core_name), _("Core %d%s"), i, cpu_core_tag(i));
        GtkWidget *core_label = gtk_label_new(core_name);
        gtk_widget_set_halign(core_label, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(app->cpu_core_freq_grid), core_label, col_label, row, 1, 1);
        gtk_widget_set_margin_end(core_label, 5);

        /* Min freq spinner (800-3600 MHz, step 100) */
        app->cpu_core_min_adjustments[i] = gtk_adjustment_new(800, 800, 3600, 100, 100, 0);
        app->cpu_core_min_spinners[i] = gtk_spin_button_new(GTK_ADJUSTMENT(app->cpu_core_min_adjustments[i]), 100, 0);
        gtk_widget_set_hexpand(app->cpu_core_min_spinners[i], TRUE);
        gtk_grid_attach(GTK_GRID(app->cpu_core_freq_grid), app->cpu_core_min_spinners[i], col_min, row, 1, 1);
        gtk_widget_set_margin_end(app->cpu_core_min_spinners[i], 5);

        /* Max freq spinner (800-3600 MHz, step 100) */
        app->cpu_core_max_adjustments[i] = gtk_adjustment_new(3600, 800, 3600, 100, 100, 0);
        app->cpu_core_max_spinners[i] = gtk_spin_button_new(GTK_ADJUSTMENT(app->cpu_core_max_adjustments[i]), 100, 0);
        gtk_widget_set_hexpand(app->cpu_core_max_spinners[i], TRUE);
        gtk_grid_attach(GTK_GRID(app->cpu_core_freq_grid), app->cpu_core_max_spinners[i], col_max, row, 1, 1);
    }

    /* Update spinner upper bounds to match the current hard limit */
    {
        const char *hard_resp = send_cpu_command(app, "GET_HARD_LIMIT");
        if (hard_resp && strncmp(hard_resp, "OK", 2) == 0) {
            const char *eq = strstr(hard_resp, "HARD_LIMIT=");
            if (eq) {
                guint64 hz = g_ascii_strtoull(eq + 11, NULL, 10);
                int max_mhz = (int)(hz / 1000000);
                for (int i = 0; i < num_cores; i++) {
                    gtk_adjustment_set_upper(GTK_ADJUSTMENT(app->cpu_core_min_adjustments[i]), max_mhz);
                    gtk_adjustment_set_upper(GTK_ADJUSTMENT(app->cpu_core_max_adjustments[i]), max_mhz);
                }
                g_print("[GUI] Updated core freq spinner upper bounds to %d MHz\n", max_mhz);
            }
        }
    }

    /* Connect core 0 spinners' changed signal to sync all cores when Apply to All is active
     * The changed signal fires on every keystroke for real-time sync */
    {
        g_signal_connect(app->cpu_core_min_spinners[0], "changed", G_CALLBACK(on_core0_entry_changed), app);
        g_signal_connect(app->cpu_core_max_spinners[0], "changed", G_CALLBACK(on_core0_entry_changed), app);
    }

    /* Add header labels */
    GtkWidget *min_header = gtk_label_new(_("Min (MHz)"));
    gtk_widget_set_halign(min_header, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(min_header, 2);
    gtk_grid_attach(GTK_GRID(app->cpu_core_freq_grid), min_header, col_min, num_cores, 1, 1);

    GtkWidget *max_header = gtk_label_new(_("Max (MHz)"));
    gtk_widget_set_halign(max_header, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(max_header, 2);
    gtk_grid_attach(GTK_GRID(app->cpu_core_freq_grid), max_header, col_max, num_cores, 1, 1);

    gtk_widget_show_all(app->cpu_core_freq_grid);
}

/* Update spinner values from profile data */
static void update_cpu_core_freq_spinners(AppData *app, int profile_idx) {
    g_print("[GUI] update_cpu_core_freq_spinners called for profile %d\n", profile_idx);

    if (!app->cpu_connected || app->cpu_socket_fd < 0) {
        g_print("[GUI] CPU daemon not connected, cannot update spinners\n");
        return;
    }

    /* Send GET_CUSTOM_PROFILE_INFO command */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "GET_CUSTOM_PROFILE_INFO|%d", profile_idx);
    const char *response = send_cpu_command(app, cmd);

    if (response == NULL || strstr(response, "ERROR")) {
        g_print("[GUI] Failed to get custom profile info: %s\n", response ? response : "NULL");
        return;
    }

    /* Parse response: lines like CORE_0_MAX:3600000000 and CORE_0_MIN:800000000 */
    char profile_gov[32] = "", profile_epp[32] = "";
    char response_copy[BUFFER_SIZE];
    strncpy(response_copy, response, sizeof(response_copy) - 1);
    response_copy[sizeof(response_copy) - 1] = '\0';

    char *line = strtok(response_copy, "\n");
    while (line != NULL) {
        /* Trim leading/trailing whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        int core_id = -1;
        unsigned long long freq_hz = 0;

        if (strncmp(p, "GOVERNOR:", 9) == 0) {
            snprintf(profile_gov, sizeof profile_gov, "%s", p + 9);
        } else if (strncmp(p, "EPP:", 4) == 0) {
            snprintf(profile_epp, sizeof profile_epp, "%s", p + 4);
        } else if (strncmp(p, "PLATFORM_PROFILE:", 17) == 0) {
            if (app->cpu_pp_combo && !gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->cpu_pp_combo), p + 17))
                gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->cpu_pp_combo), "");
        } else if (sscanf(p, "CORE_%d_MAX:%llu", &core_id, &freq_hz) == 2) {
            if (core_id >= 0 && core_id < MAX_CPUS && app->cpu_core_max_adjustments[core_id] != NULL) {
                /* Convert Hz to MHz for spinner */
                double freq_mhz = (double)freq_hz / 1000000.0;
                gtk_adjustment_set_value(GTK_ADJUSTMENT(app->cpu_core_max_adjustments[core_id]), freq_mhz);
            }
        } else if (sscanf(p, "CORE_%d_MIN:%llu", &core_id, &freq_hz) == 2) {
            if (core_id >= 0 && core_id < MAX_CPUS && app->cpu_core_min_adjustments[core_id] != NULL) {
                double freq_mhz = (double)freq_hz / 1000000.0;
                gtk_adjustment_set_value(GTK_ADJUSTMENT(app->cpu_core_min_adjustments[core_id]), freq_mhz);
            }
        }

        line = strtok(NULL, "\n");
    }

    /* Show the profile's power mode (without triggering the change handler) */
    int mode_row = gui_find_power_mode_row(app->cpu_profile_store, profile_gov, profile_epp);
    if (mode_row >= 0 && app->cpu_profile_combo) {
        g_signal_handler_block(app->cpu_profile_combo, app->cpu_profile_combo_signal_id);
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->cpu_profile_combo), mode_row);
        g_signal_handler_unblock(app->cpu_profile_combo, app->cpu_profile_combo_signal_id);
    }
    g_print("[GUI] Profile %d power mode: governor=%s epp=%s -> row %d\n",
            profile_idx, profile_gov, profile_epp[0] ? profile_epp : "-", mode_row);

    g_print("[GUI] Spinner values updated from profile %d\n", profile_idx);
}

/* Mark that frequency profiles have been modified */
static void mark_cpu_freq_profiles_modified(AppData *app) {
    app->cpu_freq_profiles_modified = 1;
}

/* Create Page 6: CPU Control */
static GtkWidget *create_cpu_page(AppData *app) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    /* 12 px at the top like the Keyboard tab, so the daemon status lines of
     * all tabs are at the same height; 20 px elsewhere as before */
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 20);
    gtk_widget_set_margin_start(vbox, 20);
    gtk_widget_set_margin_end(vbox, 20);

    /* Daemon status (like the Keyboard tab) */
    {
        GtkWidget *frame = gtk_frame_new(_("CPU Daemon Status"));
        gtk_container_set_border_width(GTK_CONTAINER(frame), 6);
        app->cpu_daemon_status_label = gtk_label_new(_("CPU daemon not connected"));
        gtk_label_set_xalign(GTK_LABEL(app->cpu_daemon_status_label), 0.0);
        gtk_widget_set_margin_start(app->cpu_daemon_status_label, 6);
        gtk_widget_set_margin_end(app->cpu_daemon_status_label, 6);
        gtk_container_add(GTK_CONTAINER(frame), app->cpu_daemon_status_label);
        gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    }

    /* Section: CPU Info */
    GtkWidget *info_frame = gtk_frame_new(_("CPU Information"));
    GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(info_box), 10);
    gtk_container_add(GTK_CONTAINER(info_frame), info_box);
    gtk_box_pack_start(GTK_BOX(vbox), info_frame, FALSE, FALSE, 0);
    /* Two columns (1.24.4): temperature, load, CPUs left; frequencies and
     * power mode right - half the height of one long list */
    GtkWidget *info_cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_box_set_homogeneous(GTK_BOX(info_cols), TRUE);
    GtkWidget *info_left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *info_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_pack_start(GTK_BOX(info_cols), info_left, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(info_cols), info_right, TRUE, TRUE, 0);
    {   /* Processor (1.38.0): name and, on hybrid CPUs, P- and E-cores */
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("Processor:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        char t[160];
        cpu_ident_text(cpu_ident(), t, sizeof t);
        GtkWidget *val = gtk_label_new(t);
        gtk_widget_set_halign(val, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), val, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_box), row, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(info_box), info_cols, FALSE, FALSE, 0);

    /* CPU Temperature */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("CPU Temperature:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_temp_label = gtk_label_new(_("N/A"));
        gtk_widget_set_halign(app->cpu_page_temp_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_temp_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_left), row, FALSE, FALSE, 0);
    }

    /* CPU Load */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("CPU Load:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_load_label = gtk_label_new(_("N/A"));
        gtk_widget_set_halign(app->cpu_page_load_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_load_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_left), row, FALSE, FALSE, 0);
    }

    /* Number of CPUs */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("Number of CPUs:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_num_label = gtk_label_new(_("N/A"));
        gtk_widget_set_halign(app->cpu_page_num_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_num_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_left), row, FALSE, FALSE, 0);
    }

    /* Current Frequency */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("Current Frequency:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_freq_label = gtk_label_new(_("N/A"));
        gtk_widget_set_halign(app->cpu_page_freq_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_freq_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_right), row, FALSE, FALSE, 0);
    }

    /* Min/Max Frequency */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("Min / Max Frequency:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_min_max_freq_label = gtk_label_new(_("N/A"));
        gtk_widget_set_halign(app->cpu_page_min_max_freq_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_min_max_freq_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_right), row, FALSE, FALSE, 0);
    }

    /* Active Governor */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("Active Power Mode:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_governor_label = gtk_label_new(_("N/A"));
        gtk_widget_set_halign(app->cpu_page_governor_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_governor_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_right), row, FALSE, FALSE, 0);
    }

    /* Section: Governor Profiles */
    GtkWidget *profile_frame = gtk_frame_new(_("Governor Profiles"));
    GtkWidget *profile_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(profile_box), 10);
    gtk_container_add(GTK_CONTAINER(profile_frame), profile_box);
    gtk_box_pack_start(GTK_BOX(vbox), profile_frame, FALSE, FALSE, 0);

    /* Profile combo box */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("Power mode:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);

        app->cpu_profile_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
        app->cpu_profile_combo = gtk_combo_box_new();
        gtk_combo_box_set_model(GTK_COMBO_BOX(app->cpu_profile_combo), GTK_TREE_MODEL(app->cpu_profile_store));
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(app->cpu_profile_combo), renderer, TRUE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(app->cpu_profile_combo), renderer, "text", 0, NULL);
        gtk_widget_set_hexpand(app->cpu_profile_combo, TRUE);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_profile_combo, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(profile_box), row, FALSE, FALSE, 0);

        /* Connect combo box changed signal and store handler ID */
        app->cpu_profile_combo_signal_id = g_signal_connect(app->cpu_profile_combo, "changed", G_CALLBACK(on_cpu_governor_changed), app);
    }

    /* Firmware power profile (1.43.0): only on laptops that offer one */
    {
        app->cpu_pp_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("Firmware power profile:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(app->cpu_pp_row), label, FALSE, FALSE, 0);
        app->cpu_pp_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->cpu_pp_combo), "", _("Unchanged"));
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->cpu_pp_combo), "");
        gtk_widget_set_tooltip_text(app->cpu_pp_combo,
            _("Set by the laptop's firmware (platform_profile) when the frequency profile is applied; "
              "Unchanged leaves it as it is. Saved with the frequency profile."));
        gtk_widget_set_hexpand(app->cpu_pp_combo, TRUE);
        gtk_box_pack_start(GTK_BOX(app->cpu_pp_row), app->cpu_pp_combo, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(profile_box), app->cpu_pp_row, FALSE, FALSE, 0);
        gtk_widget_set_no_show_all(app->cpu_pp_row, TRUE);
    }

    /* Section: Per-Core Frequency Profiles */
    {
        GtkWidget *freq_frame = gtk_frame_new(_("Per-Core Frequency Profiles"));
        GtkWidget *freq_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_container_set_border_width(GTK_CONTAINER(freq_box), 10);
        gtk_container_add(GTK_CONTAINER(freq_frame), freq_box);
        gtk_box_pack_start(GTK_BOX(vbox), freq_frame, FALSE, FALSE, 0);

        /* Profile combo box */
        {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            GtkWidget *label = gtk_label_new(_("Frequency Profile:"));
            gtk_widget_set_halign(label, GTK_ALIGN_END);
            gtk_label_set_width_chars(GTK_LABEL(label), 20);
            gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);

            app->cpu_custom_profile_store = gtk_list_store_new(1, G_TYPE_STRING);
            app->cpu_custom_profile_combo = gtk_combo_box_new();
            gtk_combo_box_set_model(GTK_COMBO_BOX(app->cpu_custom_profile_combo), GTK_TREE_MODEL(app->cpu_custom_profile_store));
            GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
            gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(app->cpu_custom_profile_combo), renderer, TRUE);
            gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(app->cpu_custom_profile_combo), renderer, "text", 0, NULL);
            gtk_widget_set_hexpand(app->cpu_custom_profile_combo, TRUE);
            gtk_box_pack_start(GTK_BOX(row), app->cpu_custom_profile_combo, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(freq_box), row, FALSE, FALSE, 0);

            app->cpu_custom_profile_combo_signal_id = g_signal_connect(app->cpu_custom_profile_combo, "changed", G_CALLBACK(on_cpu_custom_profile_changed), app);
        }

        /* Apply to All Cores checkbox */
        {
            app->cpu_apply_all_cores_checkbox = gtk_check_button_new_with_label(_("Apply values to all cores"));
            gtk_box_pack_start(GTK_BOX(freq_box), app->cpu_apply_all_cores_checkbox, FALSE, FALSE, 0);
            gtk_widget_set_sensitive(app->cpu_apply_all_cores_checkbox, FALSE); /* Disabled until profile selected */
            app->cpu_apply_all_cores_signal_id = g_signal_connect(app->cpu_apply_all_cores_checkbox, "toggled", G_CALLBACK(on_cpu_apply_all_cores_toggled), app);
        }

        /* Note on low maximums (1.34.2): measured on a Core Ultra 9 275HX, the
         * CPU kept 2.2 GHz (P-cores) / 1.5 GHz (E-cores) with work to do
         * however low scaling_max_freq was; higher maximums held exactly. */
        {
            GtkWidget *note = gtk_label_new(NULL);
            gchar *m = g_markup_printf_escaped("<small>%s</small>", _(CPU_MAX_FLOOR_NOTE));
            gtk_label_set_markup(GTK_LABEL(note), m);
            g_free(m);
            gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
            gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
            gtk_label_set_max_width_chars(GTK_LABEL(note), 110);
            gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
            gtk_box_pack_start(GTK_BOX(freq_box), note, FALSE, FALSE, 0);
        }

        /* Scrollable area for per-core min/max spinners */
        {
            GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
            gtk_container_set_border_width(GTK_CONTAINER(scroll), 5);
            gtk_widget_set_size_request(scroll, -1, 200); /* Fixed height for the scroll area */
            gtk_box_pack_start(GTK_BOX(freq_box), scroll, FALSE, FALSE, 0);

            app->cpu_core_freq_grid = gtk_grid_new();
            gtk_container_add(GTK_CONTAINER(scroll), app->cpu_core_freq_grid);
            /* Grid will be populated when a profile is selected */
        }

        /* Create, Rename, Save, Delete, Activate (same order in every tab) */
        {
            GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_box_pack_start(GTK_BOX(freq_box), btn_box, FALSE, FALSE, 0);

            app->cpu_freq_profile_create_btn = gtk_button_new_with_label(_("Create"));
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_create_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_create_btn, "clicked", G_CALLBACK(on_cpu_create_freq_profile_clicked), app);

            app->cpu_freq_profile_rename_btn = gtk_button_new_with_label(_("Rename"));
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_rename_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_rename_btn, "clicked", G_CALLBACK(on_cpu_rename_freq_profile_clicked), app);
            gtk_widget_set_sensitive(app->cpu_freq_profile_rename_btn, FALSE); /* Disabled until profile selected */

            app->cpu_freq_profile_save_btn = gtk_button_new_with_label(_("Save"));
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_save_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_save_btn, "clicked", G_CALLBACK(on_cpu_save_freq_profile_clicked), app);
            gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, FALSE); /* Disabled until profile selected */

            app->cpu_freq_profile_delete_btn = gtk_button_new_with_label(_("Delete"));
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_delete_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_delete_btn, "clicked", G_CALLBACK(on_cpu_delete_freq_profile_clicked), app);
            gtk_widget_set_sensitive(app->cpu_freq_profile_delete_btn, FALSE); /* Disabled until profile selected */

            app->cpu_freq_profile_activate_btn = gtk_button_new_with_label(_("Activate"));
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_activate_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_activate_btn, "clicked", G_CALLBACK(on_cpu_activate_freq_profile_clicked), app);
            gtk_widget_set_sensitive(app->cpu_freq_profile_activate_btn, FALSE); /* Disabled until profile selected */
        }

        /* Status label */
        {
            app->cpu_freq_profile_status_label = gtk_label_new("");
            gtk_widget_set_halign(app->cpu_freq_profile_status_label, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(freq_box), app->cpu_freq_profile_status_label, FALSE, FALSE, 0);
        }
    }

    /* Section: Hard Frequency Limit */
    {
        GtkWidget *limit_frame = gtk_frame_new(_("Hard Frequency Limit"));
        GtkWidget *limit_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_container_set_border_width(GTK_CONTAINER(limit_box), 10);
        gtk_container_add(GTK_CONTAINER(limit_frame), limit_box);
        gtk_box_pack_start(GTK_BOX(vbox), limit_frame, FALSE, FALSE, 0);

        /* Warning label */
        {
            GtkWidget *warning_label = gtk_label_new("");
            gtk_label_set_markup(GTK_LABEL(warning_label),
                _("<span foreground='#e74c3c' weight='bold'>WARNING:</span> "
                "This is a safety limit to protect your hardware. "
                "Setting it too high may cause CPU instability, overheating, or permanent damage. "
                "Only increase this if you know your CPU's safe operating limits."));
            gtk_label_set_line_wrap(GTK_LABEL(warning_label), TRUE);
            gtk_widget_set_hexpand(warning_label, TRUE);
            gtk_box_pack_start(GTK_BOX(limit_box), warning_label, FALSE, FALSE, 0);
        }

        /* Hard limit spinner (in GHz, step 0.1, range 0.5 - 8.0) */
        {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            GtkWidget *label = gtk_label_new(_("Hard Limit (GHz):"));
            gtk_widget_set_halign(label, GTK_ALIGN_END);
            gtk_label_set_width_chars(GTK_LABEL(label), 20);
            gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);

            app->cpu_hard_limit_adjustment = gtk_adjustment_new(3.6, 0.5, 8.0, 0.1, 0.5, 0);
            app->cpu_hard_limit_spinner = gtk_spin_button_new(app->cpu_hard_limit_adjustment, 0.1, 1);
            gtk_widget_set_halign(app->cpu_hard_limit_spinner, GTK_ALIGN_START);
            gtk_widget_set_tooltip_text(app->cpu_hard_limit_spinner, _(CPU_MAX_FLOOR_NOTE));
            gtk_box_pack_start(GTK_BOX(row), app->cpu_hard_limit_spinner, FALSE, FALSE, 0);

            app->cpu_hard_limit_apply_btn = gtk_button_new_with_label(_("Apply"));
            gtk_widget_set_halign(app->cpu_hard_limit_apply_btn, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(row), app->cpu_hard_limit_apply_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_hard_limit_apply_btn, "clicked", G_CALLBACK(on_cpu_hard_limit_apply_clicked), app);

            gtk_box_pack_start(GTK_BOX(limit_box), row, FALSE, FALSE, 0);
        }
    }

    return vbox;
}
