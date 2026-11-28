// SPDX-License-Identifier: GPL-2.0-only
/*
 * gui-settings.c
 * Settings tab: components, language and display, behavior, battery.
 * Part of dcc-gui.c, which #includes it: one translation unit, so the
 * static functions and AppData stay shared (split up in 1.48.1).
 */


/* ============================================================
 * Settings tab (1.27.0): switch fan, CPU or keyboard control off when it
 * does not work on this laptop or causes problems. The state is system wide
 * (flag files in /etc/dudes-control-center, written by the root helper
 * dcc-component through pkexec): the daemons' units do not start while
 * their flag exists, also not at boot or after an update.
 * ============================================================ */

/* Tabs of switched-off components are hidden (Status shows fan data only) */
static void settings_apply_visibility(AppData *app) {
    int hide[PAGE_COUNT] = { 0 };
    hide[PAGE_STATUS] = hide[PAGE_FAN] = dcc_component_disabled(COMP_FAN);
    hide[PAGE_CPU] = dcc_component_disabled(COMP_CPU);
    hide[PAGE_KEYBOARD] = dcc_component_disabled(COMP_KEYBOARD);
    for (int p = 0; p < PAGE_COUNT; p++) {
        GtkWidget *w = app->page_widgets[p];
        if (!w) continue;
        gtk_widget_set_no_show_all(w, hide[p]);    /* gtk_widget_show_all must not bring it back */
        if (hide[p]) gtk_widget_hide(w);
        else gtk_widget_show_all(w);
    }
}

/* Switches show the current state, without running the handler */
static void settings_sync_switches(AppData *app) {
    for (int c = 0; c < COMP_COUNT; c++) {
        if (!app->settings_switch[c]) continue;
        g_signal_handler_block(app->settings_switch[c], app->settings_switch_sig[c]);
        int on = !dcc_component_disabled(c);
        gtk_switch_set_active(GTK_SWITCH(app->settings_switch[c]), on);
        gtk_switch_set_state(GTK_SWITCH(app->settings_switch[c]), on);
        gtk_widget_set_sensitive(app->settings_switch[c], !app->settings_busy);
        g_signal_handler_unblock(app->settings_switch[c], app->settings_switch_sig[c]);
    }
    settings_update_ec(app);
    update_daemon_status_labels(app);
}

/* A component was switched on: connect again once its daemon is up */
static gboolean settings_reconnect_cb(gpointer user_data) {
    AppData *app = (AppData *)user_data;
    if (!dcc_component_disabled(COMP_FAN) && !app->connected) {
        struct stat st;
        if (stat(SOCKET_PATH, &st) == 0) {          /* no error dialog while it starts */
            connect_to_daemon(app);
            if (app->connected) {
                refresh_profile_list(app);
                if (app->active_profile_idx >= 0) load_curve_points_for_profile(app, app->active_profile_idx);
            }
        }
    }
    if (!dcc_component_disabled(COMP_CPU) && !app->cpu_connected) {
        connect_to_cpu_daemon(app);
        if (app->cpu_connected) {
            cpu_load_initial_data(app);
            load_cpu_custom_profiles(app);
        }
    }
    if (!dcc_component_disabled(COMP_KEYBOARD) && !app->kbd_connected) kbd_ensure_connected(app);
    update_tray_menu(app);
    settings_sync_switches(app);
    return G_SOURCE_REMOVE;
}

typedef struct { AppData *app; int comp; int enable; } SettingsJob;

static void settings_helper_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    SettingsJob *job = (SettingsJob *)user_data;
    AppData *app = job->app;
    GError *err = NULL;
    gboolean ok = g_subprocess_wait_finish(G_SUBPROCESS(source), res, &err);
    int status = ok && g_subprocess_get_if_exited(G_SUBPROCESS(source)) ? g_subprocess_get_exit_status(G_SUBPROCESS(source)) : -1;
    const char *name = dcc_component_names[job->comp];
    app->settings_busy = 0;
    if (status == 0) {
        gui_event("[GUI] Settings: %s control %s\n", name, job->enable ? "switched on" : "switched off");
        show_status(app, job->enable ? _("Component switched on") : _("Component switched off"));
        if (!job->enable) {                          /* stop talking to that daemon */
            if (job->comp == COMP_FAN) disconnect_from_daemon(app);
            if (job->comp == COMP_CPU && app->cpu_socket_fd >= 0) {
                close(app->cpu_socket_fd); app->cpu_socket_fd = -1; app->cpu_connected = 0;
            }
            if (job->comp == COMP_KEYBOARD && app->kbd_socket_fd >= 0) {
                close(app->kbd_socket_fd); app->kbd_socket_fd = -1; app->kbd_connected = 0;
            }
            update_tray_menu(app);
        } else {
            g_timeout_add(1500, settings_reconnect_cb, app);
        }
    } else {
        /* 126/127: authentication dismissed or failed */
        g_print("[GUI] Settings: switching %s %s failed (exit %d%s%s)\n", name, job->enable ? "on" : "off",
                status, err ? ": " : "", err ? err->message : "");
        show_status(app, status == 126 || status == 127 ? _("Not changed: authentication was cancelled or failed")
                                                       : _("Could not change the component (see the GUI log)"));
    }
    if (err) g_error_free(err);
    settings_apply_visibility(app);
    settings_sync_switches(app);
    g_free(job);
}

/* Switch toggled: run the root helper through pkexec (asks for the
 * administrator password); the switch follows the result */
static gboolean on_settings_switch(GtkSwitch *sw, gboolean state, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int comp = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(sw), "component"));
    if (app->settings_busy) return TRUE;
    if (!state && comp == COMP_FAN) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                              GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL,
                                              _("Switch fan control off?"));
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
            _("The fans will run at 100%% until the next reboot. After the reboot the laptop's firmware "
            "controls them again (there is no known way to hand them back to it earlier)."));
        int r = gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        if (r != GTK_RESPONSE_OK) {
            settings_sync_switches(app);
            return TRUE;
        }
    }
    GError *err = NULL;
    GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &err, "pkexec", DCC_COMPONENT_HELPER,
                                         state ? "enable" : "disable", dcc_component_names[comp], NULL);
    if (!proc) {
        gui_event("[GUI] Settings: cannot run pkexec: %s\n", err ? err->message : "?");
        show_status(app, _("Could not run pkexec"));
        if (err) g_error_free(err);
        settings_sync_switches(app);
        return TRUE;
    }
    SettingsJob *job = g_new0(SettingsJob, 1);
    job->app = app;
    job->comp = comp;
    job->enable = state ? 1 : 0;
    app->settings_busy = 1;
    for (int c = 0; c < COMP_COUNT; c++) gtk_widget_set_sensitive(app->settings_switch[c], FALSE);
    show_status(app, _("Waiting for authentication..."));
    g_subprocess_wait_async(proc, NULL, settings_helper_done, job);
    g_object_unref(proc);
    return TRUE;   /* the state is set when the helper is done */
}

/* Language (1.31.0): the choice is stored; it applies after a restart */
static void settings_update_language_note(AppData *app) {
    if (!app->settings_language_note) return;
    const char *chosen = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->settings_language_combo));
    int pending = 0;
    if (chosen) {
        cJSON *json = config_read();
        cJSON *v = cJSON_GetObjectItem(json, "language");
        const char *saved = cJSON_IsString(v) ? v->valuestring : "system";
        pending = strcmp(saved, ui_language_code) != 0;
        cJSON_Delete(json);
    }
    gtk_label_set_text(GTK_LABEL(app->settings_language_note),
        pending ? _("Restart the GUI to use the new language.")
                : _("Japanese and Korean translations were made without a native speaker's review."));
    gtk_widget_set_sensitive(app->settings_restart_btn, pending);
}

static void on_settings_language_changed(GtkComboBox *combo, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    const char *code = gtk_combo_box_get_active_id(combo);
    if (!code) return;
    cJSON *json = config_read();
    cJSON_DeleteItemFromObject(json, "language");
    cJSON_AddStringToObject(json, "language", code);
    config_write(json);
    cJSON_Delete(json);
    gui_event("[GUI] Language set to '%s' (after a restart)\n", code);
    settings_update_language_note(app);
}

/* Restart the GUI in place (same process, new program image), after the
 * same unsaved-changes questions as quitting */
static void on_settings_restart_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    if (app->curve_points_modified) {
        int r = check_unsaved_changes(app);
        if (r < 0) return;
        if (r == 1) on_apply_clicked(NULL, app);
        app->curve_points_modified = 0;
    }
    if (cpu_check_unsaved_changes(app) < 0) return;
    if (kbd_leave_tab(app) < 0) return;
    if (gtk_widget_get_visible(app->window)) {
        gtk_window_get_position(GTK_WINDOW(app->window), &app->window_x, &app->window_y);
        app->window_width = gtk_widget_get_allocated_width(app->window);
        app->window_height = gtk_widget_get_allocated_height(app->window);
        save_window_position(app);
    }
    gui_event("[GUI] Restarting for the new language\n");
    disconnect_from_daemon(app);
    disconnect_from_cpu_daemon(app);
    if (app->kbd_socket_fd >= 0) { close(app->kbd_socket_fd); app->kbd_socket_fd = -1; }
    /* The restarted GUI opens its window again, whatever "Start minimized"
     * says (1.40.0) */
    char *rargv[4];
    int rn = 0;
    rargv[rn++] = gui_argv[0];
    if (gui_debug_flag) rargv[rn++] = "--debug";
    rargv[rn++] = gtk_widget_get_visible(app->window) ? "--show" : "--minimized";
    rargv[rn] = NULL;
    execv("/proc/self/exe", rargv);
    g_print("[GUI] Restart failed: %s\n", strerror(errno));
    show_status(app, _("Restart failed - please quit and start the GUI again"));
}

/* Which theme is in use and whether it is dark */
static void settings_update_appearance_note(AppData *app) {
    if (!app->settings_appearance_note) return;
    gchar *theme = NULL;
    g_object_get(gtk_settings_get_default(), "gtk-theme-name", &theme, NULL);
    gchar *t = g_strdup_printf(_("%sTheme in use: %s (%s). The keyboard picture and the fan curve graph follow it."),
                               app->appearance == APPEARANCE_SYSTEM ? _("Uses the desktop's theme and follows its changes. ") : "",
                               theme ? theme : "?", app->is_dark ? _("dark") : _("light"));
    gtk_label_set_text(GTK_LABEL(app->settings_appearance_note), t);
    g_free(t);
    g_free(theme);
}

static void on_settings_appearance_changed(GtkComboBox *combo, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int mode = gtk_combo_box_get_active(combo);
    if (mode < 0 || mode > 2 || mode == app->appearance) return;
    app->appearance = mode;
    appearance_save_choice(mode);
    appearance_apply_theme(mode);
    appearance_refresh(app);
    show_status(app, _("Appearance changed"));
}

/* What the LED mapping of the current layout is based on */
static void settings_update_layout_note(AppData *app) {
    if (!app->settings_layout_note) return;
    char path[512];
    kbd_mapping_path(path, sizeof path, kbd_layout_code);
    const char *t;
    if (access(path, F_OK) == 0)
        t = _("The picture shows this layout. LED mapping: saved by the LED Wizard for this layout.");
    else if (!strcmp(kbd_layout_code, "de"))
        t = _("The picture shows this layout. LED mapping: built in, checked on a German keyboard.");
    else if (!strcmp(kbd_layout_code, "uk") || !strcmp(kbd_layout_code, "es"))
        t = _("The picture shows this layout. LED mapping: derived from the German layout by key position "
            "(not checked) - run the LED Wizard once (Keyboard Backlight tab) to make sure every key is right.");
    else
        t = _("The picture shows this layout. LED mapping: derived from the German layout where the keys match; "
            "keys this layout has in addition have no LED yet - run the LED Wizard once (Keyboard Backlight tab).");
    gtk_label_set_text(GTK_LABEL(app->settings_layout_note), t);
}

/* Another layout: its keys, picture and LED mapping; zones and profiles
 * keep their colors (they store LED numbers) */
static void on_settings_layout_changed(GtkComboBox *combo, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    const char *code = gtk_combo_box_get_active_id(combo);
    if (!code || !strcmp(code, kbd_layout_code)) return;
    if (app->kbd_wizard_active) {
        show_status(app, _("Finish or cancel the LED Wizard first"));
        gtk_combo_box_set_active_id(combo, kbd_layout_code);
        return;
    }
    if (kbd_layout_load(code) != 0) {
        show_status(app, _("Could not load this keyboard layout"));
        gtk_combo_box_set_active_id(combo, kbd_layout_code);
        return;
    }
    kbd_layout_save_choice();
    kbd_load_led_mapping_csv(app);
    kbd_load_layout_picture(app);
    kbd_highlight_zone(app);
    kbd_redraw_canvas(app);
    settings_update_layout_note(app);
    show_status(app, _("Keyboard layout changed"));
}

/* ============================================================
 * Start minimized to tray (1.40.0). "start_minimized" in config.json
 * (default on: the GUI always started in the tray before). The command line
 * wins: --minimized or --show. The setting also sets the Exec line of the
 * desktop's entries, so the menu and the startup applications show what
 * they do: a per-user copy of the menu entry in ~/.local/share/applications
 * (the system one in /usr/share is root's) and the user's autostart entries
 * that start dcc-gui. Only the Exec line in [Desktop Entry] is changed.
 * ============================================================ */
#define DESKTOP_FILE_NAME "dudes-control-center.desktop"
#define DESKTOP_SYSTEM_FILE "/usr/share/applications/" DESKTOP_FILE_NAME
#define DESKTOP_MARKER "X-DCC-Generated=true"

static int start_minimized_load(void) {
    cJSON *json = config_read();
    cJSON *v = cJSON_GetObjectItem(json, "start_minimized");
    int on = cJSON_IsBool(v) ? cJSON_IsTrue(v) : 1;
    cJSON_Delete(json);
    return on;
}

static int start_minimized_is_set(void) {
    cJSON *json = config_read();
    int set = cJSON_IsBool(cJSON_GetObjectItem(json, "start_minimized"));
    cJSON_Delete(json);
    return set;
}

static void start_minimized_save(int on) {
    cJSON *json = config_read();
    cJSON_DeleteItemFromObject(json, "start_minimized");
    cJSON_AddBoolToObject(json, "start_minimized", on ? 1 : 0);
    config_write(json);
    cJSON_Delete(json);
}

/* Does an Exec value start dcc-gui (any path)? */
static int desktop_exec_is_gui(const char *val) {
    while (*val == ' ' || *val == '\t') val++;
    size_t n = strcspn(val, " \t");
    const char *slash = NULL;
    for (const char *c = val; c < val + n; c++) if (*c == '/') slash = c;
    const char *base = slash ? slash + 1 : val;
    return (size_t)(val + n - base) == 7 && strncmp(base, "dcc-gui", 7) == 0;
}

/* The content with every dcc-gui Exec= line of [Desktop Entry] ending in
 * " --minimized" (on) or without it; --show and --minimized are dropped
 * first, other arguments kept. *changed tells whether anything differs. */
static gchar *desktop_exec_rewrite(const char *content, int minimized, int *changed) {
    GString *out = g_string_new(NULL);
    gchar **lines = g_strsplit(content, "\n", -1);
    int in_entry = 0;
    *changed = 0;
    for (int i = 0; lines[i]; i++) {
        const char *l = lines[i];
        if (l[0] == '[') in_entry = strncmp(l, "[Desktop Entry]", 15) == 0;
        if (in_entry && strncmp(l, "Exec=", 5) == 0 && desktop_exec_is_gui(l + 5)) {
            GString *e = g_string_new("Exec=");
            gchar **tok = g_strsplit_set(l + 5, " \t", -1);
            int first = 1;
            for (int t = 0; tok[t]; t++) {
                if (!*tok[t] || !strcmp(tok[t], "--minimized") || !strcmp(tok[t], "--show")) continue;
                if (!first) g_string_append_c(e, ' ');
                g_string_append(e, tok[t]);
                first = 0;
            }
            g_strfreev(tok);
            if (minimized) g_string_append(e, " --minimized");
            if (strcmp(e->str, l) != 0) *changed = 1;
            g_string_append(out, e->str);
            g_string_free(e, TRUE);
        } else {
            g_string_append(out, l);
        }
        if (lines[i + 1]) g_string_append_c(out, '\n');
    }
    g_strfreev(lines);
    return g_string_free(out, FALSE);
}

/* Rewrite one file's Exec line; not through symlinks, only regular files */
static int desktop_file_update(const char *path, int minimized) {
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    gchar *data = NULL;
    if (!g_file_get_contents(path, &data, NULL, NULL)) return 0;
    int changed = 0;
    gchar *nd = desktop_exec_rewrite(data, minimized, &changed);
    int ok = 1;
    if (changed) {
        ok = g_file_set_contents(path, nd, -1, NULL);
        gui_event("[GUI] %s %s\n", ok ? "Updated start option in" : "WARNING: cannot update", path);
    }
    g_free(nd);
    g_free(data);
    return ok;
}

/* Menu entry and autostart entries for the setting. sys_file: the
 * package's menu entry; apps_dir: ~/.local/share/applications;
 * autostart_dir: ~/.config/autostart. */
static void desktop_entries_apply(int minimized, const char *sys_file, const char *apps_dir,
                                  const char *autostart_dir) {
    gchar *user_file = g_build_filename(apps_dir, DESKTOP_FILE_NAME, NULL);
    struct stat st;
    int exists = lstat(user_file, &st) == 0;
    gchar *data = NULL;
    int ours = exists && S_ISREG(st.st_mode) && g_file_get_contents(user_file, &data, NULL, NULL) &&
               strstr(data, DESKTOP_MARKER) != NULL;
    g_free(data);
    if (ours && !minimized) {
        /* our copy is not needed: the system entry starts without options */
        if (unlink(user_file) == 0) gui_event("[GUI] Removed %s\n", user_file);
    } else if (ours || (minimized && !exists)) {
        /* (re)create from the package's entry, so it follows updates */
        gchar *sys = NULL;
        if (g_file_get_contents(sys_file, &sys, NULL, NULL)) {
            int changed = 0;
            gchar *nd = desktop_exec_rewrite(sys, minimized, &changed);
            GString *g = g_string_new(nd);
            if (g->len && g->str[g->len - 1] != '\n') g_string_append_c(g, '\n');
            /* the marker goes into [Desktop Entry]: right after its header */
            char *hdr = strstr(g->str, "[Desktop Entry]\n");
            if (hdr) g_string_insert(g, (hdr - g->str) + 16, DESKTOP_MARKER "\n");
            else g_string_append(g, DESKTOP_MARKER "\n");
            g_mkdir_with_parents(apps_dir, 0755);
            gchar *old = NULL;
            if (!(ours && g_file_get_contents(user_file, &old, NULL, NULL) && !strcmp(old, g->str))) {
                if (g_file_set_contents(user_file, g->str, -1, NULL))
                    gui_event("[GUI] Wrote menu entry %s\n", user_file);
                else
                    gui_event("[GUI] WARNING: cannot write %s\n", user_file);
            }
            g_free(old);
            g_string_free(g, TRUE);
            g_free(nd);
            g_free(sys);
        }
    } else if (exists) {
        desktop_file_update(user_file, minimized);   /* the user's own copy: only Exec */
    }
    g_free(user_file);

    DIR *d = opendir(autostart_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!g_str_has_suffix(e->d_name, ".desktop")) continue;
            gchar *f = g_build_filename(autostart_dir, e->d_name, NULL);
            desktop_file_update(f, minimized);
            g_free(f);
        }
        closedir(d);
    }
}

static void desktop_entries_update(int minimized) {
    gchar *apps = g_build_filename(g_get_user_data_dir(), "applications", NULL);
    gchar *autostart = g_build_filename(g_get_user_config_dir(), "autostart", NULL);
    desktop_entries_apply(minimized, DESKTOP_SYSTEM_FILE, apps, autostart);
    g_free(apps);
    g_free(autostart);
}

static gboolean on_settings_minimized_set(GtkSwitch *sw, gboolean state, gpointer user_data) {
    (void)sw; (void)user_data;
    start_minimized_save(state);
    desktop_entries_update(state);
    gui_event("[GUI] Start minimized to tray: %s\n", state ? "on" : "off");
    return FALSE;
}

/* ============================================================
 * Refresh interval (1.40.0): how often the GUI asks the daemons for
 * temperatures, fan speeds, CPU frequencies, power and GPU values. It used
 * to be fixed (fan/Status 2 s, CPU 5 s); now one setting for both timers,
 * stored as "refresh_interval" (seconds) in config.json.
 * ============================================================ */
static const int refresh_choices[] = { 1, 2, 3, 5, 10 };
#define REFRESH_NUM_CHOICES ((int)(sizeof refresh_choices / sizeof refresh_choices[0]))
#define REFRESH_DEFAULT_SEC 2

/* A stored value if it is one of the choices, else the default */
static int refresh_sec_valid(int sec) {
    for (int i = 0; i < REFRESH_NUM_CHOICES; i++)
        if (refresh_choices[i] == sec) return sec;
    return REFRESH_DEFAULT_SEC;
}

static int refresh_sec_load(void) {
    cJSON *json = config_read();
    cJSON *v = cJSON_GetObjectItem(json, "refresh_interval");
    int sec = cJSON_IsNumber(v) ? refresh_sec_valid(v->valueint) : REFRESH_DEFAULT_SEC;
    cJSON_Delete(json);
    return sec;
}

static void refresh_sec_save(int sec) {
    cJSON *json = config_read();
    cJSON_DeleteItemFromObject(json, "refresh_interval");
    cJSON_AddNumberToObject(json, "refresh_interval", sec);
    config_write(json);
    cJSON_Delete(json);
}

/* Timer ticks between keyboard syncs, so they stay about 6 s apart */
static int refresh_kbd_ticks(int sec) {
    if (sec <= 0) sec = REFRESH_DEFAULT_SEC;
    int t = (6 + sec - 1) / sec;
    return t < 1 ? 1 : t;
}

/* (Re)start both refresh timers with app->refresh_sec */
static void refresh_timers_start(AppData *app) {
    if (app->refresh_sec <= 0) app->refresh_sec = refresh_sec_load();
    if (app->refresh_timer_id > 0) g_source_remove(app->refresh_timer_id);
    if (app->cpu_refresh_timer_id > 0) g_source_remove(app->cpu_refresh_timer_id);
    app->refresh_timer_id = g_timeout_add_seconds((guint)app->refresh_sec, refresh_timer_callback, app);
    app->cpu_refresh_timer_id = g_timeout_add_seconds((guint)app->refresh_sec, cpu_refresh_timer_callback, app);
}

static void on_settings_refresh_changed(GtkComboBox *combo, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int i = gtk_combo_box_get_active(combo);
    if (i < 0 || i >= REFRESH_NUM_CHOICES || refresh_choices[i] == app->refresh_sec) return;
    app->refresh_sec = refresh_choices[i];
    refresh_sec_save(app->refresh_sec);
    refresh_timers_start(app);
    gui_event("[GUI] Refresh interval set to %d s\n", app->refresh_sec);
}

/* Settings: generic hwmon fan control (1.43.0). The switch follows the
 * opt-in file; the label says what was found and whether it is used. */
static void settings_update_hw(AppData *app) {
    if (!app->settings_hw_switch) return;
    int on = hwmon_fans_allowed();
    GtkSwitch *sw = GTK_SWITCH(app->settings_hw_switch);
    if (!app->ec_dialog && (gtk_switch_get_active(sw) != on || gtk_switch_get_state(sw) != on)) {
        g_signal_handler_block(sw, app->settings_hw_sig);
        gtk_switch_set_active(sw, on);
        gtk_switch_set_state(sw, on);
        g_signal_handler_unblock(sw, app->settings_hw_sig);
    }
    gtk_widget_set_sensitive(app->settings_hw_switch, !app->settings_busy && !app->ec_dialog);
    char dir[512], name[32], t[160];
    int n = hwmon_fans_find(dir, sizeof dir, name, sizeof name);
    if (!strcmp(app->ec_access, "module"))
        snprintf(t, sizeof t, "%s", _("Not needed: dcc_fan module in use"));
    else if (!strcmp(app->ec_access, "hwmon"))
        snprintf(t, sizeof t, _("On, in use: %s"), app->hw_driver);
    else if (n > 0)
        snprintf(t, sizeof t, on ? _("On, not in use: %s fans found") : _("Off: %s fans found"), name);
    else
        snprintf(t, sizeof t, "%s", on ? _("On, but no fan with a manual mode found") : _("Off: no fan with a manual mode found"));
    label_set_changed(app->settings_hw_state, t);
}

/* Settings: the direct EC access switch follows the opt-in file; the label
 * says whether it is used */
/* The sidebar marks the "Other laptops" page while direct EC access is
 * switched on (1.49.0) */
static void settings_other_mark(AppData *app, int on) {
    if (!app->settings_stack || !app->settings_other_page || app->settings_other_warn == on + 1) return;
    gchar *title = on ? g_strdup_printf("%s \u26a0", _("Other laptops")) : g_strdup(_("Other laptops"));
    gtk_container_child_set(GTK_CONTAINER(app->settings_stack), app->settings_other_page, "title", title, NULL);
    g_free(title);
    app->settings_other_warn = on + 1;
}

static void settings_update_ec(AppData *app) {
    if (!app->settings_ec_switch) return;
    int on = ec_direct_allowed();
    if (!app->ec_dialog && (gtk_switch_get_active(GTK_SWITCH(app->settings_ec_switch)) != on ||
                            gtk_switch_get_state(GTK_SWITCH(app->settings_ec_switch)) != on)) {
        g_signal_handler_block(app->settings_ec_switch, app->settings_ec_sig);
        gtk_switch_set_active(GTK_SWITCH(app->settings_ec_switch), on);
        gtk_switch_set_state(GTK_SWITCH(app->settings_ec_switch), on);
        g_signal_handler_unblock(app->settings_ec_switch, app->settings_ec_sig);
    }
    gtk_widget_set_sensitive(app->settings_ec_switch, !app->settings_busy && !app->ec_dialog);
    const char *t;
    if (!strcmp(app->ec_access, "module")) t = _("Not needed: dcc_fan module in use");
    else if (!strcmp(app->ec_access, "ports")) t = _("On, in use");
    else if (!strcmp(app->ec_access, "hwmon")) t = _("Not needed: generic fan control in use");
    else if (!strcmp(app->ec_access, "none")) t = on ? _("On, refused: the EC check failed") : _("Off: no fan control");
    else t = on ? _("On") : _("Off");
    label_set_changed(app->settings_ec_state, t);
    tooltip_set_changed(app->settings_ec_state, app->ec_reason[0] ? app->ec_reason : NULL);
    settings_other_mark(app, on);
    settings_update_hw(app);
}

typedef struct { AppData *app; int enable; int hw; } EcJob;   /* hw: generic fans, else direct EC */

static void settings_ec_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    EcJob *job = (EcJob *)user_data;
    AppData *app = job->app;
    GError *err = NULL;
    gboolean ok = g_subprocess_wait_finish(G_SUBPROCESS(source), res, &err);
    int status = ok && g_subprocess_get_if_exited(G_SUBPROCESS(source)) ? g_subprocess_get_exit_status(G_SUBPROCESS(source)) : -1;
    app->settings_busy = 0;
    if (status == 0) {
        gui_event("[GUI] Settings: %s %s\n", job->hw ? "generic fan control" : "direct EC access",
                  job->enable ? "switched on" : "switched off");
        if (job->hw)
            show_status(app, job->enable ? _("Generic fan control switched on - the fan daemon was restarted")
                                         : _("Generic fan control switched off - the fans are back in automatic mode"));
        else
            show_status(app, job->enable ? _("Direct EC access switched on - the fan daemon was restarted")
                                         : _("Direct EC access switched off - the fan daemon was restarted"));
        disconnect_from_daemon(app);              /* it was restarted: connect again */
        app->ec_access[0] = '\0';
        g_timeout_add(2000, settings_reconnect_cb, app);
    } else {
        g_print("[GUI] Settings: switching %s %s failed (exit %d%s%s)\n",
                job->hw ? "generic fan control" : "direct EC access", job->enable ? "on" : "off",
                status, err ? ": " : "", err ? err->message : "");
        show_status(app, status == 126 || status == 127 ? _("Not changed: authentication was cancelled or failed")
                                                       : _("Could not change the component (see the GUI log)"));
    }
    if (err) g_error_free(err);
    settings_sync_switches(app);
    g_free(job);
}

static void ec_risk_toggled(GtkToggleButton *cb, gpointer user_data) {
    gtk_widget_set_sensitive(GTK_WIDGET(user_data), gtk_toggle_button_get_active(cb));
}

/* The warning before direct EC access: "Switch on" only after the box is ticked */
static GtkWidget *ec_warning_dialog(GtkWindow *parent) {
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                          _("Switch on direct EC access?"));
    gtk_message_dialog_format_secondary_markup(GTK_MESSAGE_DIALOG(d), "%s",
        _("<b>This can damage your laptop.</b> The fan daemon would then write directly to the Embedded "
          "Controller through the I/O ports 0x62/0x66, using the register layout of Clevo laptops. On a laptop "
          "with a different layout, these writes can change charging, power or thermal settings, stop the "
          "fans or overheat the hardware.\n\n"
          "Only use it on a Clevo-based laptop where the dcc_fan module does not load. Before the first write, "
          "the daemon checks that the EC answers like a Clevo EC and refuses otherwise, but this check cannot "
          "rule out every risk.\n\nYou use it at your own risk."));
    gtk_dialog_add_button(GTK_DIALOG(d), _("_Cancel"), GTK_RESPONSE_CANCEL);
    GtkWidget *ok = gtk_dialog_add_button(GTK_DIALOG(d), _("Switch on"), GTK_RESPONSE_OK);
    gtk_style_context_add_class(gtk_widget_get_style_context(ok), "destructive-action");
    gtk_widget_set_sensitive(ok, FALSE);
    GtkWidget *cb = gtk_check_button_new_with_label(_("I understand the risk and use this on a Clevo-based laptop"));
    g_signal_connect(cb, "toggled", G_CALLBACK(ec_risk_toggled), ok);
    gtk_box_pack_start(GTK_BOX(gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(d))), cb, FALSE, FALSE, 6);
    gtk_widget_show(cb);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    gtk_window_set_title(GTK_WINDOW(d), _("Direct EC access"));
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER_ON_PARENT);
    return d;
}

static void ec_direct_run(AppData *app, int hw, int enable) {
    GError *err = NULL;
    GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &err, "pkexec", DCC_COMPONENT_HELPER,
                                         hw ? "hwmon-fans" : "ec-direct", enable ? "enable" : "disable", NULL);
    if (!proc) {
        gui_event("[GUI] Settings: cannot run pkexec: %s\n", err ? err->message : "?");
        show_status(app, _("Could not run pkexec"));
        if (err) g_error_free(err);
        settings_update_ec(app);
        return;
    }
    EcJob *job = g_new0(EcJob, 1);
    job->app = app;
    job->enable = enable;
    job->hw = hw;
    app->settings_busy = 1;
    for (int c = 0; c < COMP_COUNT; c++) gtk_widget_set_sensitive(app->settings_switch[c], FALSE);
    gtk_widget_set_sensitive(app->settings_ec_switch, FALSE);
    if (app->settings_hw_switch) gtk_widget_set_sensitive(app->settings_hw_switch, FALSE);
    show_status(app, _("Waiting for authentication..."));
    g_subprocess_wait_async(proc, NULL, settings_ec_done, job);
    g_object_unref(proc);
}

static void ec_dialog_response(GtkDialog *d, int response, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int hw = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(d), "hw"));
    gtk_widget_destroy(GTK_WIDGET(d));
    app->ec_dialog = NULL;
    if (response == GTK_RESPONSE_OK) ec_direct_run(app, hw, 1);
    else settings_update_ec(app);
}

/* Switching it on needs a confirmed warning, then the administrator
 * password (pkexec dcc-component ec-direct enable). The warning is not run
 * with gtk_dialog_run(): a nested main loop inside the switch's state-set
 * handler froze the GUI and left the dialog blank (1.41.0 test). */
static gboolean on_settings_ec_switch(GtkSwitch *sw, gboolean state, gpointer user_data) {
    (void)sw;
    AppData *app = (AppData *)user_data;
    if (app->settings_busy || app->ec_dialog) return TRUE;
    if (state) {
        app->ec_dialog = ec_warning_dialog(GTK_WINDOW(app->window));
        g_signal_connect(app->ec_dialog, "response", G_CALLBACK(ec_dialog_response), app);
        gtk_widget_set_sensitive(app->settings_ec_switch, FALSE);
        if (app->settings_hw_switch) gtk_widget_set_sensitive(app->settings_hw_switch, FALSE);
        gtk_window_present(GTK_WINDOW(app->ec_dialog));
        return TRUE;
    }
    ec_direct_run(app, 0, 0);
    return TRUE;
}

/* The short note before generic fan control (1.43.0): the standard
 * interface is meant for this, so no tick box like for direct EC access */
static GtkWidget *hw_warning_dialog(GtkWindow *parent, const char *driver) {
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                          _("Switch on generic fan control?"));
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
        _("The fan daemon then sets the speed of the %s driver's fans through the kernel's standard interface, "
          "following your fan profiles. Only fans whose driver offers a manual mode are used.\n\n"
          "If the fan daemon stops, the fans run at 100%% until it starts again or the laptop restarts. "
          "Switched off, the fans go back to the driver's automatic mode."), driver[0] ? driver : "?");
    gtk_dialog_add_button(GTK_DIALOG(d), _("_Cancel"), GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Switch on"), GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    gtk_window_set_title(GTK_WINDOW(d), _("Generic fan control"));
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER_ON_PARENT);
    g_object_set_data(G_OBJECT(d), "hw", GINT_TO_POINTER(1));
    return d;
}

static gboolean on_settings_hw_switch(GtkSwitch *sw, gboolean state, gpointer user_data) {
    (void)sw;
    AppData *app = (AppData *)user_data;
    if (app->settings_busy || app->ec_dialog) return TRUE;
    if (state) {
        char dir[512], name[32] = "";
        hwmon_fans_find(dir, sizeof dir, name, sizeof name);
        app->ec_dialog = hw_warning_dialog(GTK_WINDOW(app->window), name);
        g_signal_connect(app->ec_dialog, "response", G_CALLBACK(ec_dialog_response), app);
        gtk_widget_set_sensitive(app->settings_ec_switch, FALSE);
        gtk_widget_set_sensitive(app->settings_hw_switch, FALSE);
        gtk_window_present(GTK_WINDOW(app->ec_dialog));
        return TRUE;
    }
    ec_direct_run(app, 1, 0);
    return TRUE;
}

/* Settings page parts (1.40.1): a grid with aligned labels, one row per
 * setting with a small grey note under its control */
static GtkWidget *settings_note(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 110);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
    PangoAttrList *at = pango_attr_list_new();
    pango_attr_list_insert(at, pango_attr_scale_new(0.9));
    gtk_label_set_attributes(GTK_LABEL(l), at);
    pango_attr_list_unref(at);
    return l;
}

/* A note with markup (option names in <tt>: two hyphens melt together in
 * the small proportional font) */
static GtkWidget *settings_note_markup(const char *markup) {
    GtkWidget *l = settings_note("");
    gtk_label_set_markup(GTK_LABEL(l), markup);
    PangoAttrList *at = pango_attr_list_new();
    pango_attr_list_insert(at, pango_attr_scale_new(0.9));
    gtk_label_set_attributes(GTK_LABEL(l), at);
    pango_attr_list_unref(at);
    return l;
}

static GtkWidget *settings_grid(void) {
    GtkWidget *g = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(g), 10);
    gtk_grid_set_row_spacing(GTK_GRID(g), 4);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    return g;
}

/* label | control, then the note under the control; rows 10 px apart */
static void settings_grid_row(GtkWidget *g, int *row, const char *label, GtkWidget *control, GtkWidget *note) {
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    if (*row > 0) gtk_widget_set_margin_top(l, 10), gtk_widget_set_margin_top(control, 10);
    gtk_grid_attach(GTK_GRID(g), l, 0, *row, 1, 1);
    gtk_widget_set_halign(control, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g), control, 1, *row, 1, 1);
    (*row)++;
    if (note) {
        gtk_widget_set_hexpand(note, TRUE);
        gtk_grid_attach(GTK_GRID(g), note, 1, *row, 1, 1);
        (*row)++;
    }
}

/* Battery charge limit (1.46.0) */
static const int bat_limit_choices[] = { 100, 90, 85, 80, 75, 70, 60, 50 };

static void settings_update_battery(AppData *app) {
    int lim = app->bat_limit_seen ? app->bat_limit : -2;
    if (app->bat_limit_label) {
        if (lim >= 0 && lim < 100) {
            gchar *t = g_strdup_printf(_("Charge limit: %d %%"), lim);
            label_set_changed(app->bat_limit_label, t);
            g_free(t);
        }
        widget_set_shown(app->bat_limit_label, lim >= 0 && lim < 100);
    }
    if (!app->settings_bat_combo) return;
    gboolean ok = lim >= 0 && app->cpu_connected;
    if (gtk_widget_get_sensitive(app->settings_bat_combo) != ok) gtk_widget_set_sensitive(app->settings_bat_combo, ok);
    gchar *note;
    if (!app->cpu_connected || lim == -2)
        note = g_strdup(_("Needs the CPU daemon (CPU control in Components)."));
    else if (lim < 0)
        note = g_strdup(_("This laptop's battery driver offers no charge limit (charge_control_end_threshold)."));
    else
        note = g_strdup_printf(_("Stops charging %s at this level, which is easier on a battery that is plugged in "
                                 "most of the time. Set again after a reboot or standby; some drivers only take "
                                 "certain values."), app->bat_limit_dev);
    label_set_changed(app->settings_bat_note, note);
    g_free(note);
    if (lim < 0) return;
    char id[16];
    g_snprintf(id, sizeof id, "%d", lim);
    const char *cur = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app->settings_bat_combo));
    if (cur && !strcmp(cur, id)) return;
    app->settings_bat_syncing = 1;
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->settings_bat_combo), id)) {
        /* a value set elsewhere (dcc-cpu-cli, the driver rounded it) */
        gchar *t = g_strdup_printf(_("%d %%"), lim);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->settings_bat_combo), id, t);
        g_free(t);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->settings_bat_combo), id);
    }
    app->settings_bat_syncing = 0;
}

static void on_settings_bat_changed(GtkComboBox *combo, gpointer user_data) {
    AppData *app = user_data;
    if (app->settings_bat_syncing) return;
    const char *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    char cmd[64];
    g_snprintf(cmd, sizeof cmd, "SET_BATTERY_LIMIT|%s", id);
    const char *r = send_cpu_command(app, cmd);
    if (r && !strncmp(r, "OK", 2)) {
        app->bat_limit = atoi(id);
        gui_event("[GUI] Battery charge limit: %s\n", atoi(id) >= 100 ? "off" : id);
        settings_update_battery(app);
        return;
    }
    /* refused: show why, back to the real value */
    gchar *msg = g_strdup_printf(_("Not changed: %s"), r && !strncmp(r, "ERROR ", 6) ? r + 6 : _("no answer from the CPU daemon"));
    g_strchomp(msg);
    int keep = app->bat_limit;
    char kid[16];
    g_snprintf(kid, sizeof kid, "%d", keep);
    app->settings_bat_syncing = 1;
    gtk_combo_box_set_active_id(combo, kid);
    app->settings_bat_syncing = 0;
    label_set_changed(app->settings_bat_note, msg);
    g_free(msg);
}

/* One page of the Settings stack (1.49.0): a scrolled window holding a
 * vertical box; returns the box to fill */
static GtkWidget *settings_stack_page(AppData *app, const char *name, const char *title) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled), box);
    gtk_stack_add_titled(GTK_STACK(app->settings_stack), scrolled, name, title);
    return box;
}

/* The page shown is remembered in config.json ("settings_page") */
static void on_settings_page_changed(GObject *stack, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    const char *name = gtk_stack_get_visible_child_name(GTK_STACK(stack));
    if (!name) return;
    cJSON *json = config_read();
    cJSON *v = cJSON_GetObjectItem(json, "settings_page");
    if (!cJSON_IsString(v) || strcmp(v->valuestring, name)) {
        cJSON_DeleteItemFromObject(json, "settings_page");
        cJSON_AddStringToObject(json, "settings_page", name);
        config_write(json);
    }
    cJSON_Delete(json);
}

static GtkWidget *create_settings_page(AppData *app) {
    /* Four pages with a sidebar (1.49.0); each page scrolls on its own */
    app->settings_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app->settings_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    GtkWidget *pg_general = settings_stack_page(app, "general", _("General"));
    GtkWidget *pg_components = settings_stack_page(app, "components", _("Components"));
    GtkWidget *pg_hardware = settings_stack_page(app, "hardware", _("Hardware"));
    GtkWidget *pg_other = settings_stack_page(app, "other", _("Other laptops"));
    app->settings_other_page = gtk_widget_get_parent(gtk_widget_get_parent(pg_other));   /* the stack child */

    GtkWidget *frame = gtk_frame_new(_("Components"));
    GtkWidget *grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    const char *titles[COMP_COUNT] = { _("Fan control"), _("CPU control"), _("Keyboard backlight") };
    const char *texts[COMP_COUNT] = {
        /* xgettext:no-c-format */
        _("Fan daemon, Status and Fan Profiles tabs, fan profiles in the tray menu. Switched off, the fans run at "
        "100% until the next reboot; after it the laptop's firmware controls them."),
        _("CPU daemon and hard-limit timer, CPU Profiles tab, CPU profiles in the tray menu. Governor and "
        "frequencies stay as they were last set."),
        _("Keyboard daemon, Keyboard Backlight tab, keyboard profiles in the tray menu. The LEDs keep their last colors."),
    };
    /* Alphabetical like the tabs and the tray menu: CPU, Fan, Keyboard */
    static const int row_order[COMP_COUNT] = { COMP_CPU, COMP_FAN, COMP_KEYBOARD };
    for (int row = 0; row < COMP_COUNT; row++) {
        int c = row_order[row];
        GtkWidget *sw = gtk_switch_new();
        gtk_widget_set_valign(sw, GTK_ALIGN_START);
        g_object_set_data(G_OBJECT(sw), "component", GINT_TO_POINTER(c));
        app->settings_switch[c] = sw;
        app->settings_switch_sig[c] = g_signal_connect(sw, "state-set", G_CALLBACK(on_settings_switch), app);
        gtk_grid_attach(GTK_GRID(grid), sw, 0, row, 1, 1);

        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *title = gtk_label_new(NULL);
        gchar *m = g_markup_printf_escaped("<b>%s</b>", titles[c]);
        gtk_label_set_markup(GTK_LABEL(title), m);
        g_free(m);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        GtkWidget *text = gtk_label_new(texts[c]);
        gtk_label_set_xalign(GTK_LABEL(text), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(text), 90);
        gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0);
        gtk_widget_set_hexpand(box, TRUE);
        gtk_grid_attach(GTK_GRID(grid), box, 1, row, 1, 1);

        app->settings_state[c] = gtk_label_new("");
        gtk_widget_set_valign(app->settings_state[c], GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(app->settings_state[c]), 0.0);
        gtk_grid_attach(GTK_GRID(grid), app->settings_state[c], 2, row, 1, 1);
    }
    gtk_grid_attach(GTK_GRID(grid), settings_note(_("Switching asks for the administrator password and takes effect right away. "
                                                    "The setting applies to the whole system and stays after reboots and updates.")),
                    0, COMP_COUNT, 3, 1);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(pg_components), frame, FALSE, FALSE, 0);

    /* Other laptops (1.49.0): generic fan control and direct EC access on
     * their own page, so nobody switches them on by the way */
    GtkWidget *other_intro = gtk_label_new(_("Only for laptops without the Clevo interface (no dcc_fan module). On Clevo "
                                             "laptops with the module, nothing here is needed."));
    gtk_label_set_xalign(GTK_LABEL(other_intro), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(other_intro), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(other_intro), 110);
    gtk_box_pack_start(GTK_BOX(pg_other), other_intro, FALSE, FALSE, 0);
    frame = gtk_frame_new(_("Fan control"));
    grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    /* Generic fan control (1.43.0): off by default, short warning */
    {
        app->settings_hw_switch = gtk_switch_new();
        gtk_widget_set_valign(app->settings_hw_switch, GTK_ALIGN_START);
        app->settings_hw_sig = g_signal_connect(app->settings_hw_switch, "state-set", G_CALLBACK(on_settings_hw_switch), app);
        gtk_grid_attach(GTK_GRID(grid), app->settings_hw_switch, 0, 0, 1, 1);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *title = gtk_label_new(NULL);
        gchar *m = g_markup_printf_escaped("<b>%s</b>", _("Generic fan control"));
        gtk_label_set_markup(GTK_LABEL(title), m);
        g_free(m);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        GtkWidget *text = gtk_label_new(_("For laptops without the Clevo interface whose fan driver offers a manual mode "
                                          "(for example ThinkPad, Dell): the fan daemon sets the fan speed through the "
                                          "kernel's standard interface. Off: the driver controls the fans."));
        gtk_label_set_xalign(GTK_LABEL(text), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(text), 90);
        gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0);
        gtk_widget_set_hexpand(box, TRUE);
        gtk_grid_attach(GTK_GRID(grid), box, 1, 0, 1, 1);
        app->settings_hw_state = gtk_label_new("");
        gtk_widget_set_valign(app->settings_hw_state, GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(app->settings_hw_state), 0.0);
        gtk_grid_attach(GTK_GRID(grid), app->settings_hw_state, 2, 0, 1, 1);
    }
    /* Direct EC access (1.41.0): off by default, with a warning */
    {
        app->settings_ec_switch = gtk_switch_new();
        gtk_widget_set_valign(app->settings_ec_switch, GTK_ALIGN_START);
        app->settings_ec_sig = g_signal_connect(app->settings_ec_switch, "state-set", G_CALLBACK(on_settings_ec_switch), app);
        gtk_grid_attach(GTK_GRID(grid), app->settings_ec_switch, 0, 1, 1, 1);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *title = gtk_label_new(NULL);
        gchar *m = g_markup_printf_escaped("<b>%s</b>", _("Direct EC access (unsupported hardware)"));
        gtk_label_set_markup(GTK_LABEL(title), m);
        g_free(m);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        GtkWidget *text = gtk_label_new(_("Only for Clevo-based laptops where the dcc_fan module does not load. The fan daemon "
                                          "then writes directly to the Embedded Controller; on other laptops this can damage "
                                          "the hardware. Off: without the module, the firmware controls the fans."));
        gtk_label_set_xalign(GTK_LABEL(text), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(text), 90);
        gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0);
        gtk_widget_set_hexpand(box, TRUE);
        gtk_grid_attach(GTK_GRID(grid), box, 1, 1, 1, 1);
        app->settings_ec_state = gtk_label_new("");
        gtk_widget_set_valign(app->settings_ec_state, GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(app->settings_ec_state), 0.0);
        gtk_grid_attach(GTK_GRID(grid), app->settings_ec_state, 2, 1, 1, 1);
    }

    /* The note belongs to the switches above (1.40.1: was at the bottom of the page) */
    gtk_grid_attach(GTK_GRID(grid), settings_note(_("Switching asks for the administrator password and takes effect right away. "
                                                    "The setting applies to the whole system and stays after reboots and updates.")),
                    0, 2, 3, 1);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(pg_other), frame, FALSE, FALSE, 0);

    /* Language & Display (1.40.1): language (1.31.0), keyboard layout
     * (1.28.0) and dark/light mode (1.29.0) in one grid, labels aligned */
    {
        GtkWidget *dframe = gtk_frame_new(_("Language & Display"));   /* General page */
        GtkWidget *g = settings_grid();
        int r = 0;

        GtkWidget *lrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        app->settings_language_combo = gtk_combo_box_text_new();
        for (int i = 0; i < UI_NUM_LANGUAGES; i++)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->settings_language_combo), ui_languages[i].code,
                                      i == 0 ? _(ui_languages[i].label) : ui_languages[i].label);
        {
            cJSON *json = config_read();
            cJSON *v = cJSON_GetObjectItem(json, "language");
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->settings_language_combo),
                                        cJSON_IsString(v) ? v->valuestring : "system");
            if (gtk_combo_box_get_active(GTK_COMBO_BOX(app->settings_language_combo)) < 0)
                gtk_combo_box_set_active(GTK_COMBO_BOX(app->settings_language_combo), 0);
            cJSON_Delete(json);
        }
        g_signal_connect(app->settings_language_combo, "changed", G_CALLBACK(on_settings_language_changed), app);
        gtk_box_pack_start(GTK_BOX(lrow), app->settings_language_combo, FALSE, FALSE, 0);
        app->settings_restart_btn = gtk_button_new_with_label(_("Restart now"));
        g_signal_connect(app->settings_restart_btn, "clicked", G_CALLBACK(on_settings_restart_clicked), app);
        gtk_box_pack_start(GTK_BOX(lrow), app->settings_restart_btn, FALSE, FALSE, 0);
        app->settings_language_note = settings_note("");
        settings_grid_row(g, &r, _("Language:"), lrow, app->settings_language_note);

        app->settings_appearance_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->settings_appearance_combo), "system", _("Follow the system"));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->settings_appearance_combo), "dark", _("Dark"));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->settings_appearance_combo), "light", _("Light"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->settings_appearance_combo), app->appearance);
        g_signal_connect(app->settings_appearance_combo, "changed", G_CALLBACK(on_settings_appearance_changed), app);
        app->settings_appearance_note = settings_note("");
        settings_grid_row(g, &r, _("Mode:"), app->settings_appearance_combo, app->settings_appearance_note);

        gtk_container_add(GTK_CONTAINER(dframe), g);
        gtk_box_pack_start(GTK_BOX(pg_general), dframe, FALSE, FALSE, 0);
        settings_update_language_note(app);
        settings_update_appearance_note(app);
    }

    /* Behavior (1.40.1): start minimized and refresh interval (1.40.0) */
    {
        GtkWidget *bframe = gtk_frame_new(_("Behavior"));
        GtkWidget *g = settings_grid();
        int r = 0;

        app->settings_minimized_switch = gtk_switch_new();
        gtk_widget_set_halign(app->settings_minimized_switch, GTK_ALIGN_START);
        gtk_switch_set_active(GTK_SWITCH(app->settings_minimized_switch), start_minimized_load());
        g_signal_connect(app->settings_minimized_switch, "state-set", G_CALLBACK(on_settings_minimized_set), app);
        settings_grid_row(g, &r, _("Start minimized to tray:"), app->settings_minimized_switch,
                          settings_note_markup(_("On: the GUI starts as a tray icon; off: it opens its window. The menu entry "
                                                 "and your startup applications get the option <tt>--minimized</tt> to match. "
                                                 "Starting it again while it runs opens its window.")));

        app->settings_refresh_combo = gtk_combo_box_text_new();
        if (app->refresh_sec <= 0) app->refresh_sec = refresh_sec_load();
        for (int i = 0; i < REFRESH_NUM_CHOICES; i++) {
            gchar *t = refresh_choices[i] == 1 ? g_strdup(_("1 second"))
                                               : g_strdup_printf(_("%d seconds"), refresh_choices[i]);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->settings_refresh_combo), t);
            g_free(t);
            if (refresh_choices[i] == app->refresh_sec)
                gtk_combo_box_set_active(GTK_COMBO_BOX(app->settings_refresh_combo), i);
        }
        g_signal_connect(app->settings_refresh_combo, "changed", G_CALLBACK(on_settings_refresh_changed), app);
        settings_grid_row(g, &r, _("Refresh interval:"), app->settings_refresh_combo,
                          settings_note(_("How often the window shows new temperatures, fan speeds, frequencies and "
                                          "power. The daemons control fans and CPU on their own schedule.")));

        gtk_container_add(GTK_CONTAINER(bframe), g);
        gtk_box_pack_start(GTK_BOX(pg_general), bframe, FALSE, FALSE, 0);
    }

    /* Battery (1.46.0): charge limit through the CPU daemon (Hardware page) */
    {
        GtkWidget *bframe = gtk_frame_new(_("Battery"));
        GtkWidget *g = settings_grid();
        int r = 0;
        app->settings_bat_combo = gtk_combo_box_text_new();
        for (size_t i = 0; i < G_N_ELEMENTS(bat_limit_choices); i++) {
            gchar *id = g_strdup_printf("%d", bat_limit_choices[i]);
            gchar *t = bat_limit_choices[i] >= 100 ? g_strdup(_("Off (charges to 100 %)"))
                                                   : g_strdup_printf(_("%d %%"), bat_limit_choices[i]);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->settings_bat_combo), id, t);
            g_free(id);
            g_free(t);
        }
        gtk_widget_set_sensitive(app->settings_bat_combo, FALSE);
        g_signal_connect(app->settings_bat_combo, "changed", G_CALLBACK(on_settings_bat_changed), app);
        app->settings_bat_note = settings_note("");   /* filled by settings_update_battery() */
        settings_grid_row(g, &r, _("Charge limit:"), app->settings_bat_combo, app->settings_bat_note);
        gtk_container_add(GTK_CONTAINER(bframe), g);
        gtk_box_pack_start(GTK_BOX(pg_hardware), bframe, FALSE, FALSE, 0);
        if (!app->bat_limit_seen) app->bat_limit = -2;
        settings_update_battery(app);
    }

    /* Keyboard layout (1.28.0): describes the keyboard, so on the Hardware page (1.49.0) */
    {
        GtkWidget *kframe = gtk_frame_new(_("Keyboard"));
        GtkWidget *g = settings_grid();
        int r = 0;
        app->settings_layout_combo = gtk_combo_box_text_new();
        for (int i = 0; i < KBD_NUM_LAYOUTS; i++)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->settings_layout_combo), kbd_layouts[i].code, _(kbd_layouts[i].label));
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->settings_layout_combo), kbd_layout_code);
        g_signal_connect(app->settings_layout_combo, "changed", G_CALLBACK(on_settings_layout_changed), app);
        app->settings_layout_note = settings_note("");
        settings_grid_row(g, &r, _("Keyboard layout:"), app->settings_layout_combo, app->settings_layout_note);
        gtk_container_add(GTK_CONTAINER(kframe), g);
        gtk_box_pack_start(GTK_BOX(pg_hardware), kframe, FALSE, FALSE, 0);
        settings_update_layout_note(app);
    }

    /* sidebar | pages; the page last shown comes back */
    GtkWidget *sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(app->settings_stack));
    gtk_widget_set_size_request(sidebar, 170, -1);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(hbox), sidebar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), app->settings_stack, TRUE, TRUE, 0);
    gtk_widget_show_all(hbox);                       /* a stack shows only visible children */
    {
        cJSON *json = config_read();
        cJSON *v = cJSON_GetObjectItem(json, "settings_page");
        if (cJSON_IsString(v) && gtk_stack_get_child_by_name(GTK_STACK(app->settings_stack), v->valuestring))
            gtk_stack_set_visible_child_name(GTK_STACK(app->settings_stack), v->valuestring);
        cJSON_Delete(json);
    }
    g_signal_connect(app->settings_stack, "notify::visible-child-name", G_CALLBACK(on_settings_page_changed), app);

    settings_sync_switches(app);
    return hbox;
}
