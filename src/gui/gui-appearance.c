// SPDX-License-Identifier: GPL-2.0-only
/*
 * gui-appearance.c
 * Appearance: follow the system's theme, or dark / light for this window.
 * Part of dcc-gui.c, which #includes it: one translation unit, so the
 * static functions and AppData stay shared (split up in 1.48.1).
 */


/* ============================================================
 * Appearance (1.29.0): follow the system, or dark / light for this app.
 * Dark and light switch the GTK theme of this process only: to the theme's
 * dark or light variant if one is installed (Mint-Y-Aqua <-> Mint-Y-Dark-Aqua),
 * otherwise through gtk-application-prefer-dark-theme (themes with a
 * gtk-dark.css, e.g. Adwaita). "Follow the system" gives the desktop's
 * settings back. Whether the window ends up dark is read from the theme's
 * background color; the keyboard picture and the fan curve graph follow it.
 * ============================================================ */

/* Names a theme's dark (dark=1) or light variant could have, best first;
 * returns how many were written (0: the name already is that variant) */
static int theme_variant_candidates(const char *name, int dark, char out[4][128]) {
    int n = 0;
    if (!name || !*name) return 0;
    int is_dark = strcasestr(name, "dark") != NULL;
    if (dark) {
        if (is_dark) return 0;
        /* Mint-Y-Aqua -> Mint-Y-Dark-Aqua: after the second part */
        const char *d1 = strchr(name, '-'), *d2 = d1 ? strchr(d1 + 1, '-') : NULL;
        if (d2) snprintf(out[n++], 128, "%.*s-Dark%s", (int)(d2 - name), name, d2);
        snprintf(out[n++], 128, "%s-Dark", name);
        snprintf(out[n++], 128, "%s-dark", name);
    } else {
        if (!is_dark) return 0;
        const char *tags[] = { "-Darker", "-Dark", "-dark" };
        for (int t = 0; t < 3 && n < 4; t++) {
            const char *pos = strstr(name, tags[t]);
            if (!pos) continue;
            snprintf(out[n++], 128, "%.*s%s", (int)(pos - name), name, pos + strlen(tags[t]));
        }
    }
    return n;
}

static int theme_dir_exists(const char *base, const char *sub, const char *name) {
    gchar *path = g_build_filename(base, sub, name, "gtk-3.0", NULL);
    int ok = g_file_test(path, G_FILE_TEST_IS_DIR);
    g_free(path);
    return ok;
}

static int theme_installed(const char *name) {
    if (!name || !*name || strchr(name, '/')) return 0;
    if (theme_dir_exists(g_get_user_data_dir(), "themes", name)) return 1;
    if (theme_dir_exists(g_get_home_dir(), ".themes", name)) return 1;
    for (const gchar *const *d = g_get_system_data_dirs(); *d; d++)
        if (theme_dir_exists(*d, "themes", name)) return 1;
    return 0;
}

/* The desktop's theme setting, read directly from its settings (Cinnamon,
 * otherwise GNOME): GTK's own theme setting did not follow a theme change
 * made in the desktop's settings while the app was running. g_free it. */
static GSettings *appearance_desktop_settings(const char **key) {
    static GSettings *gs = NULL;
    static const char *found_key = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        const char *schemas[] = { "org.cinnamon.desktop.interface", "org.gnome.desktop.interface" };
        GSettingsSchemaSource *src = g_settings_schema_source_get_default();
        for (int i = 0; i < 2 && !gs && src; i++) {
            GSettingsSchema *schema = g_settings_schema_source_lookup(src, schemas[i], TRUE);
            if (!schema) continue;
            if (g_settings_schema_has_key(schema, "gtk-theme")) {
                gs = g_settings_new(schemas[i]);
                found_key = "gtk-theme";
            }
            g_settings_schema_unref(schema);
        }
    }
    if (key) *key = found_key;
    return gs;
}

static gchar *appearance_desktop_theme(void) {
    const char *key = NULL;
    GSettings *gs = appearance_desktop_settings(&key);
    if (!gs || !key) return NULL;
    gchar *t = g_settings_get_string(gs, key);
    if (t && !*t) { g_free(t); t = NULL; }
    return t;
}

/* Set the GTK theme of this app for the chosen appearance */
static void appearance_apply_theme(int mode) {
    GtkSettings *gs = gtk_settings_get_default();
    if (!gs) return;
    /* back to the desktop's values first (also the base for the variants) */
    gtk_settings_reset_property(gs, "gtk-theme-name");
    gtk_settings_reset_property(gs, "gtk-application-prefer-dark-theme");
    gchar *theme = appearance_desktop_theme();
    if (!theme) g_object_get(gs, "gtk-theme-name", &theme, NULL);
    if (mode == APPEARANCE_SYSTEM) {
        /* the desktop's theme as set now (GTK may still have an old one) */
        gchar *cur = NULL;
        g_object_get(gs, "gtk-theme-name", &cur, NULL);
        if (theme && (!cur || strcmp(cur, theme) != 0)) {
            g_object_set(gs, "gtk-theme-name", theme, NULL);
            gui_event("[GUI] Appearance system: desktop theme %s\n", theme);
        }
        g_free(cur);
        g_free(theme);
        return;
    }
    char cand[4][128];
    int n = theme_variant_candidates(theme, mode == APPEARANCE_DARK, cand);
    for (int i = 0; i < n; i++)
        if (theme_installed(cand[i])) {
            g_object_set(gs, "gtk-theme-name", cand[i], NULL);
            gui_event("[GUI] Appearance %s: theme %s instead of %s\n", appearance_names[mode], cand[i], theme ? theme : "?");
            break;
        }
    g_object_set(gs, "gtk-application-prefer-dark-theme", mode == APPEARANCE_DARK, NULL);
    g_free(theme);
}

/* Is the theme dark? Its window background color decides */
static int appearance_detect_dark(GtkWidget *widget) {
    GtkWidget *tmp = NULL;
    if (!widget) widget = tmp = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GdkRGBA c;
    int dark = 0;
    if (gtk_style_context_lookup_color(gtk_widget_get_style_context(widget), "theme_bg_color", &c))
        dark = 0.299 * c.red + 0.587 * c.green + 0.114 * c.blue < 0.5;
    else {
        gchar *theme = NULL;
        gboolean prefer = FALSE;
        g_object_get(gtk_settings_get_default(), "gtk-theme-name", &theme, "gtk-application-prefer-dark-theme", &prefer, NULL);
        dark = prefer || (theme && strcasestr(theme, "dark"));
        g_free(theme);
    }
    if (tmp) gtk_widget_destroy(tmp);
    return dark;
}

static void settings_update_appearance_note(AppData *app);

/* After a theme change: picture, graph and notes in the new colors */
static void appearance_refresh(AppData *app) {
    int dark = appearance_detect_dark(app->window);
    if (dark != app->is_dark) {
        app->is_dark = dark;
        g_print("[GUI] Appearance: window is %s\n", dark ? "dark" : "light");
        kbd_load_layout_picture(app);
        if (app->curve_graph) gtk_widget_queue_draw(app->curve_graph);
        about_logo_set(dark);
    }
    settings_update_appearance_note(app);
}

static gboolean appearance_refresh_idle(gpointer user_data) {
    appearance_refresh((AppData *)user_data);
    return G_SOURCE_REMOVE;
}

/* The desktop theme changed (matters while following the system) */
static void on_gtk_theme_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)obj; (void)pspec;
    g_idle_add(appearance_refresh_idle, user_data);
}

/* The theme was changed in the desktop's settings: follow it (or, with a
 * fixed dark/light mode, use the new theme's matching variant) */
static void on_desktop_theme_changed(GSettings *gs, const gchar *key, gpointer user_data) {
    (void)gs;
    AppData *app = (AppData *)user_data;
    gchar *t = appearance_desktop_theme();
    gui_event("[GUI] Desktop theme changed (%s): %s\n", key, t ? t : "?");
    g_free(t);
    appearance_apply_theme(app->appearance);
    g_idle_add(appearance_refresh_idle, app);
}

/* Appearance at start: config.json "appearance", default: follow the system */
static void appearance_init(AppData *app) {
    app->appearance = APPEARANCE_SYSTEM;
    cJSON *json = config_read();
    cJSON *v = cJSON_GetObjectItem(json, "appearance");
    if (cJSON_IsString(v))
        for (int i = 0; i < 3; i++) if (!strcmp(v->valuestring, appearance_names[i])) app->appearance = i;
    cJSON_Delete(json);
    appearance_apply_theme(app->appearance);
    app->is_dark = appearance_detect_dark(NULL);
    about_logo_set(app->is_dark);   /* before the About page is built */
    const char *key = NULL;
    GSettings *desk = appearance_desktop_settings(&key);
    if (desk && key) {
        gchar *sig = g_strdup_printf("changed::%s", key);
        g_signal_connect(desk, sig, G_CALLBACK(on_desktop_theme_changed), app);
        g_free(sig);
    }
    gui_event("[GUI] Appearance: %s, window is %s\n", appearance_names[app->appearance], app->is_dark ? "dark" : "light");
}

static void appearance_save_choice(int mode) {
    cJSON *json = config_read();
    cJSON_DeleteItemFromObject(json, "appearance");
    cJSON_AddStringToObject(json, "appearance", appearance_names[mode]);
    config_write(json);
    cJSON_Delete(json);
}
