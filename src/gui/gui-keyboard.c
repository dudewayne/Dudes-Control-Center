// SPDX-License-Identifier: GPL-2.0-only
/*
 * gui-keyboard.c
 * Keyboard Backlight tab: profiles, zone editor, LED wizard, effects, brightness.
 * Part of dcc-gui.c, which #includes it: one translation unit, so the
 * static functions and AppData stay shared (split up in 1.48.1).
 */


/* Extract clean keyboard profile name from stored display string.
 * Stored format: "[*] ProfileName" or "    ProfileName"
 * Returns: pointer into display_name (not a new allocation). */
static const char *kbd_profile_name_from_display(const char *display_name) {
    const char *p = display_name;
    /* Skip leading whitespace */
    while (*p == ' ' || *p == '\t') p++;
    /* Skip "[*] " marker if present */
    if (strncmp(p, "[*] ", 4) == 0) p += 4;
    return p;
}

/* Reload the profile list and select select_name (NULL or not found: the
 * active profile) without running the selection handler */
static void load_keyboard_profiles_select(AppData *app, const char *select_name) {
    if (!kbd_ensure_connected(app)) {
        g_print("[GUI] Keyboard daemon not connected, skipping profile load\n");
        return;
    }

    const char *response = send_keyboard_command(app, "LIST_PROFILES");
    if (response == NULL || strncmp(response, "ERROR", 5) == 0) {
        g_print("[GUI] Failed to load keyboard profiles: %s\n", response ? response : "(null)");
        return;
    }

    if (app->kbd_profile_combo_signal_id)
        g_signal_handler_block(app->kbd_profile_combo, app->kbd_profile_combo_signal_id);
    gtk_list_store_clear(app->kbd_profile_store);
    int select_idx = -1;

    /* Parse response: PROFILES_START COUNT=N ... PROFILE i [*] Name ... PROFILES_END */
    char *resp_copy = g_strdup(response);
    char *line = strtok(resp_copy, "\n");
    int profile_count = 0;
    int active_idx = -1;

    while (line != NULL) {
        /* Skip header/footer lines */
        if (strncmp(line, "PROFILES_START", 14) == 0 || strncmp(line, "PROFILES_END", 12) == 0) {
            line = strtok(NULL, "\n");
            continue;
        }

        /* Parse PROFILE i [*] Name */
        int idx = -1;
        int is_active = 0;
        char name[256] = "";

        if (sscanf(line, "PROFILE %d [", &idx) >= 1) {
            char *marker = strstr(line, "[");
            if (marker) {
                is_active = (marker[1] == '*');
            }
            char *name_start = strstr(line, "] ");
            if (name_start) {
                strncpy(name, name_start + 2, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';
                g_strchomp(name);
            }
        }

        if (idx >= 0 && strlen(name) > 0) {
            GtkTreeIter iter;
            gtk_list_store_append(app->kbd_profile_store, &iter);
            /* Store display text with active marker, like CPU profiles */
            char display[256];
            snprintf(display, sizeof(display), "%s %s", is_active ? "[*]" : "   ", name);
            gtk_list_store_set(app->kbd_profile_store, &iter, 0, display, -1);
            if (is_active) {
                active_idx = idx;
            }
            if (select_name && strcmp(select_name, name) == 0) select_idx = idx;
            profile_count++;
        }

        line = strtok(NULL, "\n");
    }
    g_free(resp_copy);

    /* Select the requested profile, otherwise the active one */
    if (active_idx >= 0) app->kbd_active_profile_idx = active_idx;
    if (select_idx >= 0 || active_idx >= 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->kbd_profile_combo), select_idx >= 0 ? select_idx : active_idx);
    if (app->kbd_profile_combo_signal_id)
        g_signal_handler_unblock(app->kbd_profile_combo, app->kbd_profile_combo_signal_id);
    if (app->tray_menu) update_tray_menu(app);   /* Keyboard Profiles submenu (1.26.0) */

    update_daemon_status_labels(app);

    g_print("[GUI] Loaded %d keyboard profiles (active idx=%d)\n", profile_count, active_idx);
}

/* ============================================================
 * Keyboard zone editor (1.24.0)
 * A profile is a base color (keys in no zone) plus named zones. The GUI
 * edits a copy (kbd_edit) and sends it to the daemon as a preview after
 * every change, so the keyboard shows the profile being edited; Save
 * stores exactly that, Activate makes it the profile in use. Leaving the
 * Keyboard tab brings the active profile back.
 * ============================================================ */

/* Parse GET_PROFILE / GET_ZONES ("BASE color=r,g,b", "ZONE i name=.. color=r,g,b keys=N l1 l2 ..") */
static int kbd_edit_parse(KbdProfileEdit *e, const char *resp) {
    memset(e, 0, sizeof *e);
    if (!resp || (strncmp(resp, "PROFILE_START", 13) != 0 && strncmp(resp, "ZONES_START", 11) != 0))
        return -1;
    const char *line = resp;
    while (line && *line) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        char buf[2048];
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, line, len);
        buf[len] = '\0';
        unsigned r, g, b;
        char fxv[24] = "";
        const char *eff = strstr(buf, " effect=");
        if (eff) {
            size_t fl = strcspn(eff + 8, " ");
            if (fl >= sizeof fxv) fl = sizeof fxv - 1;
            memcpy(fxv, eff + 8, fl);
            fxv[fl] = '\0';
        }
        if (sscanf(buf, "BASE color=%u,%u,%u", &r, &g, &b) == 3) {
            e->has_base = 1;
            snprintf(e->base_fx, sizeof e->base_fx, "%s", fxv);
            e->base[0] = r > 255 ? 255 : r;
            e->base[1] = g > 255 ? 255 : g;
            e->base[2] = b > 255 ? 255 : b;
        } else if (strncmp(buf, "ZONE ", 5) == 0 && e->num_zones < KBD_GUI_MAX_ZONES) {
            char *nm = strstr(buf, " name="), *c = nm ? strstr(nm, " color=") : NULL;
            char *k = c ? strstr(c, " keys=") : NULL;
            int n = 0, used = 0;
            if (nm && c && k && sscanf(c + 7, "%u,%u,%u", &r, &g, &b) == 3 &&
                sscanf(k + 6, "%d%n", &n, &used) == 1) {
                KbdZoneEdit *z = &e->zones[e->num_zones];
                size_t nl = (size_t)(c - (nm + 6));
                if (nl >= sizeof z->name) nl = sizeof z->name - 1;
                memcpy(z->name, nm + 6, nl);
                z->name[nl] = '\0';
                z->rgb[0] = r > 255 ? 255 : r;
                z->rgb[1] = g > 255 ? 255 : g;
                z->rgb[2] = b > 255 ? 255 : b;
                snprintf(z->fx, sizeof z->fx, "%s", fxv);
                const char *q = k + 6 + used;
                for (int i = 0; i < n; i++) {
                    int led = -1, adv = 0;
                    if (sscanf(q, " %d%n", &led, &adv) != 1) break;
                    q += adv;
                    if (led >= 0 && led < 120 && z->n < 120) z->leds[z->n++] = led;
                }
                e->num_zones++;
            }
        }
        line = next ? next + 1 : NULL;
    }
    return 0;
}

/* Color per LED: base color, then the zones (a later zone wins) */
static void kbd_edit_colors(const KbdProfileEdit *e, unsigned char rgb[120][3], unsigned char has[120]) {
    memset(has, 0, 120);
    if (e->has_base)
        for (int i = 0; i < 120; i++) {
            memcpy(rgb[i], e->base, 3);
            has[i] = 1;
        }
    for (int z = 0; z < e->num_zones; z++)
        for (int k = 0; k < e->zones[z].n; k++) {
            int led = e->zones[z].leds[k];
            if (led < 0 || led >= 120) continue;
            memcpy(rgb[led], e->zones[z].rgb, 3);
            has[led] = 1;
        }
}

static int kbd_edit_equal(const KbdProfileEdit *a, const KbdProfileEdit *b) {
    if (a->has_base != b->has_base || a->num_zones != b->num_zones) return 0;
    if (a->has_base && (memcmp(a->base, b->base, 3) != 0 || strcmp(a->base_fx, b->base_fx) != 0)) return 0;
    for (int z = 0; z < a->num_zones; z++) {
        const KbdZoneEdit *x = &a->zones[z], *y = &b->zones[z];
        if (strcmp(x->name, y->name) != 0 || memcmp(x->rgb, y->rgb, 3) != 0 || x->n != y->n ||
            strcmp(x->fx, y->fx) != 0 ||
            memcmp(x->leds, y->leds, sizeof(int) * (size_t)x->n) != 0)
            return 0;
    }
    return 1;
}

/* "base::zones" for PREVIEW (base "r,g,b" or "-", zones "name|r,g,b|l,l;...") */
static void kbd_edit_encode(const KbdProfileEdit *e, GString *out) {
    if (e->has_base) {
        g_string_append_printf(out, "%u,%u,%u", e->base[0], e->base[1], e->base[2]);
        if (e->base_fx[0]) g_string_append_printf(out, "@%s", e->base_fx);
    } else {
        g_string_append(out, "-");
    }
    g_string_append(out, "::");
    for (int z = 0; z < e->num_zones; z++) {
        const KbdZoneEdit *zone = &e->zones[z];
        g_string_append_printf(out, "%s%s|%u,%u,%u%s%s|", z ? ";" : "", zone->name,
                               zone->rgb[0], zone->rgb[1], zone->rgb[2], zone->fx[0] ? "@" : "", zone->fx);
        for (int k = 0; k < zone->n; k++) g_string_append_printf(out, k ? ",%d" : "%d", zone->leds[k]);
    }
}

static int kbd_zone_has_led(const KbdZoneEdit *z, int led) {
    for (int k = 0; k < z->n; k++) if (z->leds[k] == led) return 1;
    return 0;
}

static void kbd_zone_remove_led(KbdZoneEdit *z, int led) {
    int w = 0;
    for (int k = 0; k < z->n; k++) if (z->leds[k] != led) z->leds[w++] = z->leds[k];
    z->n = w;
}

/* Click on a key while zone zi is edited: if all its LEDs are in the zone
 * they leave it (back to the base color, returns 0); otherwise they join it
 * and leave every other zone (returns 1). */
static int kbd_edit_toggle_leds(KbdProfileEdit *e, int zi, const int *leds, int n) {
    if (zi < 0 || zi >= e->num_zones || n <= 0) return -1;
    KbdZoneEdit *z = &e->zones[zi];
    int all_in = 1;
    for (int i = 0; i < n; i++) if (!kbd_zone_has_led(z, leds[i])) all_in = 0;
    if (all_in) {
        for (int i = 0; i < n; i++) kbd_zone_remove_led(z, leds[i]);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (leds[i] < 0 || leds[i] >= 120) continue;
        for (int o = 0; o < e->num_zones; o++) if (o != zi) kbd_zone_remove_led(&e->zones[o], leds[i]);
        if (!kbd_zone_has_led(z, leds[i]) && z->n < 120) z->leds[z->n++] = leds[i];
    }
    return 1;
}

/* Zone whose color an LED shows (the last zone containing it), -1: base */
static int kbd_edit_zone_at_led(const KbdProfileEdit *e, int led) {
    for (int z = e->num_zones - 1; z >= 0; z--)
        if (kbd_zone_has_led(&e->zones[z], led)) return z;
    return -1;
}

/* NULL if name can be used for zone except_idx (-1: a new zone), else why not */
static const char *kbd_zone_name_problem(const KbdProfileEdit *e, const char *name, int except_idx) {
    if (!name || !*name) return _("The name is empty.");
    if (strlen(name) >= sizeof e->zones[0].name) return _("The name is too long.");
    if (strpbrk(name, "|;\n") || strstr(name, "::")) return _("The name must not contain | ; or ::");
    for (int z = 0; z < e->num_zones; z++)
        if (z != except_idx && strcmp(e->zones[z].name, name) == 0) return _("A zone with this name already exists.");
    return NULL;
}

/* Name entered in a small dialog (g_free it), NULL if cancelled */
static gchar *kbd_ask_name(AppData *app, const char *title, const char *prompt, const char *initial,
                           const char *ok_label) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                    ok_label, GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);
    gtk_box_pack_start(GTK_BOX(content), gtk_label_new(prompt), FALSE, FALSE, 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), initial ? initial : "");
    gtk_box_pack_start(GTK_BOX(content), entry, TRUE, TRUE, 0);
    gtk_widget_show_all(content);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), dialog);
    gchar *name = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
        name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
    gtk_widget_destroy(dialog);
    return name;
}

static void kbd_message(AppData *app, GtkMessageType type, const char *text) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          type, GTK_BUTTONS_OK, _("%s"), text);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static int kbd_confirm(AppData *app, const char *title, const char *text) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, _("%s"), text);
    gtk_window_set_title(GTK_WINDOW(d), title);
    int yes = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_YES;
    gtk_widget_destroy(d);
    return yes;
}

static void kbd_status(AppData *app, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void kbd_status(AppData *app, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (app->kbd_profile_status_label) gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label), msg);
    gui_event("[GUI] Keyboard: %s\n", msg);
    g_free(msg);
}

/* Name of the profile selected in the dropdown (static buffer), NULL if none */
static const char *kbd_selected_profile_name(AppData *app, int *is_active) {
    static char name[256];
    GtkTreeIter iter;
    if (is_active) *is_active = 0;
    if (!gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->kbd_profile_combo), &iter)) return NULL;
    gchar *display = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->kbd_profile_store), &iter, 0, &display, -1);
    if (!display) return NULL;
    if (is_active) *is_active = strncmp(display, "[*]", 3) == 0;
    snprintf(name, sizeof name, "%s", kbd_profile_name_from_display(display));
    g_free(display);
    return name;
}

static int kbd_profile_exists(AppData *app, const char *name, int *is_active) {
    GtkTreeIter iter;
    if (is_active) *is_active = 0;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->kbd_profile_store), &iter)) return 0;
    do {
        gchar *display = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->kbd_profile_store), &iter, 0, &display, -1);
        int found = display && strcmp(kbd_profile_name_from_display(display), name) == 0;
        if (found && is_active) *is_active = strncmp(display, "[*]", 3) == 0;
        g_free(display);
        if (found) return 1;
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->kbd_profile_store), &iter));
    return 0;
}

/* Keys of the zone being edited are highlighted on the picture */
static void kbd_highlight_zone(AppData *app) {
    app->kbd_num_selected_leds = 0;
    int zi = app->kbd_sel_zone;
    if (zi < 0 || zi >= app->kbd_edit.num_zones) return;
    const KbdZoneEdit *z = &app->kbd_edit.zones[zi];
    for (int k = 0; k < z->n && k < 120; k++) app->kbd_selected_led_indices[app->kbd_num_selected_leds++] = z->leds[k];
}

/* --- Effects (1.35.0): Effect, Speed and Direction for the selected zone,
 * or the base color with no zone selected; drawn by the keyboard daemon --- */
static const char *const kbd_fx_types[] = { "static", "breathing", "rainbow", "wave" };
static const char *const kbd_fx_dirs[] = { "right", "left", "down", "up" };

/* "wave:5:left" -> type index, speed, direction index */
static void kbd_fx_split(const char *fx, int *type, int *speed, int *dir) {
    *type = 0; *speed = 5; *dir = 0;
    if (!fx || !*fx) return;
    char buf[24];
    snprintf(buf, sizeof buf, "%s", fx);
    char *save = NULL, *t = strtok_r(buf, ":", &save), *sp = strtok_r(NULL, ":", &save), *d = strtok_r(NULL, ":", &save);
    for (int i = 0; t && i < 4; i++) if (!strcmp(t, kbd_fx_types[i])) *type = i;
    if (sp) { int v = atoi(sp); if (v >= 1 && v <= 10) *speed = v; }
    for (int i = 0; d && i < 4; i++) if (!strcmp(d, kbd_fx_dirs[i])) *dir = i;
}

static void kbd_fx_join(int type, int speed, int dir, char *out, size_t size) {
    if (type <= 0 || type > 3) out[0] = '\0';
    else if (type == 3) snprintf(out, size, "wave:%d:%s", speed, kbd_fx_dirs[dir < 0 || dir > 3 ? 0 : dir]);
    else snprintf(out, size, "%s:%d", kbd_fx_types[type], speed);
}

static char *kbd_fx_target(AppData *app) {
    KbdProfileEdit *e = &app->kbd_edit;
    int zi = app->kbd_sel_zone;
    return (zi >= 0 && zi < e->num_zones) ? e->zones[zi].fx : e->base_fx;
}

static void kbd_fx_sync_controls(AppData *app) {
    if (!app->kbd_fx_combo) return;
    int type, speed, dir;
    kbd_fx_split(kbd_fx_target(app), &type, &speed, &dir);
    app->kbd_fx_syncing = 1;
    /* without Wave in the list (no per-key LEDs, 1.44.1) a Wave shows as
     * Rainbow, which is what it looks like there */
    int n = gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(app->kbd_fx_combo)), NULL);
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->kbd_fx_combo), type < n ? type : n - 1);
    gtk_range_set_value(GTK_RANGE(app->kbd_fx_speed), speed);
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->kbd_fx_dir), dir);
    app->kbd_fx_syncing = 0;
    gtk_widget_set_sensitive(app->kbd_fx_speed, type != 0);
    gtk_widget_set_sensitive(app->kbd_fx_dir, type == 3);
}

static void on_kbd_fx_changed(GtkWidget *w, gpointer user_data) {
    (void)w;
    AppData *app = (AppData *)user_data;
    if (app->kbd_fx_syncing) return;
    int type = gtk_combo_box_get_active(GTK_COMBO_BOX(app->kbd_fx_combo));
    int speed = (int)gtk_range_get_value(GTK_RANGE(app->kbd_fx_speed));
    int dir = gtk_combo_box_get_active(GTK_COMBO_BOX(app->kbd_fx_dir));
    gtk_widget_set_sensitive(app->kbd_fx_speed, type > 0);
    gtk_widget_set_sensitive(app->kbd_fx_dir, type == 3);
    if (!kbd_ensure_connected(app)) { kbd_status(app, _("Keyboard daemon not connected")); return; }
    if (!app->kbd_edit_loaded) {
        kbd_status(app, _("Select or create a profile first"));
        kbd_fx_sync_controls(app);
        return;
    }
    KbdProfileEdit *e = &app->kbd_edit;
    int zi = app->kbd_sel_zone;
    int zone = zi >= 0 && zi < e->num_zones;
    if (!zone && !e->has_base) {
        /* an effect for the whole keyboard needs a base color to start from */
        e->has_base = 1;
        e->base[0] = e->base[1] = e->base[2] = 255;
    }
    kbd_fx_join(type, speed, dir, kbd_fx_target(app), sizeof e->base_fx);
    kbd_edit_changed(app, 1, 1);
    kbd_sync_power_switch(app);
    const char *what = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(app->kbd_fx_combo));
    gchar *label = g_strdup_printf(_("Effect %s"), what ? what : "");
    if (zone) kbd_status(app, _("%s: zone '%s' - click Save to keep it"), label, e->zones[zi].name);
    else kbd_status(app, _("%s: whole keyboard - click Save to keep it"), label);
    g_free(label);
    g_free((gchar *)what);
}

static void kbd_update_mode_label(AppData *app) {
    if (!app->kbd_mode_label) return;
    int zi = app->kbd_sel_zone;
    gchar *t;
    if (zi >= 0 && zi < app->kbd_edit.num_zones)
        t = g_strdup_printf(_("Coloring zone '%s' (%d LEDs): click keys on the picture or drag over an area to add or remove them."),
                            app->kbd_edit.zones[zi].name, app->kbd_edit.zones[zi].n);
    else
        t = g_strdup(_("Coloring the whole keyboard (base color; zones keep their colors). "
                     "Select a zone in the list to color its keys."));
    gtk_label_set_text(GTK_LABEL(app->kbd_mode_label), t);
    g_free(t);
    kbd_fx_sync_controls(app);
}

/* Zone list = kbd_edit's zones; selects kbd_sel_zone */
static void kbd_zone_list_refresh(AppData *app) {
    if (!app->kbd_zone_store) return;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->kbd_zone_view));
    g_signal_handler_block(sel, app->kbd_zone_sel_signal_id);
    gtk_list_store_clear(app->kbd_zone_store);
    for (int z = 0; z < app->kbd_edit.num_zones; z++) {
        const KbdZoneEdit *zone = &app->kbd_edit.zones[z];
        gchar *swatch = g_strdup_printf("<span foreground=\"#%02X%02X%02X\">███</span>",
                                        zone->rgb[0], zone->rgb[1], zone->rgb[2]);
        GtkTreeIter it;
        gtk_list_store_insert_with_values(app->kbd_zone_store, &it, -1, 0, swatch, 1, zone->name, 2, zone->n, -1);
        g_free(swatch);
    }
    if (app->kbd_sel_zone >= app->kbd_edit.num_zones) app->kbd_sel_zone = -1;
    if (app->kbd_sel_zone >= 0) {
        GtkTreePath *path = gtk_tree_path_new_from_indices(app->kbd_sel_zone, -1);
        gtk_tree_selection_select_path(sel, path);
        gtk_tree_path_free(path);
    }
    g_signal_handler_unblock(sel, app->kbd_zone_sel_signal_id);
    int has = app->kbd_sel_zone >= 0;
    if (app->kbd_zone_rename_btn) gtk_widget_set_sensitive(app->kbd_zone_rename_btn, has);
    if (app->kbd_zone_delete_btn) gtk_widget_set_sensitive(app->kbd_zone_delete_btn, has);
}

/* Show kbd_edit on the keyboard; light_on: a color was chosen (switches the light on) */
static void kbd_send_preview(AppData *app, int light_on) {
    if (!app->kbd_edit_loaded) return;
    GString *cmd = g_string_new("PREVIEW::");
    kbd_edit_encode(&app->kbd_edit, cmd);
    if (light_on) g_string_append(cmd, "::on");
    const char *r = send_keyboard_command(app, cmd->str);
    if (!r || strncmp(r, "OK", 2) != 0) g_print("[GUI] PREVIEW failed: %s\n", r ? r : "NULL");
    g_string_free(cmd, TRUE);
}

/* Everything that shows kbd_edit, after a change */
static void kbd_edit_changed(AppData *app, int send_preview, int light_on) {
    kbd_edit_colors(&app->kbd_edit, app->kbd_led_rgb, app->kbd_led_has_color);
    kbd_zone_list_refresh(app);
    kbd_highlight_zone(app);
    kbd_update_mode_label(app);
    if (send_preview) kbd_send_preview(app, light_on);
    kbd_redraw_canvas(app);
}

/* kbd_edit = kbd_saved = the saved profile name */
static int kbd_load_edit_profile(AppData *app, const char *name) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "GET_PROFILE::%s", name);
    const char *r = send_keyboard_command(app, cmd);
    if (kbd_edit_parse(&app->kbd_edit, r) != 0) {
        gui_event("[GUI] Could not read keyboard profile '%s': %s\n", name, r ? r : "NULL");
        return -1;
    }
    app->kbd_saved = app->kbd_edit;
    snprintf(app->kbd_edit_name, sizeof app->kbd_edit_name, "%s", name);
    app->kbd_edit_loaded = 1;
    app->kbd_sel_zone = -1;
    return 0;
}

static int kbd_unsaved(const AppData *app) {
    return app->kbd_edit_loaded && !kbd_edit_equal(&app->kbd_edit, &app->kbd_saved);
}

/* Show profile name: the active one as it is, another one as a preview */
static void kbd_show_profile(AppData *app, const char *name, int is_active) {
    if (kbd_load_edit_profile(app, name) != 0) return;
    if (is_active) send_keyboard_command(app, "RESTORE_ACTIVE");
    kbd_edit_changed(app, !is_active, 0);
    kbd_sync_power_switch(app);
}

/* Save kbd_edit into its profile (the one being edited, not the active one) */
static int kbd_save_edit(AppData *app) {
    if (!app->kbd_edit_loaded || !kbd_ensure_connected(app)) return -1;
    kbd_send_preview(app, 0);                     /* the daemon stores its live colors */
    char cmd[512];
    snprintf(cmd, sizeof cmd, "SAVE_PROFILE::%s", app->kbd_edit_name);
    const char *r = send_keyboard_command(app, cmd);
    if (!r || strncmp(r, "OK", 2) != 0) {
        kbd_status(app, _("Could not save '%s': %s"), app->kbd_edit_name, r ? r : "no response");
        return -1;
    }
    app->kbd_saved = app->kbd_edit;
    char name[256];
    snprintf(name, sizeof name, "%s", app->kbd_edit_name);
    load_keyboard_profiles_select(app, name);     /* stays on the profile just saved */
    kbd_status(app, _("Profile saved: %s"), name);
    return 0;
}

static int kbd_check_unsaved_changes(AppData *app) {
    if (!kbd_unsaved(app)) return 1;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, NULL);
    gchar *markup = g_markup_printf_escaped(
        _("<b>Warning:</b> You have unsaved changes to the keyboard profile '%s'.\n\nWhat would you like to do?"),
        app->kbd_edit_name);
    gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(dialog), markup);
    g_free(markup);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Cancel"), -102);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Discard Changes"), -100);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Save Changes"), -101);
    gtk_widget_show_all(dialog);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response == -101) return kbd_save_edit(app) == 0 ? 1 : -1;
    if (response == -100) {
        app->kbd_edit = app->kbd_saved;
        app->kbd_sel_zone = -1;
        kbd_edit_changed(app, 1, 0);
        kbd_status(app, _("Changes discarded"));
        return 0;
    }
    return -1;
}

/* Entering the Keyboard tab: edit the active profile */
static void kbd_enter_tab(AppData *app) {
    load_keyboard_profiles_select(app, NULL);
    int is_active = 0;
    const char *name = kbd_selected_profile_name(app, &is_active);
    if (!name) {
        app->kbd_edit_loaded = 0;
        memset(&app->kbd_edit, 0, sizeof app->kbd_edit);
        kbd_edit_changed(app, 0, 0);
        return;
    }
    if (kbd_load_edit_profile(app, name) == 0) kbd_edit_changed(app, 0, 0);
}

/* Leaving the Keyboard tab (or closing the window): ask about unsaved
 * changes, then the keyboard shows the active profile again */
static int kbd_leave_tab(AppData *app) {
    if (!app->kbd_connected) return 0;
    if (kbd_check_unsaved_changes(app) < 0) return -1;
    send_keyboard_command(app, "RESTORE_ACTIVE");
    load_keyboard_profiles_select(app, NULL);
    int is_active = 0;
    const char *name = kbd_selected_profile_name(app, &is_active);
    if (name && kbd_load_edit_profile(app, name) == 0) kbd_edit_changed(app, 0, 0);
    kbd_sync_power_switch(app);
    return 0;
}

/* Dropdown: show the selected profile (a preview unless it is the active one) */
static void on_kbd_profile_changed(GtkComboBox *combo, gpointer user_data) {
    (void)combo;
    AppData *app = (AppData *)user_data;
    int is_active = 0;
    const char *sel = kbd_selected_profile_name(app, &is_active);
    if (!sel || !kbd_ensure_connected(app)) return;
    char name[256];
    snprintf(name, sizeof name, "%s", sel);
    if (app->kbd_edit_loaded && strcmp(name, app->kbd_edit_name) != 0 && kbd_unsaved(app)) {
        char old[256];
        snprintf(old, sizeof old, "%s", app->kbd_edit_name);
        if (kbd_check_unsaved_changes(app) < 0) {
            load_keyboard_profiles_select(app, old);      /* stay on the edited profile */
            return;
        }
        load_keyboard_profiles_select(app, name);
        kbd_profile_exists(app, name, &is_active);
    }
    kbd_show_profile(app, name, is_active);
    if (is_active) kbd_status(app, _("Active profile: %s"), name);
    else kbd_status(app, _("Preview: %s - Activate to use it, Save to keep changes"), name);
}

/* Callback: Create new keyboard profile */
static void on_kbd_create_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    if (!kbd_ensure_connected(app)) return;
    if (kbd_check_unsaved_changes(app) < 0) return;
    gchar *name = kbd_ask_name(app, _("Create Keyboard Profile"), _("Enter profile name:"), "", _("_Create"));
    if (!name) return;
    if (!*name || strstr(name, "::")) {
        kbd_message(app, GTK_MESSAGE_ERROR, _("Please enter a name (without ::)."));
    } else if (kbd_profile_exists(app, name, NULL)) {
        gchar *t = g_strdup_printf(_("A keyboard profile with the name '%s' already exists."), name);
        kbd_message(app, GTK_MESSAGE_ERROR, t);
        g_free(t);
    } else {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "CREATE_PROFILE::%s", name);
        const char *r = send_keyboard_command(app, cmd);
        if (r && strncmp(r, "OK", 2) == 0) {
            load_keyboard_profiles_select(app, name);
            kbd_show_profile(app, name, 0);           /* new profiles start empty */
            kbd_status(app, _("Profile '%s' created: pick a color for the whole keyboard or create zones"), name);
        } else {
            kbd_status(app, _("Could not create '%s': %s"), name, r ? r : "no response");
        }
    }
    g_free(name);
}

/* Callback: Delete keyboard profile */
static void on_kbd_delete_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    if (!kbd_ensure_connected(app)) return;
    const char *sel = kbd_selected_profile_name(app, NULL);
    if (!sel) { kbd_status(app, _("No profile selected")); return; }
    char name[256];
    snprintf(name, sizeof name, "%s", sel);
    gchar *q = g_strdup_printf(_("Are you sure you want to delete keyboard profile '%s'?"), name);
    int yes = kbd_confirm(app, _("Delete Profile"), q);
    g_free(q);
    if (!yes) return;
    char cmd[512];
    snprintf(cmd, sizeof cmd, "DELETE_PROFILE::%s", name);
    const char *r = send_keyboard_command(app, cmd);
    if (!r || strncmp(r, "OK", 2) != 0) {
        kbd_status(app, _("Could not delete '%s': %s"), name, r ? r : "no response");
        return;
    }
    /* back to the active profile (the one now at its place, if it was the active one) */
    app->kbd_edit_loaded = 0;
    send_keyboard_command(app, "RESTORE_ACTIVE");
    kbd_enter_tab(app);
    kbd_sync_power_switch(app);
    kbd_status(app, _("Profile deleted: %s"), name);
}

/* Callback: Rename keyboard profile */
static void on_kbd_rename_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    if (!kbd_ensure_connected(app)) return;
    const char *sel = kbd_selected_profile_name(app, NULL);
    if (!sel) { kbd_status(app, _("No profile selected")); return; }
    char old[256];
    snprintf(old, sizeof old, "%s", sel);
    gchar *name = kbd_ask_name(app, _("Rename Keyboard Profile"), _("Enter new profile name:"), old, _("_Rename"));
    if (!name) return;
    if (!*name || strstr(name, "::")) {
        kbd_message(app, GTK_MESSAGE_ERROR, _("Please enter a name (without ::)."));
    } else if (strcmp(name, old) != 0 && kbd_profile_exists(app, name, NULL)) {
        gchar *t = g_strdup_printf(_("A keyboard profile with the name '%s' already exists."), name);
        kbd_message(app, GTK_MESSAGE_ERROR, t);
        g_free(t);
    } else if (strcmp(name, old) != 0) {
        char cmd[600];
        snprintf(cmd, sizeof cmd, "RENAME_PROFILE::%s::%s", old, name);
        const char *r = send_keyboard_command(app, cmd);
        if (r && strncmp(r, "OK", 2) == 0) {
            if (strcmp(app->kbd_edit_name, old) == 0)
                snprintf(app->kbd_edit_name, sizeof app->kbd_edit_name, "%s", name);
            load_keyboard_profiles_select(app, name);
            kbd_status(app, _("Profile renamed: %s"), name);
        } else {
            kbd_status(app, _("Could not rename '%s': %s"), old, r ? r : "no response");
        }
    }
    g_free(name);
}

/* Callback: Save the profile being edited */
static void on_kbd_save_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    if (!app->kbd_edit_loaded) { kbd_status(app, _("No profile selected")); return; }
    kbd_save_edit(app);
}

/* Callback: Activate keyboard profile */
static void on_kbd_activate_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    if (!kbd_ensure_connected(app)) return;
    const char *sel = kbd_selected_profile_name(app, NULL);
    if (!sel) { kbd_status(app, _("No profile selected")); return; }
    char name[256];
    snprintf(name, sizeof name, "%s", sel);
    if (kbd_check_unsaved_changes(app) < 0) return;
    char cmd[512];
    snprintf(cmd, sizeof cmd, "LOAD_PROFILE::%s", name);
    const char *r = send_keyboard_command(app, cmd);
    if (!r || strncmp(r, "OK", 2) != 0) {
        kbd_status(app, _("Could not activate '%s': %s"), name, r ? r : "no response");
        return;
    }
    load_keyboard_profiles_select(app, name);
    if (kbd_load_edit_profile(app, name) == 0) kbd_edit_changed(app, 0, 0);
    kbd_sync_power_switch(app);
    kbd_status(app, _("Activated profile: %s"), name);
}

/* --- Zone list --- */
static void on_kbd_zone_selection_changed(GtkTreeSelection *sel, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    GtkTreeModel *model = NULL;
    GtkTreeIter it;
    app->kbd_sel_zone = -1;
    if (gtk_tree_selection_get_selected(sel, &model, &it)) {
        GtkTreePath *path = gtk_tree_model_get_path(model, &it);
        app->kbd_sel_zone = gtk_tree_path_get_indices(path)[0];
        gtk_tree_path_free(path);
    }
    int has = app->kbd_sel_zone >= 0;
    gtk_widget_set_sensitive(app->kbd_zone_rename_btn, has);
    gtk_widget_set_sensitive(app->kbd_zone_delete_btn, has);
    kbd_highlight_zone(app);
    kbd_update_mode_label(app);
    kbd_redraw_canvas(app);
}

static void on_kbd_whole_keyboard_clicked(GtkWidget *widget, AppData *app) {
    (void)widget;
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(app->kbd_zone_view)));
}

static void on_kbd_zone_create_clicked(GtkWidget *widget, AppData *app) {
    (void)widget;
    if (!app->kbd_edit_loaded) { kbd_status(app, _("Select a profile first")); return; }
    KbdProfileEdit *e = &app->kbd_edit;
    if (e->num_zones >= KBD_GUI_MAX_ZONES) {
        kbd_message(app, GTK_MESSAGE_ERROR, _("A profile can have at most 32 zones."));
        return;
    }
    char def[32];
    int n = e->num_zones + 1;
    do snprintf(def, sizeof def, _("Zone %d"), n++); while (kbd_zone_name_problem(e, def, -1));
    gchar *name = kbd_ask_name(app, _("Create Zone"), _("Enter zone name:"), def, _("_Create"));
    if (!name) return;
    const char *problem = kbd_zone_name_problem(e, name, -1);
    if (problem) {
        kbd_message(app, GTK_MESSAGE_ERROR, problem);
    } else {
        KbdZoneEdit *z = &e->zones[e->num_zones];
        memset(z, 0, sizeof *z);
        snprintf(z->name, sizeof z->name, "%s", name);
        z->rgb[0] = z->rgb[1] = z->rgb[2] = 255;
        app->kbd_sel_zone = e->num_zones++;
        kbd_edit_changed(app, 1, 0);
        kbd_status(app, _("Zone '%s' created: click keys to add them, then choose its color"), name);
    }
    g_free(name);
}

static void on_kbd_zone_rename_clicked(GtkWidget *widget, AppData *app) {
    (void)widget;
    int zi = app->kbd_sel_zone;
    if (zi < 0 || zi >= app->kbd_edit.num_zones) return;
    gchar *name = kbd_ask_name(app, _("Rename Zone"), _("Enter new zone name:"), app->kbd_edit.zones[zi].name, _("_Rename"));
    if (!name) return;
    const char *problem = kbd_zone_name_problem(&app->kbd_edit, name, zi);
    if (problem) kbd_message(app, GTK_MESSAGE_ERROR, problem);
    else {
        snprintf(app->kbd_edit.zones[zi].name, sizeof app->kbd_edit.zones[zi].name, "%s", name);
        kbd_edit_changed(app, 1, 0);
        kbd_status(app, _("Zone renamed: %s - click Save to keep it"), name);
    }
    g_free(name);
}

static void on_kbd_zone_delete_clicked(GtkWidget *widget, AppData *app) {
    (void)widget;
    KbdProfileEdit *e = &app->kbd_edit;
    int zi = app->kbd_sel_zone;
    if (zi < 0 || zi >= e->num_zones) return;
    gchar *q = g_strdup_printf(_("Delete zone '%s'? Its keys get the base color."), e->zones[zi].name);
    int yes = kbd_confirm(app, _("Delete Zone"), q);
    g_free(q);
    if (!yes) return;
    char name[64];
    snprintf(name, sizeof name, "%s", e->zones[zi].name);
    memmove(&e->zones[zi], &e->zones[zi + 1], sizeof e->zones[0] * (size_t)(e->num_zones - zi - 1));
    e->num_zones--;
    app->kbd_sel_zone = -1;
    kbd_edit_changed(app, 1, 0);
    kbd_status(app, _("Zone deleted: %s - click Save to keep it"), name);
}

static void on_kbd_zone_save_clicked(GtkWidget *widget, AppData *app) {
    (void)widget;
    on_kbd_save_profile_clicked(NULL, app);
}

/* A color (preset or custom) for the zone being edited, or the whole keyboard */
static void kbd_apply_color(AppData *app, int r, int g, int b, const char *what) {
    if (!kbd_ensure_connected(app)) { kbd_status(app, _("Keyboard daemon not connected")); return; }
    if (!app->kbd_edit_loaded) { kbd_status(app, _("Select or create a profile first")); return; }
    KbdProfileEdit *e = &app->kbd_edit;
    int zi = app->kbd_sel_zone;
    unsigned char *dst = (zi >= 0 && zi < e->num_zones) ? e->zones[zi].rgb : e->base;
    if (!(zi >= 0 && zi < e->num_zones)) e->has_base = 1;
    dst[0] = (unsigned char)r;
    dst[1] = (unsigned char)g;
    dst[2] = (unsigned char)b;
    kbd_edit_changed(app, 1, 1);
    kbd_sync_power_switch(app);
    if (zi >= 0 && zi < e->num_zones)
        kbd_status(app, _("%s: zone '%s' - click Save to keep it"), what, e->zones[zi].name);
    else
        kbd_status(app, _("%s: whole keyboard - click Save to keep it"), what);
}

/* Callback: Keyboard backlight off switch toggled */
static gboolean on_kbd_off_switch_toggled(GtkSwitch *sw, gboolean new_state, gpointer user_data) {
    (void)sw;
    AppData *app = (AppData *)user_data;
    gboolean is_on = new_state; /* Use new_state, gtk_switch_get_state() returns old state during signal */

    if (!kbd_ensure_connected(app)) {
        g_print("[GUI] Keyboard daemon not connected, skipping off switch toggle\n");
        return TRUE;  /* handled: keep the switch where it is */
    }

    if (is_on) {
        /* Turn off all LEDs */
        app->kbd_off_active = 1;
        const char *response = send_keyboard_command(app, "TURN_OFF");
        g_print("[GUI] TURN_OFF response: %s\n", response ? response : "NULL");
        gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label), _("Backlight turned OFF"));
        gui_event("[GUI] Keyboard backlight turned OFF (override active)\n");
    } else {
        /* Turn the backlight back on (the daemon restores the current colors) */
        app->kbd_off_active = 0;
        const char *response = send_keyboard_command(app, "TURN_ON");
        g_print("[GUI] TURN_ON response: %s\n", response ? response : "NULL");
        gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label), _("Backlight turned ON"));
        gui_event("[GUI] Keyboard backlight turned ON\n");
    }

    return FALSE; /* Let GTK update the switch visual state */
}

/* Redraw the keyboard canvas */
static void kbd_redraw_canvas(AppData *app) {
    if (app->kbd_keyboard_canvas) {
        gtk_widget_queue_draw(app->kbd_keyboard_canvas);
    }
}

/* LED Wizard: Start */
static void on_led_wizard_start_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
AppData *app = (AppData *)user_data;

    /* Initialize wizard state */
    app->kbd_wizard_active = 1;
    app->kbd_wizard_current_led = 0;
    /* Start from the existing mapping: each click reassigns only the LED
     * asked for, Skip keeps its key, No Key removes it, Cancel restores
     * everything. (It used to
     * clear the mapping, so Save & Finish right away wrote an empty file.) */
    memcpy(app->kbd_wizard_backup, app->kbd_wizard_mapping, sizeof(app->kbd_wizard_backup));

    /* Update UI */
    gtk_widget_set_sensitive(app->kbd_led_wizard_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_skip_btn, TRUE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_nokey_btn, TRUE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_save_btn, TRUE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_cancel_btn, TRUE);

    /* Show first step */
    kbd_wizard_step(app, NULL);
}

/* LED Wizard: Skip current LED */
static void on_led_wizard_skip_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
AppData *app = (AppData *)user_data;

    if (!app->kbd_wizard_active) return;

    /* Move to next LED */
    app->kbd_wizard_current_led++;
    if (app->kbd_wizard_current_led >= 120) {
        kbd_wizard_finish(app);
    } else {
        kbd_wizard_step(app, NULL);
    }
}

/* LED Wizard: the lit LED lights no key (unused LED) */
static void on_led_wizard_nokey_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;

    if (!app->kbd_wizard_active) return;
    app->kbd_wizard_mapping[app->kbd_wizard_current_led] = -1;
    app->kbd_wizard_current_led++;
    if (app->kbd_wizard_current_led >= 120)
        kbd_wizard_finish(app);
    else
        kbd_wizard_step(app, _("no key"));
}

/* LED Wizard: Save & Finish */
static void on_led_wizard_save_finish_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
AppData *app = (AppData *)user_data;

    app->kbd_wizard_active = 0;

    /* Restore all LEDs */
    send_keyboard_command(app, "TURN_ON");
    kbd_sync_power_switch(app);

    /* Save mapping to CSV */
    kbd_save_led_mapping_csv(app);
    settings_update_layout_note(app);

    /* Update UI */
    gtk_widget_set_sensitive(app->kbd_led_wizard_btn, TRUE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_skip_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_nokey_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_save_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_cancel_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(app->kbd_led_wizard_label), _("LED Wizard saved!"));
    gtk_widget_show(app->kbd_led_wizard_label);

    /* Back to the zone being edited and redraw */
    kbd_highlight_zone(app);
    kbd_redraw_canvas(app);
}

/* LED Wizard: Cancel */
static void on_led_wizard_cancel_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;

    app->kbd_wizard_active = 0;
    /* Cancel: back to the mapping from before the wizard started */
    memcpy(app->kbd_wizard_mapping, app->kbd_wizard_backup, sizeof(app->kbd_wizard_mapping));

    /* Restore all LEDs */
    send_keyboard_command(app, "TURN_ON");
    kbd_sync_power_switch(app);

    /* Update UI */
    gtk_widget_set_sensitive(app->kbd_led_wizard_btn, TRUE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_skip_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_nokey_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_save_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_cancel_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(app->kbd_led_wizard_label), _("LED Wizard cancelled"));
    gtk_widget_show(app->kbd_led_wizard_label);

    /* Back to the zone being edited and redraw */
    kbd_highlight_zone(app);
    kbd_redraw_canvas(app);
}

/* LED Wizard: Update UI for current step */
static void kbd_wizard_step(AppData *app, const char *prev_key_name) {
    char buf[320];
    int led = app->kbd_wizard_current_led;
    int cur = app->kbd_wizard_mapping[led];
    const char *now = (cur >= 0 && (size_t)cur < kbd_num_keys) ? kbd_key_names[cur] : _("no key");

    /* Previous assignment, current LED and the key it has now */
    if (prev_key_name && led > 0) {
        snprintf(buf, sizeof(buf), _("LED %d -> %s | Click the key for LED %d / 119 (now: %s)"),
                 led - 1, prev_key_name, led, now);
    } else {
        snprintf(buf, sizeof(buf), _("LED Wizard: Click the key for LED %d / 119 (now: %s)"), led, now);
    }
    gtk_label_set_text(GTK_LABEL(app->kbd_led_wizard_label), buf);
    gtk_widget_show(app->kbd_led_wizard_label);

    /* Turn off all LEDs first, then light up only the current LED */
    send_keyboard_command(app, "TURN_OFF");
    char cmd_buf[64];
    snprintf(cmd_buf, sizeof(cmd_buf), "TEST_LED %d", app->kbd_wizard_current_led);
    send_keyboard_command(app, cmd_buf);

    /* Highlight the current LED in the selection */
    app->kbd_num_selected_leds = 1;
    app->kbd_selected_led_indices[0] = app->kbd_wizard_current_led;
    kbd_redraw_canvas(app);
}

/* LED Wizard: Finish and save mapping */
static void kbd_wizard_finish(AppData *app) {
    app->kbd_wizard_active = 0;

    /* Restore all LEDs */
    send_keyboard_command(app, "TURN_ON");
    kbd_sync_power_switch(app);

    /* Update UI */
    gtk_widget_set_sensitive(app->kbd_led_wizard_btn, TRUE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_skip_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_nokey_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_cancel_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(app->kbd_led_wizard_label), _("LED Wizard completed!"));
    gtk_widget_show(app->kbd_led_wizard_label);

    /* Back to the zone being edited and redraw */
    kbd_highlight_zone(app);
    kbd_redraw_canvas(app);

    /* Save mapping to CSV */
    kbd_save_led_mapping_csv(app);
}

/* Save LED mapping to CSV file */
/* LED wizard mapping of a layout (1.28.0: one file per layout) */
static void kbd_mapping_path(char *buf, size_t size, const char *code) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, size, "%s/.config/dudes-control-center/keyboard_led_mapping_%s.csv", home, code);
}

static void kbd_save_led_mapping_csv(AppData *app) {
    char csv_path[512];
    kbd_mapping_path(csv_path, sizeof csv_path, kbd_layout_code);

    FILE *f = fopen(csv_path, "w");
    if (!f) {
        g_print("[GUI] WARNING: Could not write LED mapping CSV to %s\n", csv_path);
        return;
    }

    /* Write header (semicolon delimiter) */
    fprintf(f, "led_id;position;led_index;%s;layout=%s\n", KBD_CSV_NUMBERING_MARK, kbd_layout_code);

    /* Track which positions have been written and count LEDs per position */
    int pos_led_count[120] = {0};

    /* First pass: count LEDs per position */
    for (int led = 0; led < 120; led++) {
        int pos = app->kbd_wizard_mapping[led];
        if (pos >= 0 && pos < 120 && (size_t)pos < kbd_num_keys) {
            pos_led_count[pos]++;
        }
    }

    /* Second pass: write entries */
    int pos_led_written[120] = {0};
    for (int led = 0; led < 120; led++) {
        int pos = app->kbd_wizard_mapping[led];
        if (pos >= 0 && pos < 120 && (size_t)pos < kbd_num_keys) {
            char led_index_col[16] = "";
            if (pos_led_count[pos] > 1) {
                snprintf(led_index_col, sizeof(led_index_col), "%d", pos_led_written[pos]);
            }
            fprintf(f, "%d;%d;%s\n", led, pos, led_index_col);
            pos_led_written[pos]++;
        } else {
            /* Unmapped LED */
            fprintf(f, "%d;;\n", led);
        }
    }

    fclose(f);
    gui_event("[GUI] LED mapping saved for layout '%s': %s\n", kbd_layout_code, csv_path);
}

/* led -> position from a mapping CSV; legacy: positions are indices into
 * the old German table (converted). Returns 0 if the file was read. */
static int kbd_read_mapping_file(AppData *app, const char *csv_path, int legacy) {
    FILE *f = fopen(csv_path, "r");
    if (!f) return -1;
    char line[256];
    if (fgets(line, sizeof(line), f) == NULL) {      /* header */
        fclose(f);
        return -1;
    }
    memset(app->kbd_wizard_mapping, -1, sizeof(app->kbd_wizard_mapping));
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        char *sep = strchr(line, ';');
        if (!sep || sep[1] == ';' || sep[1] == '\0') continue;    /* unmapped LED */
        int led_id = atoi(line), position = atoi(sep + 1);
        if (led_id < 0 || led_id >= 120 || position < 0) continue;
        if (legacy) position = kbd_legacy_to_current(position);
        if (position < 0 || (size_t)position >= kbd_num_keys) continue;
        app->kbd_wizard_mapping[led_id] = position;
    }
    fclose(f);
    return 0;
}

/* Load the LED mapping of the current layout: its wizard file; for German
 * an old keyboard_led_mapping.csv (before 1.28.0) is converted once;
 * otherwise the built-in mapping */
static void kbd_load_led_mapping_csv(AppData *app) {
    /* A daemon reconnect calls this too: keep the wizard's unsaved changes */
    if (app->kbd_wizard_active) return;
    char csv_path[512];
    kbd_mapping_path(csv_path, sizeof csv_path, kbd_layout_code);
    if (kbd_read_mapping_file(app, csv_path, 0) == 0) {
        gui_event("[GUI] LED mapping loaded from %s\n", csv_path);
        return;
    }
    if (!strcmp(kbd_layout_code, "de")) {
        char old_path[512];
        const char *home = getenv("HOME");
        snprintf(old_path, sizeof old_path, "%s/.config/dudes-control-center/keyboard_led_mapping.csv", home ? home : "/tmp");
        if (kbd_read_mapping_file(app, old_path, 1) == 0) {
            gui_event("[GUI] LED mapping converted from %s to the German layout file\n", old_path);
            kbd_save_led_mapping_csv(app);       /* the old file stays for older versions */
            return;
        }
    }
    gui_event("[GUI] No LED mapping for layout '%s' (%s), using the built-in mapping\n", kbd_layout_code, csv_path);
    kbd_mapping_set_defaults(app);
}

/* LEDs that light a key position (a wide key can have several LEDs): from
 * the wizard CSV, or the built-in mapping when there is none.
 * Returns the number of LEDs written to out (at most max). */
static int kbd_leds_for_position(const AppData *app, int pos, int *out, int max) {
    int n = 0;
    for (int led = 0; led < 120; led++)
        if (app->kbd_wizard_mapping[led] == pos && pos >= 0 && n < max) out[n++] = led;
    return n;
}

/* The built-in mapping, until a wizard CSV is loaded */
static void kbd_mapping_set_defaults(AppData *app) {
    for (int led = 0; led < 120; led++)
        app->kbd_wizard_mapping[led] = kbd_legacy_to_current(kbd_default_led_position[led]);
}

static int kbd_led_is_selected(const AppData *app, int led) {
    for (int s = 0; s < app->kbd_num_selected_leds; s++)
        if (app->kbd_selected_led_indices[s] == led) return 1;
    return 0;
}

/* "IS_ON=0|1" from a keyboard STATUS response, -1 if missing */
static int kbd_parse_is_on(const char *response) {
    const char *p = response ? strstr(response, "IS_ON=") : NULL;
    if (!p) return -1;
    return atoi(p + 6) ? 1 : 0;
}

/* "BRIGHTNESS=n" and "BRIGHTNESS_MAX=m" from a keyboard STATUS response */
static int kbd_parse_brightness(const char *response, int *level, int *max) {
    const char *p = response ? strstr(response, "BRIGHTNESS=") : NULL;
    const char *q = response ? strstr(response, "BRIGHTNESS_MAX=") : NULL;
    if (!p || !q) return -1;
    *level = atoi(p + 11);
    *max = atoi(q + 15);
    return (*max > 0 && *level >= 0) ? 0 : -1;
}

static void kbd_show_brightness(AppData *app, int level, int max);

/* "KEY=value" line of a daemon response into out ("" if missing) */
static void kbd_status_field(const char *response, const char *key, char *out, size_t size) {
    out[0] = '\0';
    const char *p = response ? strstr(response, key) : NULL;
    if (!p) return;
    p += strlen(key);
    size_t n = strcspn(p, "\n");
    if (n >= size) n = size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Show only what the keyboard can do (1.44.0): per-key RGB (ite_829x) gets
 * everything; one color or a few zones no key picture, zones or LED
 * Wizard; a single-color keyboard only on/off and brightness */
static void kbd_apply_kind(AppData *app, const char *status) {
    char type[16], drv[64], leds[16];
    kbd_status_field(status, "KBD_TYPE=", type, sizeof type);
    kbd_status_field(status, "KBD_DRIVER=", drv, sizeof drv);
    kbd_status_field(status, "LED_COUNT=", leds, sizeof leds);
    int kind = !type[0] ? KBD_UI_UNKNOWN
             : !strcmp(type, "per-key") ? KBD_UI_PER_KEY
             : !strcmp(type, "rgb") ? KBD_UI_RGB
             : !strcmp(type, "white") ? KBD_UI_WHITE : KBD_UI_NONE;
    int n = atoi(leds);
    gchar *what = NULL;
    switch (kind) {
    case KBD_UI_PER_KEY: what = g_strdup(_("Keyboard: per-key RGB")); break;
    case KBD_UI_RGB:
        what = n > 1 ? g_strdup_printf(_("Keyboard: %d color zones, one color for all of them"), n)
                     : g_strdup(_("Keyboard: one color for the whole keyboard"));
        break;
    case KBD_UI_WHITE: what = g_strdup(_("Keyboard: single color, brightness only")); break;
    case KBD_UI_NONE:
        what = g_strdup(_("No keyboard backlight found. The keyboard daemon looks again every 10 seconds."));
        break;
    default: break;
    }
    if (what && drv[0] && kind != KBD_UI_NONE) {
        gchar *t = g_strdup_printf(_("%s (%s driver)"), what, drv);
        g_free(what);
        what = t;
    }
    if (what) label_set_changed(app->kbd_kind_label, what);
    widget_set_shown(app->kbd_kind_label, what != NULL);
    g_free(what);
    app->kbd_kind = kind;

    gboolean per_key = kind == KBD_UI_UNKNOWN || kind == KBD_UI_PER_KEY;
    gboolean colors = per_key || kind == KBD_UI_RGB;
    widget_set_shown(app->kbd_profile_frame, colors);
    widget_set_shown(app->kbd_preset_frame, colors);
    widget_set_shown(app->kbd_off_note, colors);
    widget_set_shown(app->kbd_backlight_frame, kind != KBD_UI_NONE);
    widget_set_shown(app->kbd_zone_frame, per_key);
    widget_set_shown(app->kbd_wizard_box, per_key);
    widget_set_shown(app->kbd_wizard_sep, per_key);
    widget_set_shown(app->kbd_whole_btn, per_key);

    /* Wave runs across the key matrix: only for per-key keyboards (1.44.1) */
    GtkComboBoxText *fx = GTK_COMBO_BOX_TEXT(app->kbd_fx_combo);
    int items = gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(fx)), NULL);
    app->kbd_fx_syncing = 1;
    if (!per_key && items == 4) gtk_combo_box_text_remove(fx, 3);
    else if (per_key && items == 3) gtk_combo_box_text_append_text(fx, _("Wave"));
    app->kbd_fx_syncing = 0;
    widget_set_shown(app->kbd_fx_dir, per_key);
    if (items != gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(fx)), NULL))
        kbd_fx_sync_controls(app);
}

/* Show the daemon's real power state on the "Backlight Off" switch
 * (without sending anything back) */
static void kbd_sync_power_switch(AppData *app) {
    if (!app->kbd_off_switch || !kbd_ensure_connected(app)) return;
    const char *status = send_keyboard_command(app, "STATUS");
    if (status && !strncmp(status, "STATUS OK", 9)) kbd_apply_kind(app, status);
    int level, max;
    if (kbd_parse_brightness(status, &level, &max) == 0) kbd_show_brightness(app, level, max);
    int is_on = kbd_parse_is_on(status);
    if (is_on < 0) return;
    if (app->kbd_off_active == !is_on && gtk_switch_get_active(GTK_SWITCH(app->kbd_off_switch)) == !is_on)
        return;   /* unchanged */
    app->kbd_off_active = !is_on;
    g_signal_handlers_block_by_func(app->kbd_off_switch, G_CALLBACK(on_kbd_off_switch_toggled), app);
    gtk_switch_set_active(GTK_SWITCH(app->kbd_off_switch), !is_on);
    gtk_switch_set_state(GTK_SWITCH(app->kbd_off_switch), !is_on);
    g_signal_handlers_unblock_by_func(app->kbd_off_switch, G_CALLBACK(on_kbd_off_switch_toggled), app);
    g_print("[GUI] Keyboard backlight is %s\n", is_on ? "on" : "off");
}

/* Key shapes on the keyboard picture. Most keys are rectangles. A key that
 * spans two rows (ISO Enter) is L-shaped if a key sits beside its lower
 * half: the lower part starts right of that key ('#' on a German layout).
 * Without this the Enter overlay covered part of '#', and clicks there
 * selected Enter. Returns the x where the lower part starts and the y where
 * the narrower lower part begins; for a plain rectangle lower_x = key->x. */
static void kbd_key_shape(size_t i, double *lower_x, double *split_y) {
    const KeyPos *k = &kbd_key_positions[i];
    *lower_x = k->x;
    *split_y = k->y + k->h;
    if (k->h < 100) return;                                  /* single row */
    for (size_t j = 0; j < kbd_num_keys; j++) {
        const KeyPos *o = &kbd_key_positions[j];
        if (o->h >= 100 || o == k) continue;
        int in_lower_row = o->y > k->y + 10 && o->y < k->y + k->h;
        /* starts left of us and reaches into our left edge (not a part of
         * the same key lying inside it, like the third Numpad Enter entry) */
        int overlaps_left = o->x < k->x && o->x + o->w > k->x && o->x + o->w < k->x + k->w;
        if (in_lower_row && overlaps_left) {
            double gap = 6.0;
            if (o->x + o->w + gap > *lower_x) *lower_x = o->x + o->w + gap;
            if (o->y - gap / 2 < *split_y) *split_y = o->y - gap / 2;
        }
    }
}

/* Several positions can describe one key (one per LED: Enter, Backspace,
 * Shift, Numpad Enter...). They have the same name and overlapping
 * rectangles; the first of them stands for the whole key. */
static int kbd_keys_overlap(size_t a, size_t b) {
    const KeyPos *p = &kbd_key_positions[a], *q = &kbd_key_positions[b];
    return p->x < q->x + q->w && q->x < p->x + p->w && p->y < q->y + q->h && q->y < p->y + p->h;
}

static size_t kbd_key_group_primary(size_t i) {
    for (size_t j = 0; j < i; j++)
        if (strcmp(kbd_key_names[j], kbd_key_names[i]) == 0 && kbd_keys_overlap(i, j))
            return kbd_key_group_primary(j);
    return i;
}

static int kbd_leds_for_position(const AppData *app, int pos, int *out, int max);

/* LEDs of all positions that belong to the key at primary position i */
static int kbd_leds_for_key(const AppData *app, size_t i, int *out, int max) {
    int n = 0;
    for (size_t p = i; p < kbd_num_keys && n < max; p++) {
        if (p != i && kbd_key_group_primary(p) != i) continue;
        int leds[120];
        int m = kbd_leds_for_position(app, (int)p, leds, 120);
        for (int k = 0; k < m && n < max; k++) {
            int dup = 0;
            for (int d = 0; d < n; d++) if (out[d] == leds[k]) dup = 1;
            if (!dup) out[n++] = leds[k];
        }
    }
    return n;
}

static int kbd_key_contains(size_t i, double x, double y) {
    const KeyPos *k = &kbd_key_positions[i];
    if (x < k->x || x > k->x + k->w || y < k->y || y > k->y + k->h) return 0;
    double lx, sy;
    kbd_key_shape(i, &lx, &sy);
    return y < sy || x >= lx;
}

/* Add the key's outline to the cairo path */
static void kbd_key_path(cairo_t *cr, size_t i, double ox, double oy, double s) {
    const KeyPos *k = &kbd_key_positions[i];
    double lx, sy;
    kbd_key_shape(i, &lx, &sy);
    double x1 = ox + k->x * s, x2 = ox + (k->x + k->w) * s;
    double y1 = oy + k->y * s, y2 = oy + (k->y + k->h) * s;
    if (lx <= k->x) {
        cairo_rectangle(cr, x1, y1, x2 - x1, y2 - y1);
        return;
    }
    double xl = ox + lx * s, ys = oy + sy * s;
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x2, y1);
    cairo_line_to(cr, x2, y2);
    cairo_line_to(cr, xl, y2);
    cairo_line_to(cr, xl, ys);
    cairo_line_to(cr, x1, ys);
    cairo_close_path(cr);
}

/* Forward: zone editor helpers (defined with the profile handlers) */
static int kbd_edit_parse(KbdProfileEdit *e, const char *resp);
static void kbd_edit_colors(const KbdProfileEdit *e, unsigned char rgb[120][3], unsigned char has[120]);
static int kbd_unsaved(const AppData *app);
static int kbd_load_edit_profile(AppData *app, const char *name);
static void kbd_send_preview(AppData *app, int light_on);

/* Parse a GET_ZONES response (base color + zones) into the per-LED colors
 * shown on the keyboard canvas. */
static void kbd_parse_zone_colors(AppData *app, const char *response) {
    static KbdProfileEdit e;
    if (kbd_edit_parse(&e, response) != 0) {
        memset(app->kbd_led_has_color, 0, sizeof app->kbd_led_has_color);
        return;
    }
    kbd_edit_colors(&e, app->kbd_led_rgb, app->kbd_led_has_color);
}

/* (Re)connected: show the profile being edited (resend unsaved changes) */
static void kbd_refresh_key_colors(AppData *app) {
    if (!kbd_ensure_connected(app)) return;
    if (app->kbd_edit_loaded && kbd_unsaved(app)) {
        kbd_send_preview(app, 0);
        kbd_edit_colors(&app->kbd_edit, app->kbd_led_rgb, app->kbd_led_has_color);
    } else if (app->kbd_edit_loaded && kbd_load_edit_profile(app, app->kbd_edit_name) == 0) {
        kbd_edit_colors(&app->kbd_edit, app->kbd_led_rgb, app->kbd_led_has_color);
    } else {
        kbd_parse_zone_colors(app, send_keyboard_command(app, "GET_ZONES"));
    }
    kbd_redraw_canvas(app);
}

/* Picture of the current layout (its SVG); German falls back to the old
 * PNG if the SVG cannot be loaded (no SVG loader installed) */
static void kbd_load_layout_picture(AppData *app) {
    if (app->kbd_keyboard_image) {
        cairo_surface_destroy(app->kbd_keyboard_image);
        app->kbd_keyboard_image = NULL;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s/keyboard_%s.svg", DCC_LAYOUT_DIR, kbd_layout_code, app->is_dark ? "dark" : "light");
    if (access(path, R_OK) != 0)               /* only a dark picture */
        snprintf(path, sizeof path, "%s/%s/keyboard_dark.svg", DCC_LAYOUT_DIR, kbd_layout_code);
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
    if (pb) {
        int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(surf);
        gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        g_object_unref(pb);
        app->kbd_keyboard_image = surf;
    } else {
        g_print("[GUI] WARNING: Could not load %s: %s\n", path, err ? err->message : "?");
        if (err) g_error_free(err);
        if (!strcmp(kbd_layout_code, "de")) {
            cairo_surface_t *png = cairo_image_surface_create_from_png("/usr/share/doc/dudes-control-center/images/keyboard_dark.png");
            if (cairo_surface_status(png) == CAIRO_STATUS_SUCCESS) app->kbd_keyboard_image = png;
            else cairo_surface_destroy(png);
        }
    }
    if (!app->kbd_keyboard_canvas) return;
    if (!app->kbd_keyboard_image) {
        gtk_widget_set_size_request(app->kbd_keyboard_canvas, -1, 200);   /* fallback height */
        return;
    }
    /* new aspect ratio: height for the current width */
    int width = gtk_widget_get_allocated_width(app->kbd_keyboard_canvas);
    app->kbd_canvas_width_used = 0;
    if (width > 1) {
        double iw = cairo_image_surface_get_width(app->kbd_keyboard_image);
        double ih = cairo_image_surface_get_height(app->kbd_keyboard_image);
        app->kbd_canvas_width_used = width;
        app->kbd_canvas_height_want = (int)(width * ih / iw + 0.5);
        gtk_widget_set_size_request(app->kbd_keyboard_canvas, -1, app->kbd_canvas_height_want);
    }
    gtk_widget_queue_draw(app->kbd_keyboard_canvas);
}

/* The keyboard picture keeps its aspect ratio: the canvas asks for the
 * height that fits its width (the zone list next to it gets the same
 * height), instead of filling all free space with the picture centered in
 * it (1.24.2). Only recomputed when the width changed noticeably, so a
 * scrollbar appearing or disappearing cannot make it oscillate. */
static gboolean kbd_canvas_apply_height(gpointer user_data) {
    AppData *app = (AppData *)user_data;
    if (app->kbd_keyboard_canvas && app->kbd_canvas_height_want > 0)
        gtk_widget_set_size_request(app->kbd_keyboard_canvas, -1, app->kbd_canvas_height_want);
    return G_SOURCE_REMOVE;
}

static void on_kbd_canvas_size_allocate(GtkWidget *widget, GdkRectangle *alloc, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    if (!app->kbd_keyboard_image || alloc->width <= 1) return;
    if (app->kbd_canvas_width_used && abs(alloc->width - app->kbd_canvas_width_used) < 24) return;
    double iw = cairo_image_surface_get_width(app->kbd_keyboard_image);
    double ih = cairo_image_surface_get_height(app->kbd_keyboard_image);
    if (iw <= 0) return;
    app->kbd_canvas_width_used = alloc->width;
    app->kbd_canvas_height_want = (int)(alloc->width * ih / iw + 0.5);
    g_idle_add(kbd_canvas_apply_height, app);   /* not during the allocation itself */
}

/* Draw callback for keyboard canvas */
static gboolean kbd_canvas_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);

    /* Draw keyboard image if available */
    if (app->kbd_keyboard_image) {
        double img_width = cairo_image_surface_get_width(app->kbd_keyboard_image);
        double img_height = cairo_image_surface_get_height(app->kbd_keyboard_image);

        /* Scale to fit canvas while maintaining aspect ratio */
        double scale_x = (double)width / img_width;
        double scale_y = (double)height / img_height;
        double scale = (scale_x < scale_y) ? scale_x : scale_y;

        double scaled_w = img_width * scale;
        double offset_x = (width - scaled_w) / 2.0;
        double offset_y = 0;   /* picture at the top (1.24.2) */

        cairo_save(cr);
        cairo_translate(cr, offset_x, offset_y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, app->kbd_keyboard_image, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);

        /* Draw key overlays */
        size_t num_keys = kbd_num_keys;
        for (size_t i = 0; i < num_keys; i++) {
            if (kbd_key_group_primary(i) != i) continue;   /* drawn with its key */
            /* Check if one of this key's LEDs is selected */
            int key_leds[120];
            int n_leds = kbd_leds_for_key(app, i, key_leds, 120);
            int is_selected = 0;
            for (int k = 0; k < n_leds && !is_selected; k++)
                is_selected = kbd_led_is_selected(app, key_leds[k]);

            /* Current color of the key (first LED with a color) */
            int has_color = 0;
            if (!app->kbd_wizard_active) {
                for (int k = 0; k < n_leds && !has_color; k++) {
                    int led = key_leds[k];
                    if (led >= 0 && led < 120 && app->kbd_led_has_color[led]) {
                        cairo_set_source_rgba(cr, app->kbd_led_rgb[led][0] / 255.0,
                                              app->kbd_led_rgb[led][1] / 255.0,
                                              app->kbd_led_rgb[led][2] / 255.0, 0.45);
                        kbd_key_path(cr, i, offset_x, offset_y, scale);
                        cairo_fill(cr);
                        has_color = 1;
                    }
                }
            }
            int has_mapping = 0;   /* colors replace the old "mapped key" blue tint */

            /* Draw overlay */
            if (is_selected || has_mapping) {
                /* Selected keys - green highlight, mapped keys - blue highlight */
                if (is_selected) {
                    cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 0.3);
                } else {
                    cairo_set_source_rgba(cr, 0.0, 0.0, 1.0, 0.25);
                }
                kbd_key_path(cr, i, offset_x, offset_y, scale);
                cairo_fill_preserve(cr);
                /* Border color matches highlight */
                if (is_selected) {
                    cairo_set_source_rgb(cr, 0.0, 1.0, 0.0);
                } else {
                    cairo_set_source_rgb(cr, 0.0, 0.0, 1.0);
                }
                cairo_set_line_width(cr, 2.0);
                cairo_stroke(cr);
            }
        }
        /* Rectangle being dragged (1.25.0) */
        if (app->kbd_drag_active && app->kbd_drag_moved) {
            double x = MIN(app->kbd_drag_x0, app->kbd_drag_x1), y = MIN(app->kbd_drag_y0, app->kbd_drag_y1);
            double w = ABS(app->kbd_drag_x1 - app->kbd_drag_x0), h = ABS(app->kbd_drag_y1 - app->kbd_drag_y0);
            cairo_rectangle(cr, x, y, w, h);
            cairo_set_source_rgba(cr, 0.3, 0.7, 1.0, 0.18);
            cairo_fill_preserve(cr);
            double dash[] = { 5.0, 3.0 };
            cairo_set_dash(cr, dash, 2, 0);
            cairo_set_line_width(cr, 1.5);
            cairo_set_source_rgb(cr, 0.3, 0.7, 1.0);
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0);
        }
    } else {
        /* No image loaded, show placeholder text */
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, width/2 - 80, height/2);
        cairo_show_text(cr, _("Keyboard schematic loading..."));
        cairo_stroke(cr);
    }

    return FALSE;
}

/* Click handler for keyboard canvas */
/* Click on the picture at widget position x, y (wizard or zone editor) */
static gboolean kbd_canvas_click(GtkWidget *widget, double click_x, double click_y, AppData *app) {
    if (!app->kbd_keyboard_image) return FALSE;

    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    double img_width = cairo_image_surface_get_width(app->kbd_keyboard_image);
    double img_height = cairo_image_surface_get_height(app->kbd_keyboard_image);

    /* Same scaling as draw callback */
    double scale_x = (double)width / img_width;
    double scale_y = (double)height / img_height;
    double scale = (scale_x < scale_y) ? scale_x : scale_y;

    double scaled_w = img_width * scale;
    double offset_x = (width - scaled_w) / 2.0;
    double offset_y = 0;   /* picture at the top (1.24.2) */

    /* Convert click to image coordinates */
    double unscaled_x = (click_x - offset_x) / scale;
    double unscaled_y = (click_y - offset_y) / scale;

    /* Find which key was clicked */
    size_t num_keys = kbd_num_keys;
    for (size_t i = 0; i < num_keys; i++) {
        if (kbd_key_contains(i, unscaled_x, unscaled_y)) {

            /* Handle wizard mode */
            if (app->kbd_wizard_active) {
                /* Assign this key position to the current LED */
                int current_led = app->kbd_wizard_current_led;
                app->kbd_wizard_mapping[current_led] = (int)i;
                const char *key_name = (i < kbd_num_keys) ? kbd_key_names[i] : "Unknown";

                /* Highlight the key just assigned to the current LED */
                app->kbd_num_selected_leds = 1;
                app->kbd_selected_led_indices[0] = current_led;
                kbd_redraw_canvas(app);
                while (gtk_events_pending()) {
                    gtk_main_iteration();
                }

                /* Move to next LED */
                app->kbd_wizard_current_led++;
                if (app->kbd_wizard_current_led >= 120) {
                    kbd_wizard_finish(app);
                } else {
                    kbd_wizard_step(app, key_name);
                }
                return TRUE;
            }

            /* Zone editor: a click adds the key to the zone being edited or
             * removes it; with no zone selected it selects the key's zone */
            int key_leds[120];
            int n_leds = kbd_leds_for_key(app, kbd_key_group_primary(i), key_leds, 120);
            const char *kname = i < kbd_num_keys ? kbd_key_names[i] : "?";
            if (n_leds == 0) {
                kbd_status(app, _("Key '%s' has no LED in the mapping (run the LED wizard)"), kname);
                return TRUE;
            }
            if (!app->kbd_edit_loaded) return TRUE;
            int zi = app->kbd_sel_zone;
            if (zi >= 0 && zi < app->kbd_edit.num_zones) {
                int added = kbd_edit_toggle_leds(&app->kbd_edit, zi, key_leds, n_leds);
                kbd_edit_changed(app, 1, 0);
                if (added) kbd_status(app, _("'%s' added to zone '%s' - click Save to keep it"), kname, app->kbd_edit.zones[zi].name);
                else kbd_status(app, _("'%s' removed from zone '%s' - click Save to keep it"), kname, app->kbd_edit.zones[zi].name);
            } else {
                int z = kbd_edit_zone_at_led(&app->kbd_edit, key_leds[0]);
                if (z >= 0) {
                    GtkTreePath *path = gtk_tree_path_new_from_indices(z, -1);
                    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(app->kbd_zone_view)), path);
                    gtk_tree_path_free(path);
                    kbd_status(app, _("Zone '%s' selected"), app->kbd_edit.zones[z].name);
                } else {
                    kbd_status(app, _("'%s' has the base color - select or create a zone to color single keys"), kname);
                }
            }
            return TRUE;
        }
    }

    return FALSE;
}

/* Widget -> picture coordinates, as in the draw callback */
static int kbd_canvas_to_image(GtkWidget *widget, AppData *app, double wx, double wy, double *ix, double *iy) {
    if (!app->kbd_keyboard_image) return -1;
    double iw = cairo_image_surface_get_width(app->kbd_keyboard_image);
    double ih = cairo_image_surface_get_height(app->kbd_keyboard_image);
    double width = gtk_widget_get_allocated_width(widget), height = gtk_widget_get_allocated_height(widget);
    double scale = MIN(width / iw, height / ih);
    if (scale <= 0) return -1;
    double offset_x = (width - iw * scale) / 2.0;
    *ix = (wx - offset_x) / scale;
    *iy = wy / scale;   /* picture at the top */
    return 0;
}

/* Keys (one entry per key, see kbd_key_group_primary) whose center lies in
 * the rectangle between two corners in picture coordinates (1.25.0) */
static int kbd_keys_in_rect(double ax, double ay, double bx, double by, size_t *out, int max) {
    double x0 = MIN(ax, bx), x1 = MAX(ax, bx), y0 = MIN(ay, by), y1 = MAX(ay, by);
    int n = 0;
    for (size_t i = 0; i < kbd_num_keys && n < max; i++) {
        if (kbd_key_group_primary(i) != i) continue;
        const KeyPos *k = &kbd_key_positions[i];
        double cx = k->x + k->w / 2.0, cy = k->y + k->h / 2.0;
        if (cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1) out[n++] = i;
    }
    return n;
}

/* Rectangle dragged over the picture: its keys join the zone being edited,
 * or leave it if all of them were in it already (like a click on one key) */
static void kbd_drag_select(GtkWidget *widget, AppData *app) {
    int zi = app->kbd_sel_zone;
    if (!app->kbd_edit_loaded || zi < 0 || zi >= app->kbd_edit.num_zones) return;
    double ax, ay, bx, by;
    if (kbd_canvas_to_image(widget, app, app->kbd_drag_x0, app->kbd_drag_y0, &ax, &ay) != 0 ||
        kbd_canvas_to_image(widget, app, app->kbd_drag_x1, app->kbd_drag_y1, &bx, &by) != 0)
        return;
    size_t keys[KBD_MAX_KEYS];
    int nk = kbd_keys_in_rect(ax, ay, bx, by, keys, KBD_MAX_KEYS);
    int leds[120], n = 0;
    for (int k = 0; k < nk; k++) {
        int kl[120];
        int m = kbd_leds_for_key(app, keys[k], kl, 120);
        for (int j = 0; j < m && n < 120; j++) {
            int dup = 0;
            for (int d = 0; d < n; d++) if (leds[d] == kl[j]) dup = 1;
            if (!dup) leds[n++] = kl[j];
        }
    }
    if (n == 0) {
        kbd_status(app, _("No keys with an LED in the marked area"));
        return;
    }
    int added = kbd_edit_toggle_leds(&app->kbd_edit, zi, leds, n);
    kbd_edit_changed(app, 1, 0);
    if (added) kbd_status(app, _("%d key(s) added to zone '%s' - click Save to keep it"), nk, app->kbd_edit.zones[zi].name);
    else kbd_status(app, _("%d key(s) removed from zone '%s' - click Save to keep it"), nk, app->kbd_edit.zones[zi].name);
}

/* Left button: in the zone editor a click or a dragged rectangle (decided
 * on release); otherwise (wizard, no zone selected) a click right away.
 * Only real presses count: GTK also sends a double-click event, which used
 * to count as one more click. */
static gboolean kbd_canvas_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    if (!app->kbd_keyboard_image || event->button != 1 || event->type != GDK_BUTTON_PRESS) return FALSE;
    if (!app->kbd_wizard_active && app->kbd_edit_loaded &&
        app->kbd_sel_zone >= 0 && app->kbd_sel_zone < app->kbd_edit.num_zones) {
        app->kbd_drag_active = 1;
        app->kbd_drag_moved = 0;
        app->kbd_drag_x0 = app->kbd_drag_x1 = event->x;
        app->kbd_drag_y0 = app->kbd_drag_y1 = event->y;
        return TRUE;
    }
    return kbd_canvas_click(widget, event->x, event->y, app);
}

static gboolean kbd_canvas_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    if (!app->kbd_drag_active) return FALSE;
    app->kbd_drag_x1 = event->x;
    app->kbd_drag_y1 = event->y;
    if (ABS(app->kbd_drag_x1 - app->kbd_drag_x0) > 4 || ABS(app->kbd_drag_y1 - app->kbd_drag_y0) > 4)
        app->kbd_drag_moved = 1;
    if (app->kbd_drag_moved) kbd_redraw_canvas(app);
    return TRUE;
}

static gboolean kbd_canvas_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    if (!app->kbd_drag_active || event->button != 1) return FALSE;
    app->kbd_drag_active = 0;
    if (app->kbd_drag_moved) kbd_drag_select(widget, app);
    else kbd_canvas_click(widget, app->kbd_drag_x0, app->kbd_drag_y0, app);
    app->kbd_drag_moved = 0;
    kbd_redraw_canvas(app);
    return TRUE;
}

/* Callback: Preset color button clicked */
static void on_preset_color_clicked(GtkWidget *widget, AppData *app) {
    int *rgb = (int *)g_object_get_data(G_OBJECT(widget), "preset-rgb");
    if (!rgb) return;
    kbd_apply_color(app, rgb[0], rgb[1], rgb[2], gtk_button_get_label(GTK_BUTTON(widget)));
}

/* Callback: Custom color (color chooser) */
static void on_kbd_custom_color_clicked(GtkWidget *widget, AppData *app) {
    (void)widget;
    const KbdProfileEdit *e = &app->kbd_edit;
    int zi = app->kbd_sel_zone;
    const unsigned char *cur = (zi >= 0 && zi < e->num_zones) ? e->zones[zi].rgb
                             : (e->has_base ? e->base : (const unsigned char *)"\xff\xff\xff");
    GtkWidget *dialog = gtk_color_chooser_dialog_new(
        (zi >= 0 && zi < e->num_zones) ? _("Zone Color") : _("Keyboard Color"), GTK_WINDOW(app->window));
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(dialog), FALSE);
    GdkRGBA rgba = { cur[0] / 255.0, cur[1] / 255.0, cur[2] / 255.0, 1.0 };
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(dialog), &rgba);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dialog), &rgba);
        kbd_apply_color(app, (int)(rgba.red * 255 + 0.5), (int)(rgba.green * 255 + 0.5),
                        (int)(rgba.blue * 255 + 0.5), _("Custom color"));
    }
    gtk_widget_destroy(dialog);
}

/* Keyboard brightness slider: shows the daemon's level (also when it was
 * changed with the desktop's keyboard slider) without sending it back */
static void kbd_show_brightness(AppData *app, int level, int max) {
    if (!app->kbd_brightness_scale || app->kbd_brightness_timer) return;  /* user is dragging */
    GtkRange *r = GTK_RANGE(app->kbd_brightness_scale);
    if (max != app->kbd_brightness_max) {
        app->kbd_brightness_max = max;
        gtk_range_set_range(r, 0, max);
    }
    if ((int)gtk_range_get_value(r) != level) {
        g_object_set_data(G_OBJECT(r), "dcc-syncing", GINT_TO_POINTER(1));
        gtk_range_set_value(r, level);
        g_object_set_data(G_OBJECT(r), "dcc-syncing", NULL);
    }
    gtk_widget_set_sensitive(app->kbd_brightness_scale, TRUE);
}

static gboolean kbd_brightness_send(gpointer user_data) {
    AppData *app = (AppData *)user_data;
    app->kbd_brightness_timer = 0;
    int level = (int)gtk_range_get_value(GTK_RANGE(app->kbd_brightness_scale));
    char cmd[48];
    snprintf(cmd, sizeof cmd, "SET_BRIGHTNESS %d", level);
    const char *r = send_keyboard_command(app, cmd);
    gui_event("[GUI] %s -> %s\n", cmd, r ? r : "NULL");
    return G_SOURCE_REMOVE;
}

static void on_kbd_brightness_changed(GtkRange *range, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    if (g_object_get_data(G_OBJECT(range), "dcc-syncing")) return;
    if (app->kbd_brightness_timer) g_source_remove(app->kbd_brightness_timer);
    app->kbd_brightness_timer = g_timeout_add(120, kbd_brightness_send, app);
}

/* Create Page 7: Keyboard Backlight Control */
static GtkWidget *create_keyboard_page(AppData *app) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    /* --- Status section --- */
    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    app->kbd_status_label = gtk_label_new(_("Keyboard daemon not connected"));
    gtk_label_set_xalign(GTK_LABEL(app->kbd_status_label), 0.0);
    gtk_widget_set_hexpand(app->kbd_status_label, TRUE);
    gtk_box_pack_start(GTK_BOX(status_box), app->kbd_status_label, FALSE, FALSE, 0);
    app->kbd_kind_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->kbd_kind_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(app->kbd_kind_label), TRUE);
    gtk_box_pack_start(GTK_BOX(status_box), app->kbd_kind_label, FALSE, FALSE, 0);

    GtkWidget *status_frame = gtk_frame_new(_("Keyboard Daemon Status"));
    gtk_container_set_border_width(GTK_CONTAINER(status_frame), 6);
    gtk_container_add(GTK_CONTAINER(status_frame), status_box);
    gtk_box_pack_start(GTK_BOX(vbox), status_frame, FALSE, FALSE, 0);

    /* --- Profile management section --- */
    GtkWidget *profile_frame = app->kbd_profile_frame = gtk_frame_new(_("Keyboard Profiles"));
    gtk_container_set_border_width(GTK_CONTAINER(profile_frame), 6);
    GtkWidget *profile_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    /* Profile combo box */
    GtkWidget *profile_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_label_set_xalign(GTK_LABEL(gtk_label_new(_("Profile:"))), 0.0);
    GtkWidget *profile_label = gtk_label_new(_("Profile:"));
    gtk_box_pack_start(GTK_BOX(profile_hbox), profile_label, FALSE, FALSE, 0);

    app->kbd_profile_store = gtk_list_store_new(1, G_TYPE_STRING);
    app->kbd_profile_combo = gtk_combo_box_new();
    gtk_combo_box_set_model(GTK_COMBO_BOX(app->kbd_profile_combo), GTK_TREE_MODEL(app->kbd_profile_store));
    {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(app->kbd_profile_combo), renderer, TRUE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(app->kbd_profile_combo), renderer, "text", 0, NULL);
    }
    gtk_combo_box_set_id_column(GTK_COMBO_BOX(app->kbd_profile_combo), 0);
    app->kbd_profile_combo_signal_id = g_signal_connect(app->kbd_profile_combo, "changed",
                                                        G_CALLBACK(on_kbd_profile_changed), app);
    gtk_box_pack_start(GTK_BOX(profile_hbox), app->kbd_profile_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(profile_vbox), profile_hbox, FALSE, FALSE, 0);

    /* Profile buttons row */
    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->kbd_profile_create_btn = gtk_button_new_with_label(_("Create"));
    g_signal_connect(app->kbd_profile_create_btn, "clicked", G_CALLBACK(on_kbd_create_profile_clicked), app);
    app->kbd_profile_delete_btn = gtk_button_new_with_label(_("Delete"));
    g_signal_connect(app->kbd_profile_delete_btn, "clicked", G_CALLBACK(on_kbd_delete_profile_clicked), app);
    app->kbd_profile_rename_btn = gtk_button_new_with_label(_("Rename"));
    g_signal_connect(app->kbd_profile_rename_btn, "clicked", G_CALLBACK(on_kbd_rename_profile_clicked), app);
    app->kbd_profile_save_btn = gtk_button_new_with_label(_("Save"));
    g_signal_connect(app->kbd_profile_save_btn, "clicked", G_CALLBACK(on_kbd_save_profile_clicked), app);
    app->kbd_profile_activate_btn = gtk_button_new_with_label(_("Activate"));
    g_signal_connect(app->kbd_profile_activate_btn, "clicked", G_CALLBACK(on_kbd_activate_profile_clicked), app);
    gtk_box_pack_start(GTK_BOX(btn_row), app->kbd_profile_create_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_row), app->kbd_profile_rename_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_row), app->kbd_profile_save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_row), app->kbd_profile_delete_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_row), app->kbd_profile_activate_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(profile_vbox), btn_row, FALSE, FALSE, 0);

    /* Profile status label */
    app->kbd_profile_status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->kbd_profile_status_label), 0.0);
    gtk_widget_set_name(app->kbd_profile_status_label, "kbd-status-label");
    gtk_box_pack_start(GTK_BOX(profile_vbox), app->kbd_profile_status_label, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(profile_frame), profile_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), profile_frame, FALSE, FALSE, 0);

    /* --- Off Switch section --- */
    GtkWidget *off_switch_frame = app->kbd_backlight_frame = gtk_frame_new(_("Backlight"));
    gtk_container_set_border_width(GTK_CONTAINER(off_switch_frame), 6);
    GtkWidget *off_switch_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    GtkWidget *off_switch_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *off_switch_label = gtk_label_new(_("Backlight Off:"));
    gtk_label_set_xalign(GTK_LABEL(off_switch_label), 0.0);
    gtk_box_pack_start(GTK_BOX(off_switch_hbox), off_switch_label, FALSE, FALSE, 0);

    app->kbd_off_switch = gtk_switch_new();
    gtk_switch_set_state(GTK_SWITCH(app->kbd_off_switch), FALSE);
    g_signal_connect(app->kbd_off_switch, "state-set", G_CALLBACK(on_kbd_off_switch_toggled), app);
    gtk_box_pack_start(GTK_BOX(off_switch_hbox), app->kbd_off_switch, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(off_switch_vbox), off_switch_hbox, FALSE, FALSE, 0);

    /* Brightness (keyboard hardware brightness, also set by the desktop's keyboard slider) */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *label = gtk_label_new(_("Brightness:"));
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->kbd_brightness_max = 10;
        app->kbd_brightness_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 10, 1);
        gtk_scale_set_digits(GTK_SCALE(app->kbd_brightness_scale), 0);
        gtk_scale_set_value_pos(GTK_SCALE(app->kbd_brightness_scale), GTK_POS_RIGHT);
        gtk_range_set_value(GTK_RANGE(app->kbd_brightness_scale), 10);
        gtk_widget_set_size_request(app->kbd_brightness_scale, 220, -1);
        gtk_widget_set_sensitive(app->kbd_brightness_scale, FALSE);   /* until the daemon answered */
        g_signal_connect(app->kbd_brightness_scale, "value-changed", G_CALLBACK(on_kbd_brightness_changed), app);
        gtk_box_pack_start(GTK_BOX(row), app->kbd_brightness_scale, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(off_switch_vbox), row, FALSE, FALSE, 0);
    }

    GtkWidget *off_switch_note = app->kbd_off_note = gtk_label_new(_("When ON, overrides all profile settings until disabled."));
    gtk_widget_set_name(off_switch_note, "kbd-off-switch-note");
    gtk_label_set_xalign(GTK_LABEL(off_switch_note), 0.0);
    gtk_box_pack_start(GTK_BOX(off_switch_vbox), off_switch_note, FALSE, FALSE, 0);

    /* Backlight settings on the left, the LED Wizard on the right (1.24.2:
     * it is the key-to-LED mapping, not part of the zone editor) */
    GtkWidget *backlight_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(backlight_row), off_switch_vbox, FALSE, FALSE, 0);
    app->kbd_wizard_sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(backlight_row), app->kbd_wizard_sep, FALSE, FALSE, 0);
    GtkWidget *wizard_box = app->kbd_wizard_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    {
        GtkWidget *wizard_title = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(wizard_title), _("<b>LED Wizard</b>"));
        gtk_label_set_xalign(GTK_LABEL(wizard_title), 0.0);
        gtk_box_pack_start(GTK_BOX(wizard_box), wizard_title, FALSE, FALSE, 0);
        GtkWidget *wizard_note = gtk_label_new(_("Lights one LED after the other; click the matching key on the picture "
                                               "below. Needed once for per-key colors."));
        gtk_label_set_xalign(GTK_LABEL(wizard_note), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(wizard_note), TRUE);
        gtk_widget_set_name(wizard_note, "kbd-off-switch-note");
        gtk_box_pack_start(GTK_BOX(wizard_box), wizard_note, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(backlight_row), wizard_box, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(off_switch_frame), backlight_row);
    gtk_box_pack_start(GTK_BOX(vbox), off_switch_frame, FALSE, FALSE, 0);

    /* --- Preset Colors section --- */
    GtkWidget *preset_frame = app->kbd_preset_frame = gtk_frame_new(_("Preset Colors"));
    gtk_container_set_border_width(GTK_CONTAINER(preset_frame), 6);
    GtkWidget *preset_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    app->kbd_mode_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->kbd_mode_label), 0.0);
    gtk_box_pack_start(GTK_BOX(preset_vbox), app->kbd_mode_label, FALSE, FALSE, 0);

    GtkWidget *preset_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    /* Define preset colors: {label, R, G, B} */
    const char *preset_names[] = {_("White"), _("Red"), _("Green"), _("Blue"), _("Yellow"), _("Cyan"), _("Magenta"), _("Orange"), _("Purple")};
    uint8_t preset_r[] = {255, 255, 0, 0, 255, 0, 255, 255, 128};
    uint8_t preset_g[] = {255, 0, 255, 0, 255, 255, 0, 165, 0};
    uint8_t preset_b[] = {255, 0, 0, 255, 0, 255, 255, 0, 128};
    int preset_count = 9;

    for (int i = 0; i < preset_count; i++) {
        GtkWidget *btn = gtk_button_new_with_label(preset_names[i]);
        g_signal_connect_data(btn, "clicked", G_CALLBACK(on_preset_color_clicked), app,
                              NULL, (GConnectFlags)G_CONNECT_AFTER);
        /* Store RGB values as user data on the button */
        int *rgb = g_new(int, 3);
        rgb[0] = preset_r[i];
        rgb[1] = preset_g[i];
        rgb[2] = preset_b[i];
        g_object_set_data_full(G_OBJECT(btn), "preset-rgb", rgb, g_free);
        gtk_box_pack_start(GTK_BOX(preset_hbox), btn, FALSE, FALSE, 0);
    }

    {
        GtkWidget *custom_btn = gtk_button_new_with_label(_("Custom…"));
        gtk_widget_set_tooltip_text(custom_btn, _("Choose any color"));
        g_signal_connect(custom_btn, "clicked", G_CALLBACK(on_kbd_custom_color_clicked), app);
        gtk_box_pack_start(GTK_BOX(preset_hbox), custom_btn, FALSE, FALSE, 0);

        /* Effect, Speed, Direction (1.35.0) */
        GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
        gtk_box_pack_start(GTK_BOX(preset_hbox), sep, FALSE, FALSE, 4);
        gtk_box_pack_start(GTK_BOX(preset_hbox), gtk_label_new(_("Effect:")), FALSE, FALSE, 0);
        app->kbd_fx_combo = gtk_combo_box_text_new();
        const char *fx_labels[] = { N_("Static"), N_("Breathing"), N_("Rainbow"), N_("Wave") };
        for (int i = 0; i < 4; i++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->kbd_fx_combo), _(fx_labels[i]));
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->kbd_fx_combo), 0);
        gtk_widget_set_tooltip_text(app->kbd_fx_combo,
            _("Animated effect of the selected zone, or of the base color when no zone is selected. "
              "Breathing fades the color in and out; Rainbow and Wave use all colors. "
              "Runs in the keyboard daemon, also when this window is closed."));
        gtk_box_pack_start(GTK_BOX(preset_hbox), app->kbd_fx_combo, FALSE, FALSE, 0);
        app->kbd_fx_speed = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 10, 1);
        gtk_scale_set_draw_value(GTK_SCALE(app->kbd_fx_speed), FALSE);
        gtk_range_set_value(GTK_RANGE(app->kbd_fx_speed), 5);
        gtk_widget_set_size_request(app->kbd_fx_speed, 110, -1);
        gtk_widget_set_tooltip_text(app->kbd_fx_speed, _("Speed: slow (left) to fast (right)"));
        gtk_widget_set_sensitive(app->kbd_fx_speed, FALSE);
        gtk_box_pack_start(GTK_BOX(preset_hbox), app->kbd_fx_speed, FALSE, FALSE, 0);
        app->kbd_fx_dir = gtk_combo_box_text_new();
        const char *dir_labels[] = { "\u2192", "\u2190", "\u2193", "\u2191" };   /* → ← ↓ ↑ */
        for (int i = 0; i < 4; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->kbd_fx_dir), dir_labels[i]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->kbd_fx_dir), 0);
        gtk_widget_set_tooltip_text(app->kbd_fx_dir, _("Direction of the wave"));
        gtk_widget_set_sensitive(app->kbd_fx_dir, FALSE);
        gtk_box_pack_start(GTK_BOX(preset_hbox), app->kbd_fx_dir, FALSE, FALSE, 0);
        g_signal_connect(app->kbd_fx_combo, "changed", G_CALLBACK(on_kbd_fx_changed), app);
        g_signal_connect(app->kbd_fx_speed, "value-changed", G_CALLBACK(on_kbd_fx_changed), app);
        g_signal_connect(app->kbd_fx_dir, "changed", G_CALLBACK(on_kbd_fx_changed), app);

        GtkWidget *whole_btn = app->kbd_whole_btn = gtk_button_new_with_label(_("Whole Keyboard"));
        gtk_widget_set_tooltip_text(whole_btn, _("Stop editing a zone: colors go to the whole keyboard (base color)"));
        g_signal_connect(whole_btn, "clicked", G_CALLBACK(on_kbd_whole_keyboard_clicked), app);
        gtk_box_pack_end(GTK_BOX(preset_hbox), whole_btn, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(preset_vbox), preset_hbox, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(preset_frame), preset_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), preset_frame, FALSE, FALSE, 0);

    /* --- Visual Zone Editor section --- */
    GtkWidget *zone_frame = app->kbd_zone_frame = gtk_frame_new(_("Visual Zone Editor"));
    gtk_container_set_border_width(GTK_CONTAINER(zone_frame), 6);
    GtkWidget *zone_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);


    /* LED Wizard controls */
    GtkWidget *wizard_btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->kbd_led_wizard_btn = gtk_button_new_with_label(_("LED Wizard"));
    g_signal_connect(app->kbd_led_wizard_btn, "clicked", G_CALLBACK(on_led_wizard_start_clicked), app);
    app->kbd_led_wizard_skip_btn = gtk_button_new_with_label(_("Skip"));
    gtk_widget_set_tooltip_text(app->kbd_led_wizard_skip_btn, _("Keep this LED's current key and go to the next LED"));
    g_signal_connect(app->kbd_led_wizard_skip_btn, "clicked", G_CALLBACK(on_led_wizard_skip_clicked), app);
    gtk_widget_set_sensitive(app->kbd_led_wizard_skip_btn, FALSE);
    app->kbd_led_wizard_nokey_btn = gtk_button_new_with_label(_("No Key"));
    gtk_widget_set_tooltip_text(app->kbd_led_wizard_nokey_btn, _("This LED lights no key: remove its key and go to the next LED"));
    g_signal_connect(app->kbd_led_wizard_nokey_btn, "clicked", G_CALLBACK(on_led_wizard_nokey_clicked), app);
    gtk_widget_set_sensitive(app->kbd_led_wizard_nokey_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_nokey_btn, FALSE);
    app->kbd_led_wizard_cancel_btn = gtk_button_new_with_label(_("Cancel"));
    g_signal_connect(app->kbd_led_wizard_cancel_btn, "clicked", G_CALLBACK(on_led_wizard_cancel_clicked), app);
    gtk_widget_set_sensitive(app->kbd_led_wizard_cancel_btn, FALSE);
    app->kbd_led_wizard_save_btn = gtk_button_new_with_label(_("Save & Finish"));
    g_signal_connect(app->kbd_led_wizard_save_btn, "clicked", G_CALLBACK(on_led_wizard_save_finish_clicked), app);
    gtk_widget_set_sensitive(app->kbd_led_wizard_save_btn, FALSE);
    gtk_box_pack_start(GTK_BOX(wizard_btn_row), app->kbd_led_wizard_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wizard_btn_row), app->kbd_led_wizard_skip_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wizard_btn_row), app->kbd_led_wizard_nokey_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wizard_btn_row), app->kbd_led_wizard_save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wizard_btn_row), app->kbd_led_wizard_cancel_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wizard_box), wizard_btn_row, FALSE, FALSE, 0);

    /* LED Wizard status label */
    app->kbd_led_wizard_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->kbd_led_wizard_label), 0.0);
    gtk_widget_set_name(app->kbd_led_wizard_label, "kbd-wizard-label");
    gtk_widget_set_no_show_all(app->kbd_led_wizard_label, TRUE);   /* shown once the wizard runs */
    gtk_label_set_line_wrap(GTK_LABEL(app->kbd_led_wizard_label), TRUE);
    gtk_box_pack_start(GTK_BOX(wizard_box), app->kbd_led_wizard_label, FALSE, FALSE, 0);

    /* Keyboard schematic canvas */
    app->kbd_keyboard_canvas = gtk_drawing_area_new();
    gtk_widget_set_events(app->kbd_keyboard_canvas,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK);
    g_signal_connect(app->kbd_keyboard_canvas, "draw", G_CALLBACK(kbd_canvas_draw), app);
    g_signal_connect(app->kbd_keyboard_canvas, "button-press-event", G_CALLBACK(kbd_canvas_button_press), app);
    g_signal_connect(app->kbd_keyboard_canvas, "motion-notify-event", G_CALLBACK(kbd_canvas_motion), app);
    g_signal_connect(app->kbd_keyboard_canvas, "button-release-event", G_CALLBACK(kbd_canvas_button_release), app);
    g_signal_connect(app->kbd_keyboard_canvas, "size-allocate", G_CALLBACK(on_kbd_canvas_size_allocate), app);
    /* Picture on the left, zone list on the right */
    GtkWidget *canvas_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(canvas_row), app->kbd_keyboard_canvas, TRUE, TRUE, 0);
    {
        GtkWidget *zframe = gtk_frame_new(_("Zones"));
        GtkWidget *zbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(zbox), 4);
        app->kbd_zone_store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
        app->kbd_zone_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->kbd_zone_store));
        GtkCellRenderer *rc = gtk_cell_renderer_text_new();
        gtk_tree_view_append_column(GTK_TREE_VIEW(app->kbd_zone_view),
            gtk_tree_view_column_new_with_attributes(_("Color"), rc, "markup", 0, NULL));
        GtkCellRenderer *rn = gtk_cell_renderer_text_new();
        g_object_set(rn, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *cn = gtk_tree_view_column_new_with_attributes(_("Zone"), rn, "text", 1, NULL);
        gtk_tree_view_column_set_expand(cn, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(app->kbd_zone_view), cn);
        gtk_tree_view_append_column(GTK_TREE_VIEW(app->kbd_zone_view),
            gtk_tree_view_column_new_with_attributes(_("LEDs"), gtk_cell_renderer_text_new(), "text", 2, NULL));
        GtkTreeSelection *zsel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->kbd_zone_view));
        gtk_tree_selection_set_mode(zsel, GTK_SELECTION_SINGLE);
        app->kbd_zone_sel_signal_id = g_signal_connect(zsel, "changed", G_CALLBACK(on_kbd_zone_selection_changed), app);
        GtkWidget *zscroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(zscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(zscroll), app->kbd_zone_view);
        gtk_box_pack_start(GTK_BOX(zbox), zscroll, TRUE, TRUE, 0);
        /* Create, Rename, Save, Delete (like the profile buttons) */
        GtkWidget *zbtns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *zc = gtk_button_new_with_label(_("Create"));
        g_signal_connect(zc, "clicked", G_CALLBACK(on_kbd_zone_create_clicked), app);
        app->kbd_zone_rename_btn = gtk_button_new_with_label(_("Rename"));
        g_signal_connect(app->kbd_zone_rename_btn, "clicked", G_CALLBACK(on_kbd_zone_rename_clicked), app);
        GtkWidget *zs = gtk_button_new_with_label(_("Save"));
        gtk_widget_set_tooltip_text(zs, _("Save the profile with its zones"));
        g_signal_connect(zs, "clicked", G_CALLBACK(on_kbd_zone_save_clicked), app);
        app->kbd_zone_delete_btn = gtk_button_new_with_label(_("Delete"));
        g_signal_connect(app->kbd_zone_delete_btn, "clicked", G_CALLBACK(on_kbd_zone_delete_clicked), app);
        gtk_widget_set_sensitive(app->kbd_zone_rename_btn, FALSE);
        gtk_widget_set_sensitive(app->kbd_zone_delete_btn, FALSE);
        gtk_box_pack_start(GTK_BOX(zbtns), zc, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(zbtns), app->kbd_zone_rename_btn, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(zbtns), zs, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(zbtns), app->kbd_zone_delete_btn, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(zbox), zbtns, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(zframe), zbox);
        gtk_widget_set_size_request(zframe, 300, -1);
        gtk_box_pack_start(GTK_BOX(canvas_row), zframe, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(zone_vbox), canvas_row, FALSE, FALSE, 0);   /* as high as the picture */

    /* Load keyboard image */
    kbd_load_layout_picture(app);


    gtk_container_add(GTK_CONTAINER(zone_frame), zone_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), zone_frame, FALSE, FALSE, 0);

    /* Parts hidden for keyboards without per-key colors (1.44.0): shown now,
     * then left alone by the window's show_all */
    GtkWidget *hideable[] = { app->kbd_profile_frame, app->kbd_backlight_frame, app->kbd_preset_frame,
                              app->kbd_zone_frame, app->kbd_wizard_box, app->kbd_wizard_sep,
                              app->kbd_whole_btn, app->kbd_off_note, app->kbd_kind_label };
    for (size_t i = 0; i < G_N_ELEMENTS(hideable); i++) {
        gtk_widget_show_all(hideable[i]);
        gtk_widget_set_no_show_all(hideable[i], TRUE);
    }
    gtk_widget_hide(app->kbd_kind_label);   /* until the daemon tells the kind */

    return vbox;
}

/* Keyboard layout at start: config.json, otherwise the system's keyboard
 * layout (/etc/default/keyboard XKBLAYOUT), otherwise German */
static void kbd_layout_init(void) {
    char code[8] = "";
    cJSON *json = config_read();
    cJSON *v = cJSON_GetObjectItem(json, "keyboard_layout");
    if (cJSON_IsString(v) && kbd_layout_known(v->valuestring)) snprintf(code, sizeof code, "%s", v->valuestring);
    cJSON_Delete(json);
    if (!code[0]) {
        gchar *kb = NULL;
        if (g_file_get_contents("/etc/default/keyboard", &kb, NULL, NULL)) {
            const char *x = strstr(kb, "XKBLAYOUT=");
            if (x) {
                char first[16] = "";
                x += 10;
                if (*x == '"') x++;
                size_t n = 0;
                while (x[n] && x[n] != ',' && x[n] != '"' && x[n] != '\n' && n < sizeof first - 1) { first[n] = x[n]; n++; }
                first[n] = '\0';
                for (int i = 0; i < KBD_NUM_LAYOUTS && !code[0]; i++) {
                    gchar **xk = g_strsplit(kbd_layouts[i].xkb, ",", -1);
                    for (int j = 0; xk[j]; j++)
                        if (!strcmp(xk[j], first)) snprintf(code, sizeof code, "%s", kbd_layouts[i].code);
                    g_strfreev(xk);
                }
            }
            g_free(kb);
        }
    }
    if (!code[0]) snprintf(code, sizeof code, "de");
    if (kbd_layout_load(code) != 0 && strcmp(code, "de") != 0) kbd_layout_load("de");
}

static void kbd_layout_save_choice(void) {
    cJSON *json = config_read();
    cJSON_DeleteItemFromObject(json, "keyboard_layout");
    cJSON_AddStringToObject(json, "keyboard_layout", kbd_layout_code);
    config_write(json);
    cJSON_Delete(json);
}
