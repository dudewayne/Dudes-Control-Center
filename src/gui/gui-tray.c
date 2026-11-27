// SPDX-License-Identifier: GPL-2.0-only
/*
 * gui-tray.c
 * Tray icon and its menu.
 * Part of dcc-gui.c, which #includes it: one translation unit, so the
 * static functions and AppData stay shared (split up in 1.48.1).
 */


/* Update tray menu - destroy old and create new one with updated profile states */
static void update_tray_menu(AppData *app) {
    if (app->tray_menu && GTK_IS_WIDGET(app->tray_menu)) {
        gtk_widget_destroy(app->tray_menu);
    }
    app->tray_menu = create_tray_menu(app);
}

/* Create system tray icon with context menu */
static void create_system_tray(AppData *app) {
    /* Load the icon - try multiple locations */
    GdkPixbuf *pixbuf = NULL;

    /* Try installed location first (from .deb package) */
    pixbuf = gdk_pixbuf_new_from_file_at_size("/usr/share/icons/hicolor/256x256/apps/dudes-control-center.png", 48, 48, NULL);

    /* Fallback to current directory (for development) */
    if (!pixbuf) {
        g_print("[GUI] Trying alternative icon location...\n");
        pixbuf = gdk_pixbuf_new_from_file_at_size("DudesControlCenter.png", 48, 48, NULL);
    }

    /* Fallback: next to the executable (running from the source tree) */
    if (!pixbuf) {
        gchar *exe = g_file_read_link("/proc/self/exe", NULL);
        if (exe) {
            gchar *dir = g_path_get_dirname(exe);
            gchar *icon = g_build_filename(dir, "DudesControlCenter.png", NULL);
            g_print("[GUI] Trying %s...\n", icon);
            pixbuf = gdk_pixbuf_new_from_file_at_size(icon, 48, 48, NULL);
            g_free(icon);
            g_free(dir);
            g_free(exe);
        }
    }

    if (!pixbuf) {
        g_printerr("[GUI] Failed to load tray icon from any location\n");
        return;
    }

    #pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    /* Create status icon */
    app->status_icon = gtk_status_icon_new_from_pixbuf(pixbuf);
    g_object_unref(pixbuf);

    gtk_status_icon_set_title(GTK_STATUS_ICON(app->status_icon), "Dudes Control Center");
    gtk_status_icon_set_tooltip_text(GTK_STATUS_ICON(app->status_icon), "Dudes Control Center");
#pragma GCC diagnostic pop

    /* Create context menu */
    app->tray_menu = create_tray_menu(app);

    /* Connect popup-menu signal for right-click context menu */
    g_signal_connect(app->status_icon, "popup-menu", G_CALLBACK(on_tray_popup_menu), app);

    /* Connect activate signal for left-click to toggle window visibility */
    g_signal_connect(app->status_icon, "activate", G_CALLBACK(on_tray_activate_left_click), app);
}

/* Create the tray context menu */
static GtkWidget* create_tray_menu(AppData *app) {
    g_print("[GUI] Creating tray menu with %d profiles\n", app->num_profiles);

    /* Set flag to prevent callback recursion during menu creation */
    app->updating_tray_menu = 1;

    GtkWidget *menu = gtk_menu_new();

    /* Show/Hide application */
    GtkWidget *show_item = gtk_menu_item_new_with_label(_("Show Application"));
    g_signal_connect(show_item, "activate", G_CALLBACK(on_tray_show_app), app);
    gtk_container_add(GTK_CONTAINER(menu), show_item);
    gtk_widget_show(show_item);

    /* Separator */
    GtkWidget *separator1 = gtk_separator_menu_item_new();
    gtk_container_add(GTK_CONTAINER(menu), separator1);
    gtk_widget_show(separator1);

    /* CPU Profiles submenu (alphabetical: C before F) */
    if (app->cpu_connected) {
        GtkWidget *cpu_profiles_menuitem = gtk_menu_item_new_with_label(_("CPU Profiles"));
        GtkWidget *cpu_profiles_submenu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(cpu_profiles_menuitem), cpu_profiles_submenu);
        gtk_container_add(GTK_CONTAINER(menu), cpu_profiles_menuitem);
        gtk_widget_show(cpu_profiles_menuitem);

        /* Get number of CPU profiles from the store */
        int cpu_profile_count = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(app->cpu_custom_profile_store), NULL);

        GtkWidget *first_cpu_profile_radio = NULL;
        GtkTreeIter cpu_iter;
        gboolean has_iter = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->cpu_custom_profile_store), &cpu_iter);
        int i = 0;
        while (has_iter && i < cpu_profile_count) {
            char *raw_name = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &cpu_iter, 0, &raw_name, -1);

            char label[256];
            if (raw_name) {
                const char *clean_name = cpu_profile_name_from_display(raw_name);
                if (i == app->cpu_active_custom_profile_idx) {
                    snprintf(label, sizeof(label), _("%s (Active)"), clean_name);
                } else {
                    snprintf(label, sizeof(label), "%s", clean_name);
                }
                g_free(raw_name);
            } else {
                snprintf(label, sizeof(label), _("Profile %d"), i);
            }

            g_print("[GUI] Adding CPU profile item: %s\n", label);

            GtkWidget *cpu_profile_item;
            if (first_cpu_profile_radio == NULL) {
                cpu_profile_item = gtk_radio_menu_item_new_with_label(NULL, label);
                first_cpu_profile_radio = cpu_profile_item;
            } else {
                cpu_profile_item = gtk_radio_menu_item_new_with_label_from_widget(GTK_RADIO_MENU_ITEM(first_cpu_profile_radio), label);
            }

            /* Store profile index in widget data */
            int *idx_ptr = g_malloc(sizeof(int));
            *idx_ptr = i;
            g_object_set_data(G_OBJECT(cpu_profile_item), "cpu_profile_idx", idx_ptr);

            if (i == app->cpu_active_custom_profile_idx) {
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(cpu_profile_item), TRUE);
            }
            g_signal_connect(cpu_profile_item, "activate", G_CALLBACK(on_tray_select_cpu_profile), app);
            gtk_container_add(GTK_CONTAINER(cpu_profiles_submenu), cpu_profile_item);
            gtk_widget_show(cpu_profile_item);

            /* Advance to next row */
            has_iter = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->cpu_custom_profile_store), &cpu_iter);
            i++;
        }
    }

    /* Fan Profiles submenu */
    if (!dcc_component_disabled(COMP_FAN)) {
        GtkWidget *profiles_menuitem = gtk_menu_item_new_with_label(_("Fan Profiles"));
        GtkWidget *profiles_submenu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(profiles_menuitem), profiles_submenu);
        gtk_container_add(GTK_CONTAINER(menu), profiles_menuitem);
        gtk_widget_show(profiles_menuitem);

        GtkWidget *first_profile_radio = NULL;
        for (int i = 0; i < app->num_profiles; i++) {
            char label[128];
            if (i == app->active_profile_idx) {
                snprintf(label, sizeof(label), _("%s (Active)"), app->profile_names[i]);
            } else {
                snprintf(label, sizeof(label), "%s", app->profile_names[i]);
            }

            g_print("[GUI] Adding profile item: %s\n", label);

            GtkWidget *profile_item;
            if (first_profile_radio == NULL) {
                profile_item = gtk_radio_menu_item_new_with_label(NULL, label);
                first_profile_radio = profile_item;
            } else {
                profile_item = gtk_radio_menu_item_new_with_label_from_widget(GTK_RADIO_MENU_ITEM(first_profile_radio), label);
            }

            int *idx_ptr = g_malloc(sizeof(int));
            *idx_ptr = i;
            g_object_set_data(G_OBJECT(profile_item), "profile_idx", idx_ptr);

            if (i == app->active_profile_idx) {
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(profile_item), TRUE);
            }
            g_signal_connect(profile_item, "activate", G_CALLBACK(on_tray_select_profile), app);
            gtk_container_add(GTK_CONTAINER(profiles_submenu), profile_item);
            gtk_widget_show(profile_item);
        }
    }

    /* Keyboard Profiles submenu (1.26.0; alphabetical: after Fan). Asks the
     * daemon only over an existing connection: a reconnect from here would
     * rebuild this menu while it is being built. */
    if (app->kbd_connected && app->kbd_socket_fd >= 0 && app->kbd_kind != KBD_UI_WHITE && app->kbd_kind != KBD_UI_NONE) {
        const char *r = send_keyboard_command_once(app, "LIST_PROFILES");
        if (r && strncmp(r, "PROFILES_START", 14) == 0) {
            GtkWidget *kbd_item = gtk_menu_item_new_with_label(_("Keyboard Profiles"));
            GtkWidget *kbd_submenu = gtk_menu_new();
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(kbd_item), kbd_submenu);
            gtk_container_add(GTK_CONTAINER(menu), kbd_item);
            gtk_widget_show(kbd_item);
            GtkWidget *first_radio = NULL;
            gchar **lines = g_strsplit(r, "\n", -1);
            for (int i = 0; lines[i]; i++) {
                int idx = -1;
                if (sscanf(lines[i], "PROFILE %d [", &idx) != 1) continue;
                const char *marker = strchr(lines[i], '[');
                const char *name = strstr(lines[i], "] ");
                if (!marker || !name || !name[2]) continue;
                int active = marker[1] == '*';
                gchar *clean = g_strchomp(g_strdup(name + 2));
                gchar *label = active ? g_strdup_printf(_("%s (Active)"), clean) : g_strdup(clean);
                GtkWidget *it = first_radio
                    ? gtk_radio_menu_item_new_with_label_from_widget(GTK_RADIO_MENU_ITEM(first_radio), label)
                    : gtk_radio_menu_item_new_with_label(NULL, label);
                if (!first_radio) first_radio = it;
                g_object_set_data_full(G_OBJECT(it), "kbd_profile_name", clean, g_free);
                if (active) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), TRUE);
                g_signal_connect(it, "activate", G_CALLBACK(on_tray_select_kbd_profile), app);
                gtk_container_add(GTK_CONTAINER(kbd_submenu), it);
                gtk_widget_show(it);
                g_free(label);
            }
            g_strfreev(lines);
        }
    }

    /* Separator */
    GtkWidget *separator2 = gtk_separator_menu_item_new();
    gtk_container_add(GTK_CONTAINER(menu), separator2);
    gtk_widget_show(separator2);

    /* Version label (greyed out) - wrapped in menu item */
    {
        char version_label[256];
        snprintf(version_label, sizeof(version_label), _("Version %s"), APP_VERSION);
        GtkWidget *version_menuitem = gtk_menu_item_new_with_label(version_label);
        gtk_widget_set_sensitive(version_menuitem, FALSE); /* Greyed out effect */
        gtk_container_add(GTK_CONTAINER(menu), version_menuitem);
        gtk_widget_show(version_menuitem);
    }

    /* Separator after version */
    GtkWidget *separator3 = gtk_separator_menu_item_new();
    gtk_container_add(GTK_CONTAINER(menu), separator3);
    gtk_widget_show(separator3);

    /* Exit */
    GtkWidget *exit_item = gtk_menu_item_new_with_label(_("Exit"));
    g_signal_connect(exit_item, "activate", G_CALLBACK(on_tray_exit), app);
    gtk_container_add(GTK_CONTAINER(menu), exit_item);
    gtk_widget_show(exit_item);

    /* Unset flag after menu creation */
    app->updating_tray_menu = 0;

    return menu;
}

/* Handle popup-menu signal for right-click - show context menu */
static gboolean on_tray_popup_menu(GtkStatusIcon *status_icon, guint button, guint activate_time, gpointer user_data) {
    (void)status_icon;
    (void)activate_time;
    AppData *app = (AppData *)user_data;

    g_print("[GUI] Tray popup-menu signal received, button: %d\n", button);

    if (!app->tray_menu || !GTK_IS_MENU(app->tray_menu)) {
        g_printerr("[GUI] Tray menu is NULL or invalid\n");
        return FALSE;
    }

    gtk_widget_show_all(app->tray_menu);
    gtk_menu_popup_at_pointer(GTK_MENU(app->tray_menu), NULL);

    return TRUE;
}

/* Handle activate signal for left-click - toggle window visibility */
static void on_tray_activate_left_click(GtkStatusIcon *status_icon, gpointer user_data) {
    (void)status_icon;
    AppData *app = (AppData *)user_data;

    g_print("[GUI] Tray icon activated (left-click), toggling window\n");

    if (!app->window || !GTK_IS_WIDGET(app->window)) {
        g_printerr("[GUI] Window widget is NULL or invalid\n");
        return;
    }

    if (gtk_widget_get_visible(app->window)) {
        /* Save current window position before hiding */
        gtk_window_get_position(GTK_WINDOW(app->window), &app->window_x, &app->window_y);
        app->window_width = gtk_widget_get_allocated_width(app->window);
        app->window_height = gtk_widget_get_allocated_height(app->window);
        save_window_position(app);
        g_print("[GUI] Hiding window\n");
        gtk_widget_hide(app->window);
    } else {
        /* Show all widgets in the window */
        g_print("[GUI] About to call gtk_widget_show_all\n");
        gtk_widget_show_all(app->window);
        g_print("[GUI] gtk_widget_show_all completed successfully\n");

        /* Reapply saved window position after showing — WM ignores positions on hidden windows */
        gtk_window_move(GTK_WINDOW(app->window), app->window_x, app->window_y);

        /* Force a redraw to ensure proper display after widget recreation */
        gtk_widget_queue_draw(app->window);
        g_print("[GUI] About to call gtk_window_present\n");

        /* Fetch current status from daemon to sync manual mode state before showing window */
        const char *response = send_command(app, "STATUS");
        if (response != NULL) {
            parse_status(app, response);  /* Parse STATUS to update app->in_manual_mode flag */

            /* If we're on the Fan Profiles tab, defer refresh until widgets are realized.
             * gtk_widget_show_all() triggers realization asynchronously. */
            gint current_page = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
            if (current_page == PAGE_FAN) {
                g_idle_add(refresh_profile_tab_idle, app);
            }
        }

        /* Present the window */
        gtk_window_present(GTK_WINDOW(app->window));
    }
}

/* Show application from tray menu */
static void on_tray_show_app(GtkMenuItem *menuitem, gpointer user_data) {
    (void)menuitem;
    AppData *app = (AppData *)user_data;

    if (!gtk_widget_get_visible(app->window)) {
        gtk_widget_show_all(app->window);
        /* Reapply saved window position after showing — WM ignores positions on hidden windows */
        gtk_window_move(GTK_WINDOW(app->window), app->window_x, app->window_y);
        gtk_widget_queue_draw(app->window);

        /* Fetch current status from daemon to sync manual mode state before showing window */
        const char *response = send_command(app, "STATUS");
        if (response != NULL) {
            parse_status(app, response);  /* Parse STATUS to update app->in_manual_mode flag */

            /* If we're on the Fan Profiles tab, defer refresh until widgets are realized.
             * gtk_widget_show_all() triggers realization asynchronously. */
            gint current_page = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
            if (current_page == PAGE_FAN) {
                g_idle_add(refresh_profile_tab_idle, app);
            }
        }
    }

    gtk_window_present(GTK_WINDOW(app->window));
}

/* Exit application from tray menu */
static void on_tray_exit(GtkMenuItem *menuitem, gpointer user_data) {
    (void)menuitem;
    AppData *app = (AppData *)user_data;

    /* Unsaved fan curve or CPU profile changes: ask before quitting */
    if (app->curve_points_modified) {
        int r = check_unsaved_changes(app);
        if (r < 0) return;
        if (r == 1) on_apply_clicked(NULL, app);
        app->curve_points_modified = 0;
    }
    if (cpu_check_unsaved_changes(app) < 0) return;
    if (kbd_leave_tab(app) < 0) return;

    /* Save window position before exiting if window is visible */
    if (gtk_widget_get_visible(app->window)) {
        gtk_window_get_position(GTK_WINDOW(app->window), &app->window_x, &app->window_y);
        app->window_width = gtk_widget_get_allocated_width(app->window);
        app->window_height = gtk_widget_get_allocated_height(app->window);
        save_window_position(app);
    }

    gtk_main_quit();
}

/* Select profile from tray context menu */
static void on_tray_select_profile(GtkRadioMenuItem *radiomenuitem, gpointer user_data) {
    AppData *app = (AppData *)user_data;

    /* Prevent callback during menu creation/update to avoid infinite loop */
    if (app->updating_tray_menu) {
        return;
    }

    /* Get the profile index stored in widget data */
    gpointer idx_ptr = g_object_get_data(G_OBJECT(radiomenuitem), "profile_idx");
    if (!idx_ptr) {
        g_printerr("[GUI] No profile index found in menu item\n");
        return;
    }

    int selected_idx = *(int*)idx_ptr;
    gui_event("[GUI] Tray profile selected: index %d (%s)\n", selected_idx, app->profile_names[selected_idx]);

    if (selected_idx >= 0 && selected_idx < app->num_profiles && selected_idx != app->active_profile_idx) {
        g_print("[GUI] Activating profile %d: %s\n", selected_idx, app->profile_names[selected_idx]);
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "SET_ACTIVE_PROFILE %d", selected_idx);
        const char *response = send_command(app, cmd);
        if (response && strstr(response, "OK")) {
            gui_event("[GUI] Profile activated successfully\n");
            show_status(app, g_strdup_printf(_("Profile '%s' activated"), app->profile_names[selected_idx]));

            /* Update active profile index */
            app->active_profile_idx = selected_idx;
            app->pending_active_profile_idx = selected_idx;

            /* Refresh the dropdown so "(Active)" moves */
            refresh_profile_list(app);

            /* Update tray menu to reflect new active profile */
            update_tray_menu(app);
        } else {
            show_status(app, _("Failed to activate profile"));
        }
    }
}

/* Callback: select a keyboard profile from the system tray menu (1.26.0) */
static void on_tray_select_kbd_profile(GtkMenuItem *item, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    if (app->updating_tray_menu) return;
    if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item))) return;   /* the one switched off */
    const char *name = g_object_get_data(G_OBJECT(item), "kbd_profile_name");
    if (!name) return;
    char pname[256];
    snprintf(pname, sizeof pname, "%s", name);

    /* While the Keyboard tab edits a profile: its unsaved changes first */
    int on_kbd_tab = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook)) == PAGE_KEYBOARD;
    if (on_kbd_tab && kbd_check_unsaved_changes(app) < 0) {
        update_tray_menu(app);              /* radio back to the active profile */
        return;
    }
    char cmd[512];
    snprintf(cmd, sizeof cmd, "LOAD_PROFILE::%s", pname);
    const char *r = send_keyboard_command(app, cmd);
    int ok = r && strncmp(r, "OK", 2) == 0;
    gui_event("[GUI] Tray keyboard profile '%s': %s\n", pname, r ? r : "NULL");
    if (on_kbd_tab) {
        load_keyboard_profiles_select(app, pname);
        if (ok && kbd_load_edit_profile(app, pname) == 0) kbd_edit_changed(app, 0, 0);
    }
    kbd_sync_power_switch(app);
    show_status(app, ok ? _("Keyboard profile activated") : _("Failed to activate keyboard profile"));
    update_tray_menu(app);
}

/* Callback: select a CPU profile from the system tray menu */
static void on_tray_select_cpu_profile(GtkRadioMenuItem *radiomenuitem, gpointer user_data) {
    AppData *app = (AppData *)user_data;

    /* Prevent callback during menu creation/update to avoid infinite loop */
    if (app->updating_tray_menu) {
        return;
    }

    /* Get the profile index stored in widget data */
    gpointer idx_ptr = g_object_get_data(G_OBJECT(radiomenuitem), "cpu_profile_idx");
    if (!idx_ptr) {
        g_printerr("[GUI] No CPU profile index found in menu item\n");
        return;
    }

    int selected_idx = *(int*)idx_ptr;

    /* Get the profile name from the store */
    char *profile_name = NULL;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter)) {
        if (selected_idx == 0) {
            gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter, 0, &profile_name, -1);
        } else {
            for (int k = 1; k < selected_idx; k++) {
                if (!gtk_tree_model_iter_next(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter))
                    break;
            }
            gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter, 0, &profile_name, -1);
        }
    }

    gui_event("[GUI] Tray CPU profile selected: index %d (%s)\n", selected_idx, profile_name ? profile_name : "(null)");

    if (selected_idx >= 0 && selected_idx != app->cpu_active_custom_profile_idx) {
        g_print("[GUI] Activating CPU profile %d\n", selected_idx);
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "APPLY_CUSTOM_PROFILE|%d", selected_idx);
        const char *response = send_cpu_command(app, cmd);
        if (response && strncmp(response, "OK", 2) == 0) {
            gui_event("[GUI] CPU profile activated successfully\n");
            show_status(app, g_strdup_printf(_("CPU profile '%s' activated"), profile_name ? profile_name : _("Unknown")));

            /* Update active index */
            app->cpu_active_custom_profile_idx = selected_idx;

            /* Reload profile list to update markers */
            load_cpu_custom_profiles(app);

            /* Persist the active_profile_index */
            gui_save_profiles_to_disk(app);

            /* Update tray menu to reflect new active profile */
            update_tray_menu(app);
        } else {
            show_status(app, _("Failed to activate CPU profile"));
        }
    }

    if (profile_name) g_free(profile_name);
}
