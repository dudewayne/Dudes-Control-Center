// SPDX-License-Identifier: GPL-2.0-only
/*
 * gui-fan.c
 * Fan Profiles tab: curve editor and graph, fan profiles, manual fan speed.
 * Part of dcc-gui.c, which #includes it: one translation unit, so the
 * static functions and AppData stay shared (split up in 1.48.1).
 */


/* Combined idle callback: refresh profile list + update manual mode UI (defer until GTK realizes widgets) */
static gboolean refresh_profile_tab_idle(gpointer data) {
    AppData *app = (AppData *)data;
    refresh_profile_list(app);
    update_profile_tab_for_manual_mode(app, app->in_manual_mode);
    return FALSE; /* Don't repeat */
}


/* Check for unsaved curve point changes and show warning dialog if needed */
/* Returns: 1 if user chose to apply changes, 0 if discard */
static int check_unsaved_changes(AppData *app) {
    if (!app->curve_points_modified) {
        return 1; /* No unsaved changes, proceed normally */
    }

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_WARNING,
                                                GTK_BUTTONS_NONE,
                                                NULL);

    gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(dialog),
                                  _("<b>Warning:</b> You have unsaved changes to the curve points.\n\nWhat would you like to do?"));

    /* Add buttons with custom response IDs */
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Cancel"), -102);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Discard Changes"), -100);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Save Changes"), -101);

    gtk_widget_show_all(dialog);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == -101) {
        return 1; /* Apply changes */
    } else if (response == -100) {
        return 0; /* Discard changes */
    } else {
        return -1; /* Cancel or X clicked - abort the action */
    }
}

/* Callback: Manual mode switch toggled */
static gboolean on_manual_mode_switched(GtkSwitch *switch_widget, gboolean new_state, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int is_on = new_state; /* Use the new_state parameter from state-set signal */

    const char *response;
    char ts[32];
    get_timestamp(ts, sizeof(ts)); /* Initialize timestamp first! */

    g_print("%s [GUI] >>> on_manual_mode_switched called: is_on=%d, app->in_manual_mode=%d, app->auto_mode_enabled=%d <<<\n", ts, is_on, app->in_manual_mode, app->auto_mode_enabled);

    if (is_on) {
        /* Enable manual mode - disable auto mode */
        g_print("%s [GUI] Sending SET_AUTO 0 to daemon\n", ts);
        response = send_command(app, "SET_AUTO 0");
        g_print("%s [GUI] SET_AUTO 0 response: %s\n", ts, response);
        app->auto_mode_enabled = 0;

        /* Store the current profile index before entering manual mode */
        app->profile_before_manual = app->active_profile_idx;
        g_print("%s [GUI] Stored profile_before_manual = %d\n", ts, app->profile_before_manual);

        gtk_widget_set_sensitive(app->manual_minus_btn, TRUE);
        gtk_widget_set_sensitive(app->manual_plus_btn, TRUE);
        /* Fetch fresh status to get current fan speed */
        const char *status_response = send_command(app, "STATUS");
        int actual_speed = 50; /* Default fallback */
        if (status_response != NULL) {
            /* Parse FAN_DUTY from status response - make a copy since strtok modifies the string */
            char status_copy[BUFFER_SIZE];
            strncpy(status_copy, status_response, sizeof(status_copy) - 1);
            status_copy[sizeof(status_copy) - 1] = '\0';
            char *token = strtok(status_copy, " \t\n");
            while (token != NULL) {
                if (strncmp(token, "FAN_DUTY=", 9) == 0) {
                    actual_speed = atoi(token + 9);
                    break;
                }
                token = strtok(NULL, " \t\n");
            }

        }
        app->last_known_fan_duty = actual_speed; /* Cache for future use */
        g_print("%s [GUI] DEBUG: Current fan speed from STATUS = %d\n", ts, actual_speed);
        /* Round up to nearest 10% for cleaner manual mode start */
        app->manual_current_speed = ((actual_speed + 9) / 10) * 10;
        if (app->manual_current_speed > 100) app->manual_current_speed = 100;
        g_print("%s [GUI] DEBUG: Rounded manual_current_speed = %d\n", ts, app->manual_current_speed);
        char speed_text[16];
        snprintf(speed_text, sizeof(speed_text), "%d%%", app->manual_current_speed);
        gtk_label_set_text(GTK_LABEL(app->manual_speed_label), speed_text);
        /* Explicitly set the switch state to ensure visual update */
        g_print("%s [GUI] Blocking signal handler and setting switch to TRUE\n", ts);
        g_signal_handler_block(app->manual_mode_switch, app->manual_mode_signal_id);
        gtk_switch_set_state(switch_widget, TRUE);
        g_signal_handler_unblock(app->manual_mode_switch, app->manual_mode_signal_id);
        g_print("%s [GUI] Signal handler unblocked, switch state after set: %d\n", ts, gtk_switch_get_active(GTK_SWITCH(switch_widget)));

        /* Track that we're in manual mode */
        app->in_manual_mode = 1;

        /* Update the profile tab UI for manual mode */
        update_profile_tab_for_manual_mode(app, 1);

        get_timestamp(ts, sizeof(ts));
        gui_event("%s [GUI] Manual mode ENABLED, app->in_manual_mode=%d, app->auto_mode_enabled=%d\n", ts, app->in_manual_mode, app->auto_mode_enabled);
    } else {
        /* Disable manual mode - enable auto mode */
        response = send_command(app, "SET_AUTO 1");
        app->auto_mode_enabled = 1;
        gtk_widget_set_sensitive(app->manual_minus_btn, FALSE);
        gtk_widget_set_sensitive(app->manual_plus_btn, FALSE);
        gtk_label_set_text(GTK_LABEL(app->manual_speed_label), "--");
        /* Explicitly set the switch state to ensure visual update */
        g_signal_handler_block(app->manual_mode_switch, app->manual_mode_signal_id);
        gtk_switch_set_state(switch_widget, FALSE);
        g_signal_handler_unblock(app->manual_mode_switch, app->manual_mode_signal_id);

        /* Track that we're no longer in manual mode */
        app->in_manual_mode = 0;

        /* Update the profile tab UI for manual mode */
        update_profile_tab_for_manual_mode(app, 0);

        get_timestamp(ts, sizeof(ts));
        gui_event("%s [GUI] Manual mode DISABLED (auto mode enabled)\n", ts);
    }

    show_status(app, response);

    return TRUE; /* Accept the state change */
}

/* Next manual fan speed for the +/- buttons: 10% steps from 0% (fans
 * stopped) to 100%. The fan daemon's fail-safes still switch the fans to
 * 100% at 95 degC or when a temperature sensor fails, also at 0%. */
static int manual_speed_step(int current_pct, int delta) {
    int next = current_pct + delta;
    if (delta < 0 && current_pct % 10 != 0) next = (current_pct / 10) * 10;  /* 37 -> 30 */
    if (next < 0) next = 0;
    if (next > 100) next = 100;
    return next;
}

static void apply_manual_speed(AppData *app, int pct) {
    if (pct == app->manual_current_speed) return;
    app->manual_current_speed = pct; /* Update state immediately */

    char speed_text[16];
    snprintf(speed_text, sizeof(speed_text), "%d%%", pct);
    gtk_label_set_text(GTK_LABEL(app->manual_speed_label), speed_text);

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "SET_MANUAL %d", pct);
    const char *response = send_command(app, cmd);
    if (pct == 0 && response && strncmp(response, "OK", 2) == 0)
        show_status(app, _("Fans stopped (0%) - they switch to 100% automatically at 95°C or on a sensor error"));
    else
        show_status(app, response);
}

/* Callback: Manual speed + button clicked */
static void on_manual_speed_plus_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;
    AppData *app = (AppData *)user_data;
    apply_manual_speed(app, manual_speed_step(app->manual_current_speed, +10));
}

/* Callback: Manual speed - button clicked (down to 0% = fans off) */
static void on_manual_speed_minus_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;
    AppData *app = (AppData *)user_data;
    apply_manual_speed(app, manual_speed_step(app->manual_current_speed, -10));
}

/* Callback: Apply curve changes - save reordered curve points to daemon */
static void on_apply_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;

    int profile_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->profile_combo));
    if (profile_idx < 0) {
        show_status(app, _("ERROR No profile selected"));
        return;
    }

    /* Collect all curve points from the tree view */
    GtkTreeModel *model = GTK_TREE_MODEL(app->curve_store);
    GtkTreeIter iter;
    int num_points = 0;
    int temps[50], fan_pcts[50];

    if (gtk_tree_model_get_iter_first(model, &iter)) {
        do {
            gchar *temp_str;
            gint fan_pct;
            gtk_tree_model_get(model, &iter, 0, &temp_str, 1, &fan_pct, -1);
            temps[num_points] = atoi(temp_str);
            fan_pcts[num_points] = fan_pct;
            g_free(temp_str);
            num_points++;
        } while (gtk_tree_model_iter_next(model, &iter) && num_points < 50);
    }

    /* Build the SET_CURVE_POINTS command */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "SET_CURVE_POINTS %d %d", profile_idx, num_points);

    /* Append all points as temp,fan_pct,temp,fan_pct,... */
    for (int i = 0; i < num_points; i++) {
        char point_str[32];
        snprintf(point_str, sizeof(point_str), ",%d,%d", temps[i], fan_pcts[i]);
        strncat(cmd, point_str, sizeof(cmd) - strlen(cmd) - 1);
    }

    const char *response = send_command(app, cmd);
    show_status(app, response);

    /* Also save profiles to disk (GUI writes directly) */
    gui_save_profiles_to_disk(app);

    /* Clear the modified flag since changes are now saved */
    app->curve_points_modified = 0;

    /* Reload curve points to ensure UI matches daemon state */
    load_curve_points_for_profile(app, profile_idx);
}

/* Callback: Create new profile */
static void on_create_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;

    /* Handle unsaved curve changes before profile creation to prevent
     * nested modal dialogs when the combo box signal fires during refresh. */
    if (app->curve_points_modified) {
        int result = check_unsaved_changes(app);
        if (result == 1) {
            on_apply_clicked(NULL, app);
        } else if (result == 0) {
            app->curve_points_modified = 0;
        } else {
            return; /* User cancelled */
        }
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Create New Profile"),
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL,
                                                    _("Cancel"), GTK_RESPONSE_CANCEL,
                                                    _("Create"), GTK_RESPONSE_ACCEPT,
                                                    NULL);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), _("Profile name"));

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
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
            for (int k = 0; k < app->num_profiles; k++) {
                if (strcmp(app->profile_names[k], name) == 0) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) {
                GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                                GTK_DIALOG_MODAL,
                                                                GTK_MESSAGE_ERROR,
                                                                GTK_BUTTONS_OK,
                                                                _("A profile with the name '%s' already exists."),
                                                                name);
                gtk_dialog_run(GTK_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
                gtk_widget_destroy(dialog);
                return;
            }
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "CREATE_PROFILE %s", name);
            const char *response = send_command(app, cmd);
            show_status(app, response);

            /* Refresh profile list and tray menu, then switch to the new profile */
            refresh_profile_list(app);
            update_tray_menu(app);

            /* Find and select the newly created profile */
            int new_idx = -1;
            for (int k = 0; k < app->num_profiles; k++) {
                if (strcmp(app->profile_names[k], name) == 0) {
                    new_idx = k;
                    break;
                }
            }
            if (new_idx >= 0) {
                g_signal_handler_block(app->profile_combo, app->profile_combo_signal_id);
                gtk_combo_box_set_active(GTK_COMBO_BOX(app->profile_combo), new_idx);
                g_signal_handler_unblock(app->profile_combo, app->profile_combo_signal_id);
                load_curve_points_for_profile(app, new_idx);
            }
        } else {
            show_status(app, _("ERROR Profile name cannot be empty"));
        }
    }
    gtk_widget_destroy(dialog);
}

/* Callback: Delete selected profile */

static void on_delete_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget; /* Unused */
    AppData *app = (AppData *)user_data;

    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->profile_combo));
    if (idx < 0 || idx >= app->num_profiles) {
        show_status(app, _("ERROR No profile selected"));
        return;
    }

    const char *profile_name = app->profile_names[idx];

    /* Don't allow deleting the Default profile */
    if (strcmp(profile_name, "Default") == 0) {
        show_status(app, _("ERROR Cannot delete Default profile"));
        return;
    }

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_QUESTION,
                                               GTK_BUTTONS_YES_NO,
                                               _("Are you sure you want to delete the profile '%s'?"), profile_name);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "DELETE_PROFILE %d", idx);
        const char *response = send_command(app, cmd);
        show_status(app, response);

        /* Refresh profile list and tray menu */
        refresh_profile_list(app);
        update_tray_menu(app);

        /* Reload curve points for the new active profile */

        int new_active = gtk_combo_box_get_active(GTK_COMBO_BOX(app->profile_combo));
        if (new_active >= 0) {
            load_curve_points_for_profile(app, new_active);
        }
    }

    gtk_widget_destroy(dialog);
}

/* Callback: Rename selected profile */
static void on_rename_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;

    /* Handle unsaved curve changes before showing rename dialog */
    if (app->curve_points_modified) {
        int result = check_unsaved_changes(app);
        if (result == 1) {
            on_apply_clicked(NULL, app);
        } else if (result == 0) {
            app->curve_points_modified = 0;
        } else {
            return; /* User cancelled */
        }
    }

    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->profile_combo));
    if (idx < 0 || idx >= app->num_profiles) {
        show_status(app, _("ERROR No profile selected"));
        return;
    }

    const char *profile_name = app->profile_names[idx];

    /* Don't allow renaming the Default profile */
    if (strcmp(profile_name, "Default") == 0) {
        show_status(app, _("ERROR Cannot rename Default profile"));
        return;
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Rename Profile"),
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL,
                                                    _("Cancel"), GTK_RESPONSE_CANCEL,
                                                    _("Rename"), GTK_RESPONSE_ACCEPT,
                                                    NULL);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), profile_name);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_pack_start(GTK_BOX(content), entry, TRUE, TRUE, 0);
    gtk_widget_show_all(content);

    /* Set Accept as default so Enter key triggers rename */
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const gchar *new_name = gtk_entry_get_text(GTK_ENTRY(entry));
        if (strlen(new_name) > 0) {
            /* Send rename command to daemon */
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "RENAME_PROFILE %d %s", idx, new_name);
            const char *response = send_command(app, cmd);
            show_status(app, response);

            /* Refresh profile list and tray menu */
            refresh_profile_list(app);
            update_tray_menu(app);

            /* Persist the renamed profile to profiles.json */
            gui_save_profiles_to_disk(app);

            /* Reload curve points for the renamed profile */
            int new_active = gtk_combo_box_get_active(GTK_COMBO_BOX(app->profile_combo));
            if (new_active >= 0) {
                load_curve_points_for_profile(app, new_active);
            }
        } else {
            show_status(app, _("ERROR Profile name cannot be empty"));
        }
    }
    gtk_widget_destroy(dialog);
}

/* Helper function: Check if temperature already exists in curve store
 * Returns 1 if a duplicate is found (excluding the row at index_to_exclude)
 */
static int has_duplicate_temperature(AppData *app, int temp, gint index_to_exclude) {
    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(app->curve_store);
    gint current_index = 0;

    if (gtk_tree_model_get_iter_first(model, &iter)) {
        do {
            /* Skip the row we're currently editing */
            if (index_to_exclude >= 0 && current_index == index_to_exclude) {
                current_index++;
                continue;
            }

            gchar *temp_str;
            gtk_tree_model_get(model, &iter, 0, &temp_str, -1);
            /* Parse temperature from string like "50°C" */
            int existing_temp = atoi(temp_str);
            g_free(temp_str);
            if (existing_temp == temp) {
                return 1; /* Duplicate found */
            }
            current_index++;
        } while (gtk_tree_model_iter_next(model, &iter));
    }
    return 0; /* No duplicate */
}

/* Add a point (edit_index < 0) or change the point in row edit_index - its
 * temperature and fan speed - and show the points sorted by temperature.
 * Editing used to keep the old temperature, so a changed temperature did
 * nothing (fixed in 1.48.2). */
static void curve_point_apply(AppData *app, gint edit_index, int temp, int fan_pct) {
    CurvePoint all_points[MAX_CURVE_POINTS];
    int point_count = 0;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->curve_store), &iter)) {
        do {
            gchar *temp_str;
            int fpct;
            gtk_tree_model_get(GTK_TREE_MODEL(app->curve_store), &iter, 0, &temp_str, 1, &fpct, -1);
            if (point_count < MAX_CURVE_POINTS) {
                all_points[point_count].temp = atoi(temp_str);
                all_points[point_count].fan_pct = fpct;
                point_count++;
            }
            g_free(temp_str);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->curve_store), &iter));
    }

    if (edit_index >= 0 && edit_index < point_count) {
        all_points[edit_index].temp = temp;
        all_points[edit_index].fan_pct = fan_pct;
    } else if (edit_index < 0 && point_count < MAX_CURVE_POINTS) {
        all_points[point_count].temp = temp;
        all_points[point_count].fan_pct = fan_pct;
        point_count++;
    }

    if (point_count > 1)
        qsort(all_points, point_count, sizeof(CurvePoint), compare_curve_points_asc);

    gtk_list_store_clear(GTK_LIST_STORE(app->curve_store));
    for (int i = 0; i < point_count; i++) {
        char temp_str[16];
        snprintf(temp_str, sizeof(temp_str), "%d°C", all_points[i].temp);
        gtk_list_store_append(GTK_LIST_STORE(app->curve_store), &iter);
        gtk_list_store_set(GTK_LIST_STORE(app->curve_store), &iter, 0, temp_str, 1, all_points[i].fan_pct, -1);
    }
}

/* Callback: Add or edit curve point via dialog */
static void show_curve_point_dialog(AppData *app, GtkTreeIter *edit_iter, int edit_temp, int edit_fan_pct) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        edit_iter ? _("Edit Curve Point") : _("Add Curve Point"),
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL,
        _("Cancel"), GTK_RESPONSE_CANCEL,
        edit_iter ? _("Update") : _("Add"),
        GTK_RESPONSE_ACCEPT,
        NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 20);

    GtkWidget *temp_label = gtk_label_new(_("Temperature (°C):"));
    GtkWidget *temp_entry = gtk_spin_button_new_with_range(0, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(temp_entry), edit_temp);

    GtkWidget *fan_label = gtk_label_new(_("Fan Speed (%)"));
    GtkWidget *fan_entry = gtk_spin_button_new_with_range(0, 100, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(fan_entry), edit_fan_pct);

    gtk_grid_attach(GTK_GRID(grid), temp_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), temp_entry, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), fan_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), fan_entry, 1, 1, 1, 1);

    /* Connect Enter key to trigger Add/Update button */
    g_signal_connect(temp_entry, "key-press-event", G_CALLBACK(on_curve_point_dialog_key_press), dialog);
    g_signal_connect(fan_entry, "key-press-event", G_CALLBACK(on_curve_point_dialog_key_press), dialog);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);
    gtk_widget_show_all(content);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));

    /* Handle Cancel or X click - just close dialog without changes */
    if (response != GTK_RESPONSE_ACCEPT) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Curve point dialog cancelled\n", ts);
        gtk_widget_destroy(dialog);
        return;
    }

    /* User clicked Add/Update - proceed with the operation. A value typed
     * without Enter is taken over first. */
    gtk_spin_button_update(GTK_SPIN_BUTTON(temp_entry));
    gtk_spin_button_update(GTK_SPIN_BUTTON(fan_entry));
    int temp = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(temp_entry));
    int fan_pct = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(fan_entry));

    /* Get row index for exclusion when editing */
    gint edit_index = -1;
    if (edit_iter) {
        GtkTreePath *path = gtk_tree_model_get_path(
            GTK_TREE_MODEL(app->curve_store), edit_iter);
        if (path) {
            edit_index = gtk_tree_path_get_indices(path)[0];
            gtk_tree_path_free(path);
        }
    }

    /* Check for duplicate temperature (excluding the current row when editing) */
    if (has_duplicate_temperature(app, temp, edit_index)) {
        GtkWidget *msg_dialog = gtk_message_dialog_new(
            GTK_WINDOW(app->window),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            _("A curve point with temperature %d°C already exists. Please use a different temperature."),
            temp);
        gtk_dialog_run(GTK_DIALOG(msg_dialog));
        gtk_widget_destroy(msg_dialog);
        gtk_widget_destroy(dialog);
        return;
    }

    curve_point_apply(app, edit_index, temp, fan_pct);

    if (edit_iter) {
        show_status(app, _("Curve point updated (click Save to keep it)"));
    } else {
        show_status(app, _("Curve point added (click Save to keep it)"));
    }

    /* Mark curve points as modified */
    app->curve_points_modified = 1;

    /* Update the graph visualization */
    update_curve_graph(app);

    gtk_widget_destroy(dialog);
}

/* Key press handler for curve point dialog - handles Enter key */
static gboolean on_curve_point_dialog_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)widget; /* Unused */
    GtkWidget *dialog = GTK_WIDGET(user_data);

    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        /* Commit the current entry value before responding */
        gtk_spin_button_update(GTK_SPIN_BUTTON(widget));
        gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        return TRUE; /* Event handled */
    }
    return FALSE; /* Let other handlers process the event */
}

/* Callback: Add curve point */
static void on_add_curve_point_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget; /* Unused */
    AppData *app = (AppData *)user_data;
    show_curve_point_dialog(app, NULL, 50, 30); /* Default values for new point */
}

/* Callback: Double-click to edit curve point */
static void on_curve_point_activated(GtkTreeView *treeview, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data) {
    (void)treeview;
    (void)column; /* Unused - we don't care which column was clicked */
    AppData *app = (AppData *)user_data;

    GtkTreeModel *model = GTK_TREE_MODEL(app->curve_store);
    GtkTreeIter iter;

    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gchar *temp_str;
        int fan_pct;
        gtk_tree_model_get(model, &iter, 0, &temp_str, 1, &fan_pct, -1);

        /* Parse temperature from string like "50°C" */
        int temp = atoi(temp_str);
        g_free(temp_str);

        /* Show edit dialog with current values */
        show_curve_point_dialog(app, &iter, temp, fan_pct);
    }
}

/* Callback: Delete selected curve point */
static void on_delete_curve_point_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;

    /* Get selected row from tree view */
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->curve_treeview));
    GtkTreeModel *model;
    GtkTreeIter iter;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        /* Remove the selected row from the tree view (local only, not saved yet) */
        gtk_list_store_remove(app->curve_store, &iter);

        show_status(app, _("Curve point deleted (click Save to keep it)"));

        /* Mark curve points as modified */
        app->curve_points_modified = 1;

        /* Update the graph visualization */
        update_curve_graph(app);
    } else {
        show_status(app, _("Please select a curve point to delete"));
    }
}



/* Refresh profile list from daemon */
static void refresh_profile_list(AppData *app) {
    if (!app->connected || app->socket_fd < 0) {
        return;
    }

    char ts[32];
    get_timestamp(ts, sizeof(ts));
    g_print("%s [GUI] refresh_profile_list called\n", ts);

    /* Re-entry guard: prevent concurrent calls */
    if (app->refreshing_profile_list) {
        g_print("%s [GUI] -> Already refreshing, returning early\n", ts);
        return;
    }

    /* Set flag to prevent callback recursion during refresh */
    app->refreshing_profile_list = 1;
    g_print("%s [GUI] -> Set refreshing_profile_list=1\n", ts);


    char buffer[BUFFER_SIZE];

    /* Free old profile names */
    if (app->profile_names) {
        for (int i = 0; i < app->num_profiles; i++) {
            g_free(app->profile_names[i]);
        }
        g_free(app->profile_names);
        app->profile_names = NULL;
    }

    /* Remember the selected profile, so a refresh does not jump to the active one */
    int keep_idx = app->profile_combo ? gtk_combo_box_get_active(GTK_COMBO_BOX(app->profile_combo)) : -1;

    /* Clear the combo box (blocked: clearing would emit "changed") */
    if (app->profile_combo) {
        g_signal_handler_block(app->profile_combo, app->profile_combo_signal_id);
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->profile_combo));
        g_signal_handler_unblock(app->profile_combo, app->profile_combo_signal_id);
    }

    const char *response = send_command(app, "LIST_PROFILES");

    /* Check for error responses */
    if (strncmp(response, "ERROR", 5) == 0) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Failed to refresh profile list: %s\n", ts, response);
        app->refreshing_profile_list = 0;
        return;
    }

    strncpy(buffer, response, BUFFER_SIZE - 1);
    buffer[BUFFER_SIZE - 1] = '\0';

    /* Make a copy for parsing (strtok modifies the string) */
    char buffer_copy[BUFFER_SIZE];
    strncpy(buffer_copy, buffer, BUFFER_SIZE - 1);
    buffer_copy[BUFFER_SIZE - 1] = '\0';

    /* Parse response - first count profiles */
    app->num_profiles = 0;
    char *saveptr;
    char *line = strtok_r(buffer_copy, "\n", &saveptr);
    while (line != NULL) {
        if (strncmp(line, "PROFILE ", 8) == 0) {
            app->num_profiles++;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* Allocate profile names array */
    if (app->num_profiles > 0) {
        app->profile_names = g_new0(char*, app->num_profiles);
    } else {
        app->profile_names = NULL;
    }

    /* Parse response again to fill the array using original buffer */
    line = strtok_r(buffer, "\n", &saveptr);
    int idx = 0;
    app->active_profile_idx = -1; /* Reset to find active profile */
    while (line != NULL) {
        if (strncmp(line, "PROFILE ", 8) == 0) {
            /* Parse: PROFILE <index> [<marker>] <name>
             * Use manual parsing instead of sscanf to support names with spaces. */
            int profile_num = -1;
            int is_active = 0;
            const char *name_start = NULL;

            /* Skip "PROFILE " prefix */
            const char *p = line + 8;

            /* Parse profile index */
            while (*p >= '0' && *p <= '9') {
                if (profile_num < 0) profile_num = 0;
                profile_num = profile_num * 10 + (*p - '0');
                p++;
            }

            /* Skip space before bracket */
            if (*p == ' ') p++;

            /* Check for [*] (active) or [ ] (inactive) */
            if (*p == '[') {
                is_active = (*(p + 1) == '*') ? 1 : 0;
                /* Skip past "] " to reach the name */
                p += 3;
            }
            /* Skip leading space before the name */
            if (*p == ' ') p++;

            /* Everything remaining is the profile name (may contain spaces) */
            name_start = p;

            if (profile_num >= 0 && name_start != NULL && *name_start != '\0') {
                if (is_active) {
                    app->active_profile_idx = profile_num;
                }

                app->profile_names[idx] = g_strdup(name_start);
                idx++;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    g_print("[GUI] Refreshed profile list: %d profiles, active_profile_idx=%d\n", app->num_profiles, app->active_profile_idx);

    /* Fill the dropdown; the active profile is marked "(Active)" */
    if (app->profile_combo && GTK_IS_COMBO_BOX(app->profile_combo)) {
        g_signal_handler_block(app->profile_combo, app->profile_combo_signal_id);
        for (int i = 0; i < app->num_profiles; i++) {
            if (i == app->active_profile_idx) {
                gchar *t = g_strdup_printf(_("%s (Active)"), app->profile_names[i]);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->profile_combo), t);
                g_free(t);
            } else {
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->profile_combo), app->profile_names[i]);
            }
        }
        /* Stay on the selected profile; the active one on the first load */
        int sel = (keep_idx >= 0 && keep_idx < app->num_profiles) ? keep_idx : app->active_profile_idx;
        if (sel >= 0 && sel < app->num_profiles)
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->profile_combo), sel);
        g_signal_handler_unblock(app->profile_combo, app->profile_combo_signal_id);
    }

    app->refreshing_profile_list = 0;
    update_profile_tab_for_manual_mode(app, app->in_manual_mode);

    g_print("%s [GUI] refresh_profile_list completed successfully\n", ts);
}

/* Callback when profile combo box changes */
static void on_profile_combo_changed(GtkWidget *widget, gpointer user_data) {
    AppData *app = (AppData *)user_data;

    /* Validate app pointer */
    if (!app || !GTK_IS_COMBO_BOX(widget)) {
        g_print("[GUI] ERROR: Invalid app or widget in on_profile_combo_changed\n");
        return;
    }

    char ts[32];
    get_timestamp(ts, sizeof(ts));
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));

    /* Don't load curve points if we're currently refreshing the profile list */
    if (app->refreshing_profile_list) {
        return;
    }

    /* Additional safety: ensure we're not in the middle of a page switch */
    if (app->switching_page) {
        return;
    }

    /* Check for unsaved changes before switching profiles */
    if (app->curve_points_modified) {
        int result = check_unsaved_changes(app);
        if (result == 1) {
            /* User chose to apply changes - save and proceed with profile switch */
            on_apply_clicked(NULL, app);
        } else if (result == 0) {
            /* User chose to discard - reload current profile but continue with switch */
            load_curve_points_for_profile(app, app->active_profile_idx);
            app->curve_points_modified = 0;
            /* Don't reset combo box - let the profile switch proceed */
        } else {
            /* User clicked Cancel - abort profile switch, keep curve points as-is */
            g_print("%s [GUI] -> Profile switch cancelled by user\n", ts);
            /* Don't reload anything - keep the unsaved changes visible in GUI */
            /* The flag stays set so we can warn again if they try to leave */

            /* Reset combo box to current active profile */
            g_signal_handler_block(widget, app->profile_combo_signal_id);
            gtk_combo_box_set_active(GTK_COMBO_BOX(widget), app->active_profile_idx);
            g_signal_handler_unblock(widget, app->profile_combo_signal_id);

            return;
        }
    }

    get_timestamp(ts, sizeof(ts)); /* Get fresh timestamp */
    g_print("%s [GUI] -> DEBUG: After refreshing check, continuing\n", ts);

    if (idx >= 0) {
        load_curve_points_for_profile(app, idx);
    } else {
        g_print("%s [GUI] -> Index is -1, not loading curve points\n", ts);
    }

    g_print("%s [GUI] -> on_profile_combo_changed completed normally\n", ts);
}

/* Save all profiles (fan + CPU custom) to profiles.json directly from the GUI */
static void gui_save_profiles_to_disk(AppData *app) {
    char profiles_path[512];
    const char *home = getenv("HOME");
    if (!home) {
        g_print("[GUI] WARNING: Cannot determine HOME, skipping profile save\n");
        return;
    }
    snprintf(profiles_path, sizeof(profiles_path), "%s/.config/dudes-control-center/profiles.json", home);

    /* Ensure directory exists */
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/.config/dudes-control-center", home);
    mkdir(dir_path, 0755);

    /* Start from the existing file so sections owned by others
     * (keyboard_profiles, future keys) are preserved */
    cJSON *root = NULL;
    gchar *existing = NULL;
    gsize existing_len = 0;
    GError *read_err = NULL;
    if (g_file_get_contents(profiles_path, &existing, &existing_len, &read_err)) {
        root = cJSON_Parse(existing);
        g_free(existing);
        if (!root) {
            LOG_ERROR("Profile save aborted: %s is not valid JSON (not overwriting it, to avoid data loss)",
                      profiles_path);
            return;
        }
    } else {
        if (!g_error_matches(read_err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            LOG_ERROR("Profile save aborted: cannot read %s: %s", profiles_path, read_err->message);
            g_error_free(read_err);
            return;
        }
        g_error_free(read_err);
        root = cJSON_CreateObject();
        if (!root) {
            LOG_ERROR("Failed to create JSON root for profile save");
            return;
        }
    }
    if (!cJSON_GetObjectItem(root, "version")) {
        cJSON_AddStringToObject(root, "version", "1.0");
    }

    /* Build fan_profiles section (nested object) */
    cJSON *fan_section = cJSON_CreateObject();
    cJSON_AddNumberToObject(fan_section, "active_profile_index", app->active_profile_idx);

    /* Query fan daemon for all profiles. Only replace the saved fan section
     * if every profile could be read; a partial list would lose profiles. */
    int fan_complete = (app->connected && app->num_profiles > 0);
    cJSON *fan_profiles_array = cJSON_CreateArray();
    for (int i = 0; i < app->num_profiles; i++) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "GET_PROFILE %d", i);
        const char *response = send_command(app, cmd);
        if (!response || strncmp(response, "ERROR", 5) == 0) {
            fan_complete = 0;
            continue;
        }

        /* Parse PROFILE_INFO line: "PROFILE_INFO name=XXX points=N active=N" */
        char *resp_copy = g_strdup(response);
        char *line = strtok(resp_copy, "\n");
        char profile_name[256] = "";

        while (line) {
            if (strncmp(line, "PROFILE_INFO", 12) == 0) {
                char *name_ptr = strstr(line, "name=");
                if (name_ptr) {
                    name_ptr += 5;
                    /* Use " points=" as delimiter to support names with spaces */
                    char *end = strstr(name_ptr, " points=");
                    if (end) {
                        int len = end - name_ptr;
                        if (len > 0 && len < (int)sizeof(profile_name)) {
                            strncpy(profile_name, name_ptr, len);
                            profile_name[len] = '\0';
                        } else {
                            strncpy(profile_name, name_ptr, sizeof(profile_name) - 1);
                        }
                    } else {
                        strncpy(profile_name, name_ptr, sizeof(profile_name) - 1);
                    }
                }
            }
            line = strtok(NULL, "\n");
        }

        /* Parse CURVE_POINT lines */
        cJSON *curve_array = cJSON_CreateArray();
        g_free(resp_copy);
        resp_copy = g_strdup(response);
        line = strtok(resp_copy, "\n");
        while (line) {
            if (strncmp(line, "CURVE_POINT", 11) == 0) {
                int temp = 0, fan_pct = 0;
                char *temp_ptr = strstr(line, "temp=");
                char *fan_ptr = strstr(line, "fan_pct=");
                if (temp_ptr) temp = atoi(temp_ptr + 5);
                if (fan_ptr) fan_pct = atoi(fan_ptr + 8);

                cJSON *point = cJSON_CreateObject();
                cJSON_AddNumberToObject(point, "temperature", temp);
                cJSON_AddNumberToObject(point, "fan_percent", fan_pct);
                cJSON_AddItemToArray(curve_array, point);
            }
            line = strtok(NULL, "\n");
        }
        g_free(resp_copy);

        /* Build profile object */
        cJSON *profile_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(profile_obj, "name", profile_name);
        cJSON_AddItemToObject(profile_obj, "curve_points", curve_array);
        cJSON_AddItemToArray(fan_profiles_array, profile_obj);
    }
    cJSON_AddItemToObject(fan_section, "profiles", fan_profiles_array);
    if (fan_complete) {
        if (cJSON_GetObjectItem(root, "fan_profiles")) {
            cJSON_ReplaceItemInObject(root, "fan_profiles", fan_section);
        } else {
            cJSON_AddItemToObject(root, "fan_profiles", fan_section);
        }
    } else {
        LOG_WARN("Fan daemon profiles incomplete (connected=%d, profiles=%d) - keeping saved fan section",
                 app->connected, app->num_profiles);
        cJSON_Delete(fan_section);
    }

    /* Build cpu_freq_profiles section (nested object) */
    if (app->cpu_connected) {
        const char *cpu_response = send_cpu_command(app, "LIST_CUSTOM_PROFILES_JSON");
        if (cpu_response && strncmp(cpu_response, "ERROR", 5) != 0) {
            /* The daemon returns raw JSON array for cpu profiles */
            cJSON *cpu_profiles_array = cJSON_Parse(cpu_response);
            if (cpu_profiles_array && cJSON_IsArray(cpu_profiles_array)) {
                /* Patch the apply_all_cores field for the active profile
                 * with the current checkbox state before saving */
                if (app->cpu_active_custom_profile_idx >= 0) {
                    cJSON *active_profile = cJSON_GetArrayItem(cpu_profiles_array, app->cpu_active_custom_profile_idx);
                    if (active_profile) {
                        int checkbox_state = 0;
                        if (GTK_IS_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)) {
                            checkbox_state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)) ? 1 : 0;
                        }
                        cJSON_ReplaceItemInObject(active_profile, "apply_all_cores", cJSON_CreateNumber(checkbox_state));
                    }
                }

                /* Build the nested cpu_freq_profiles section */
                cJSON *cpu_section = cJSON_CreateObject();
                cJSON_AddNumberToObject(cpu_section, "active_profile_index", app->cpu_active_custom_profile_idx);

                /* Get the current hard limit value from the spinner */
                {
                    double ghz = 0;
                    if (GTK_IS_SPIN_BUTTON(app->cpu_hard_limit_spinner)) {
                        ghz = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->cpu_hard_limit_spinner));
                    }
                    guint64 hz = (guint64)(ghz * 1000000000.0 + 0.5);
                    if (hz > 0) {
                        cJSON_AddNumberToObject(cpu_section, "cpu_hard_limit_hz", (double)hz);
                    } else {
                        /* Spinner unavailable: carry over the saved limit so the
                         * hard-limit enforcer never loses its value */
                        cJSON *old_cpu = cJSON_GetObjectItem(root, "cpu_freq_profiles");
                        cJSON *old_limit = old_cpu ? cJSON_GetObjectItem(old_cpu, "cpu_hard_limit_hz") : NULL;
                        if (cJSON_IsNumber(old_limit) && old_limit->valuedouble > 0) {
                            cJSON_AddNumberToObject(cpu_section, "cpu_hard_limit_hz", old_limit->valuedouble);
                        }
                    }
                }

                cJSON_AddItemToObject(cpu_section, "profiles", cpu_profiles_array);
                if (cJSON_GetObjectItem(root, "cpu_freq_profiles")) {
                    cJSON_ReplaceItemInObject(root, "cpu_freq_profiles", cpu_section);
                } else {
                    cJSON_AddItemToObject(root, "cpu_freq_profiles", cpu_section);
                }
            } else {
                LOG_WARN("Could not parse CPU profiles from daemon - keeping saved CPU section");
                if (cpu_profiles_array) cJSON_Delete(cpu_profiles_array);
            }
        } else {
            LOG_WARN("CPU daemon did not return profiles - keeping saved CPU section");
        }
    } else {
        /* CPU daemon not connected: leave the saved CPU section (and hard limit) untouched */
        LOG_WARN("CPU daemon not connected - keeping saved CPU section");
    }

    /* Write atomically (temp file + rename), so a crash never leaves a
     * half-written file */
    char *json_str = cJSON_Print(root);
    if (json_str) {
        GError *write_err = NULL;
        if (g_file_set_contents(profiles_path, json_str, -1, &write_err)) {
            LOG_INFO("Saved profiles to %s (fan section %s, keyboard section %s)", profiles_path,
                     fan_complete ? "updated" : "kept",
                     cJSON_GetObjectItem(root, "keyboard_profiles") ? "preserved" : "absent");
        } else {
            LOG_ERROR("Could not write %s: %s", profiles_path, write_err->message);
            g_error_free(write_err);
        }
        cJSON_free(json_str);
    }
    cJSON_Delete(root);
}

/* Load curve points for a specific profile into the tree view */
static void load_curve_points_for_profile(AppData *app, int profile_idx) {
    if (!app->connected || app->socket_fd < 0) {
        return;
    }

    /* Check if curve_store is valid */
    if (!app->curve_store || !GTK_IS_LIST_STORE(app->curve_store)) {
        g_print("[GUI] WARNING: curve_store is NULL or invalid, cannot load curve points\n");
        return;
    }

    g_print("[GUI] Loading curve points for profile index %d (active_profile_idx=%d)\n", profile_idx, app->active_profile_idx);

    /* Clear existing curve points in tree view */
    gtk_list_store_clear(GTK_LIST_STORE(app->curve_store));

    char buffer[BUFFER_SIZE];
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "GET_PROFILE %d", profile_idx);
    const char *response = send_command(app, cmd);

    /* Check for error responses */
    if (strncmp(response, "ERROR", 5) == 0) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Failed to load profile %d: %s\n", ts, profile_idx, response);
        return;
    }

    strncpy(buffer, response, BUFFER_SIZE - 1);
    buffer[BUFFER_SIZE - 1] = '\0';

    /* First pass: collect all curve points into an array */
    CurvePoint points[MAX_CURVE_POINTS];
    int point_count = 0;

    char *line = strtok(buffer, "\n");
    while (line != NULL && point_count < MAX_CURVE_POINTS) {
        if (strncmp(line, "CURVE_POINT temp=", 15) == 0) {
            int temp, fan_pct;
            sscanf(line, "CURVE_POINT temp=%d fan_pct=%d", &temp, &fan_pct);
            points[point_count].temp = temp;
            points[point_count].fan_pct = fan_pct;
            point_count++;
        }
        line = strtok(NULL, "\n");
    }

    /* Sort curve points by temperature in ascending order */
    if (point_count > 1) {
        qsort(points, point_count, sizeof(CurvePoint), compare_curve_points_asc);
    }

    /* Second pass: add sorted curve points to tree view */
    for (int i = 0; i < point_count; i++) {
        GtkTreeIter iter;
        gtk_list_store_append(GTK_LIST_STORE(app->curve_store), &iter);
        char temp_str[16], pct_str[16];
        snprintf(temp_str, sizeof(temp_str), "%d°C", points[i].temp);
        snprintf(pct_str, sizeof(pct_str), "%d%%", points[i].fan_pct);
        gtk_list_store_set(GTK_LIST_STORE(app->curve_store), &iter,
                          0, temp_str,
                          1, points[i].fan_pct,
                          -1);
    }

    /* Update the graph visualization */
    update_curve_graph(app);
}

/* Fan Profiles tab: activate the profile selected in the dropdown */
static void on_fan_activate_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->profile_combo));
    if (idx < 0) {
        show_status(app, _("No profile selected"));
        return;
    }
    if (app->curve_points_modified) {
        int r = check_unsaved_changes(app);
        if (r == 1) on_apply_clicked(NULL, app);
        else if (r == 0) { load_curve_points_for_profile(app, idx); app->curve_points_modified = 0; }
        else return;
    }
    app->pending_active_profile_idx = idx;
    on_apply_profile_clicked(NULL, app);
}

/* Create Page 2: Curve Editor */
static GtkWidget *create_curve_editor_page(AppData *app) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    /* 12 px at the top like the Keyboard tab, so the daemon status lines of
     * all tabs are at the same height; 20 px elsewhere as before */
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 20);
    gtk_widget_set_margin_start(vbox, 20);
    gtk_widget_set_margin_end(vbox, 20);

    /* Daemon status (like the Keyboard tab) */
    {
        GtkWidget *frame = gtk_frame_new(_("Fan Daemon Status"));
        gtk_container_set_border_width(GTK_CONTAINER(frame), 6);
        app->fan_daemon_status_label = gtk_label_new(_("Fan daemon not connected"));
        gtk_label_set_xalign(GTK_LABEL(app->fan_daemon_status_label), 0.0);
        gtk_widget_set_margin_start(app->fan_daemon_status_label, 6);
        gtk_widget_set_margin_end(app->fan_daemon_status_label, 6);
        gtk_container_add(GTK_CONTAINER(frame), app->fan_daemon_status_label);
        gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    }

    GtkWidget *info_label = gtk_label_new(_("Choose a profile to edit its curve. Activate makes it control the fans; "
                                          "\"(Active)\" marks the profile in use."));
    gtk_label_set_line_wrap(GTK_LABEL(info_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), info_label, FALSE, FALSE, 0);

    /* Manual mode warning and button (shown only in manual mode) */
    app->manual_mode_warning_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app->manual_mode_warning_label),
                         _("<span size='x-large' weight='bold' foreground='#d9534f'>Manual Mode Active!</span>"));
    gtk_widget_set_halign(app->manual_mode_warning_label, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all(app->manual_mode_warning_label, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), app->manual_mode_warning_label, FALSE, FALSE, 0);
    app->disable_manual_mode_btn = gtk_button_new_with_label(_("Disable Manual Mode"));
    gtk_widget_set_halign(app->disable_manual_mode_btn, GTK_ALIGN_CENTER);
    g_signal_connect(app->disable_manual_mode_btn, "clicked", G_CALLBACK(on_disable_manual_mode_clicked), app);
    gtk_widget_set_no_show_all(app->disable_manual_mode_btn, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), app->disable_manual_mode_btn, FALSE, FALSE, 0);

    /* Profile selection */
    GtkWidget *profile_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *profile_label = gtk_label_new(_("Profile:"));
    app->profile_combo = gtk_combo_box_text_new();

    /* Connect profile change to load curve points and store handler ID */
    app->profile_combo_signal_id = g_signal_connect(app->profile_combo, "changed", G_CALLBACK(on_profile_combo_changed), app);

    gtk_box_pack_start(GTK_BOX(profile_box), profile_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(profile_box), app->profile_combo, TRUE, TRUE, 0);

    /* Profile buttons, same names and order in every tab (1.22.1):
     * Create, Rename, Save, Delete, Activate */
    GtkWidget *create_btn = gtk_button_new_with_label(_("Create"));
    g_signal_connect(create_btn, "clicked", G_CALLBACK(on_create_profile_clicked), app);
    gtk_box_pack_start(GTK_BOX(profile_box), create_btn, FALSE, FALSE, 0);

    GtkWidget *rename_profile_btn = gtk_button_new_with_label(_("Rename"));
    g_signal_connect(rename_profile_btn, "clicked", G_CALLBACK(on_rename_profile_clicked), app);
    gtk_box_pack_start(GTK_BOX(profile_box), rename_profile_btn, FALSE, FALSE, 0);

    /* Save: stores the curve points of the selected profile (was "Apply"
     * under the curve) */
    GtkWidget *save_btn = gtk_button_new_with_label(_("Save"));
    gtk_widget_set_tooltip_text(save_btn, _("Save the curve points of the selected profile"));
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_apply_clicked), app);
    gtk_box_pack_start(GTK_BOX(profile_box), save_btn, FALSE, FALSE, 0);

    GtkWidget *delete_profile_btn = gtk_button_new_with_label(_("Delete"));
    g_signal_connect(delete_profile_btn, "clicked", G_CALLBACK(on_delete_profile_clicked), app);
    gtk_box_pack_start(GTK_BOX(profile_box), delete_profile_btn, FALSE, FALSE, 0);

    /* Activate the selected profile (was the radio list in the old Fan Profiles tab) */
    app->fan_activate_btn = gtk_button_new_with_label(_("Activate"));
    g_signal_connect(app->fan_activate_btn, "clicked", G_CALLBACK(on_fan_activate_clicked), app);
    gtk_box_pack_start(GTK_BOX(profile_box), app->fan_activate_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), profile_box, FALSE, FALSE, 0);

    /* Create horizontal box for tree view and graph side by side */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    /* Left side: Curve points tree view */
    GtkWidget *tree_frame = gtk_frame_new(_("Temperature Curve Points"));
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    /* as wide as its columns (translated headers are longer, 1.31.0): no
     * horizontal scrolling, the natural width is passed on */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scrolled), TRUE);

    /* Create list store with columns: Temperature (text), Fan % (int) */
    app->curve_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);

    app->curve_treeview = GTK_WIDGET(gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->curve_store)));

    /* Temperature column */
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *temp_col = gtk_tree_view_column_new_with_attributes(
        _("Temperature"), renderer, "text", 0, NULL);
    /* sized to header and content: translated headers are longer (1.31.0) */
    gtk_tree_view_column_set_sizing(temp_col, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->curve_treeview), temp_col);

    /* Fan % column */
    renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *fan_col = gtk_tree_view_column_new_with_attributes(
        _("Fan Speed (%)"), renderer, "text", 1, NULL);
    gtk_tree_view_column_set_sizing(fan_col, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->curve_treeview), fan_col);

    /* Connect double-click to edit curve point */
    g_signal_connect(app->curve_treeview, "row-activated",
                    G_CALLBACK(on_curve_point_activated), app);

    gtk_container_add(GTK_CONTAINER(scrolled), app->curve_treeview);
    gtk_container_add(GTK_CONTAINER(tree_frame), scrolled);
    gtk_box_pack_start(GTK_BOX(hbox), tree_frame, FALSE, FALSE, 0);

    /* Right side: Temperature curve graph */
    GtkWidget *graph_frame = gtk_frame_new(_("Temperature Curve Graph"));
    app->curve_graph = gtk_drawing_area_new();
    gtk_widget_set_size_request(app->curve_graph, 700, 430);   /* fills the rest of the row */

    /* Connect draw signal */
    g_signal_connect(app->curve_graph, "draw", G_CALLBACK(draw_curve_graph), app);

    gtk_container_add(GTK_CONTAINER(graph_frame), app->curve_graph);
    gtk_box_pack_start(GTK_BOX(hbox), graph_frame, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    /* Curve point controls */
    GtkWidget *point_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *add_btn = gtk_button_new_with_label(_("Add Point"));
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_curve_point_clicked), app);
    gtk_box_pack_start(GTK_BOX(point_controls), add_btn, FALSE, FALSE, 0);

    GtkWidget *delete_btn = gtk_button_new_with_label(_("Delete Point"));
    g_signal_connect(delete_btn, "clicked", G_CALLBACK(on_delete_curve_point_clicked), app);
    gtk_box_pack_start(GTK_BOX(point_controls), delete_btn, FALSE, FALSE, 0);

    GtkWidget *control_label = gtk_label_new(_("  (Double-click to edit, points are auto-sorted by temperature)"));
    gtk_box_pack_start(GTK_BOX(point_controls), control_label, FALSE, FALSE, 10);

    gtk_box_pack_start(GTK_BOX(vbox), point_controls, FALSE, FALSE, 5);

    /* No Cancel button (1.24.3): unsaved curve changes are asked about
     * (Save / Discard) when switching profiles, leaving the tab or closing */

    return vbox;
}

/* Update the curve graph by triggering a redraw */
static void update_curve_graph(AppData *app) {
    if (app->curve_graph) {
        gtk_widget_queue_draw(app->curve_graph);
    }
}

/* Draw callback for the temperature curve graph using Cairo */
/* Text for the fan curve graph, drawn with Pango (fonts with fallback, so
 * every language shows): (x, y) is the anchor, align 0 left / 1 center /
 * 2 right of it, vertically centered; vertical = rotated to read upwards */
static void graph_text(cairo_t *cr, const char *text, double x, double y, double size, int align, int vertical) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans");
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, fd);
    pango_layout_set_text(layout, text, -1);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    cairo_save(cr);
    cairo_translate(cr, x, y);
    if (vertical) cairo_rotate(cr, -M_PI_2);
    cairo_move_to(cr, align == 1 ? -w / 2.0 : align == 2 ? -w : 0, -h / 2.0);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    pango_font_description_free(fd);
    g_object_unref(layout);
}

static gboolean draw_curve_graph(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    AppData *app = (AppData *)user_data;

    /* Get widget dimensions */
    gint width = gtk_widget_get_allocated_width(widget);
    gint height = gtk_widget_get_allocated_height(widget);

    /* Margins for the graph area */
    const double left_margin = 60;   /* Space for Y-axis labels */
    const double right_margin = 20;
    const double top_margin = 20;
    const double bottom_margin = 50;  /* Space for X-axis labels */

    /* Graph area dimensions */
    double graph_width = width - left_margin - right_margin;
    double graph_height = height - top_margin - bottom_margin;

    if (graph_width <= 0 || graph_height <= 0) {
        return FALSE;
    }

    /* Clear background */
    /* colors follow the theme (1.29.0) */
    const double bg = app->is_dark ? 0.14 : 1.0, grid = app->is_dark ? 0.28 : 0.9, fg = app->is_dark ? 0.88 : 0.0;
    cairo_set_source_rgb(cr, bg, bg, bg);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    /* Draw grid lines and axes */
    cairo_set_source_rgb(cr, grid, grid, grid);
    cairo_set_line_width(cr, 1.0);

    /* Horizontal grid lines (fan speed) - every 10% */
    for (int i = 0; i <= 10; i++) {
        double y = top_margin + (graph_height * (1.0 - (i * 0.1)));
        cairo_move_to(cr, left_margin, y);
        cairo_line_to(cr, width - right_margin, y);
    }
    cairo_stroke(cr);

    /* Vertical grid lines (temperature) - every 10°C */
    for (int i = 0; i <= 10; i++) {
        double x = left_margin + (graph_width * (i * 0.1));
        cairo_move_to(cr, x, top_margin);
        cairo_line_to(cr, x, height - bottom_margin);
    }
    cairo_stroke(cr);

    /* Draw axes */
    cairo_set_source_rgb(cr, fg, fg, fg);   /* axes */
    cairo_set_line_width(cr, 2.0);

    /* X-axis (temperature) */
    cairo_move_to(cr, left_margin, height - bottom_margin);
    cairo_line_to(cr, width - right_margin, height - bottom_margin);

    /* Y-axis (fan speed) */
    cairo_move_to(cr, left_margin, top_margin);
    cairo_line_to(cr, left_margin, height - bottom_margin);
    cairo_stroke(cr);

    /* Axis labels and titles with Pango (1.31.0): cairo's toy text API has
     * no font fallback, so Japanese and Korean showed as boxes */
    cairo_set_source_rgb(cr, fg, fg, fg);   /* labels */

    /* Y-axis labels (fan speed %) - every 20%, right-aligned next to the axis */
    for (int i = 0; i <= 5; i++) {
        double y = top_margin + (graph_height * (1.0 - (i * 0.2)));
        char label[16];
        snprintf(label, sizeof(label), "%d%%", i * 20);
        graph_text(cr, label, left_margin - 6, y, 10, 2, 0);
    }

    /* X-axis labels (temperature °C) - every 10°C, centered under the ticks */
    for (int i = 0; i <= 10; i++) {
        double x = left_margin + (graph_width * (i * 0.1));
        char label[16];
        snprintf(label, sizeof(label), "%d", i * 10);
        graph_text(cr, label, x, height - bottom_margin + 12, 10, 1, 0);
    }

    /* Axis titles: centered on their axis; the vertical one at the left
     * edge, clear of the percentage labels */
    graph_text(cr, _("Temperature (°C)"), left_margin + graph_width / 2, height - bottom_margin + 34, 12, 1, 0);
    graph_text(cr, _("Fan Speed (%)"), 12, top_margin + graph_height / 2, 12, 1, 1);

    /* Get curve points from the list store */
    GtkTreeIter iter;
    gboolean has_iter = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->curve_store), &iter);

    if (!has_iter) {
        return TRUE;  /* No data to draw */
    }

    /* Store points in arrays for drawing */
    int temps[100], speeds[100];
    int point_count = 0;

    while (has_iter && point_count < 100) {
        gchar *temp_str;
        int speed_val;
        gtk_tree_model_get(GTK_TREE_MODEL(app->curve_store), &iter,
                          0, &temp_str,
                          1, &speed_val,
                          -1);
        temps[point_count] = atoi(temp_str);
        speeds[point_count] = speed_val;
        g_free(temp_str);
        point_count++;
        has_iter = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->curve_store), &iter);
    }

    if (point_count < 2) {
        return TRUE;  /* Not enough points to draw a line */
    }

    /* Draw the curve line connecting points */
    cairo_set_source_rgb(cr, 0.0, 0.5, 1.0);  /* Blue for the curve */
    cairo_set_line_width(cr, 2.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    cairo_move_to(cr, left_margin + (graph_width * temps[0] / 100.0),
                  top_margin + (graph_height * (1.0 - speeds[0] / 100.0)));

    for (int i = 1; i < point_count; i++) {
        cairo_line_to(cr, left_margin + (graph_width * temps[i] / 100.0),
                      top_margin + (graph_height * (1.0 - speeds[i] / 100.0)));
    }
    cairo_stroke(cr);

    /* Draw points as circles */
    cairo_set_source_rgb(cr, 1.0, 0.3, 0.0);  /* Red-orange for points */
    cairo_set_line_width(cr, 1.0);

    for (int i = 0; i < point_count; i++) {
        double x = left_margin + (graph_width * temps[i] / 100.0);
        double y = top_margin + (graph_height * (1.0 - speeds[i] / 100.0));

        /* orange for every point (1.32.1: only the first was orange, the
         * outline color of the previous point filled the others) */
        cairo_set_source_rgb(cr, 1.0, 0.3, 0.0);
        cairo_arc(cr, x, y, 5.0, 0, 2 * M_PI);
        cairo_fill(cr);

        /* outline in the background color for better visibility */
        cairo_set_source_rgb(cr, bg, bg, bg);
        cairo_arc(cr, x, y, 5.0, 0, 2 * M_PI);
        cairo_stroke(cr);
    }

    return TRUE;
}


/* Callback: Apply profile selection */
static void on_apply_profile_clicked(GtkWidget *widget, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int profile_idx = -1;

    /* Check if called from tray menu (profile index stored in widget data) */
    if (GTK_IS_WIDGET(widget)) {
        gpointer idx_ptr = g_object_get_data(G_OBJECT(widget), "profile_idx");
        if (idx_ptr) {
            profile_idx = *(int*)idx_ptr;
        }
    }

    /* Fall back to the pending selection (Fan Profiles tab: Activate) */
    if (profile_idx < 0) {
        profile_idx = app->pending_active_profile_idx;
    }

    if (profile_idx >= 0 && profile_idx < app->num_profiles) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "SET_ACTIVE_PROFILE %d", profile_idx);
        const char *response = send_command(app, cmd);
        show_status(app, response);

        /* Update the active profile index in app */
        app->active_profile_idx = profile_idx;
        app->pending_active_profile_idx = profile_idx;

        /* Refresh the dropdown so "(Active)" moves */
        refresh_profile_list(app);

        /* Update tray menu to reflect new active profile */
        update_tray_menu(app);

        /* Persist the active_profile_index to profiles.json */
        gui_save_profiles_to_disk(app);
    } else {
        show_status(app, _("No profile selected"));
    }
}


/* Update the profile tab UI based on manual mode state */
static void update_profile_tab_for_manual_mode(AppData *app, int manual_mode_enabled) {
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    g_print("%s [GUI] Updating profile tab for manual mode: %s\n", ts, manual_mode_enabled ? "ENABLED" : "DISABLED");
    g_print("%s [GUI] -> app->manual_mode_warning_label = %p\n", ts, (void*)app->manual_mode_warning_label);
    g_print("%s [GUI] -> app->disable_manual_mode_btn = %p\n", ts, (void*)app->disable_manual_mode_btn);

    /* Show/hide warning label and button */
    if (app->manual_mode_warning_label && GTK_IS_WIDGET(app->manual_mode_warning_label)) {
        if (manual_mode_enabled) {
            gtk_widget_show(app->manual_mode_warning_label);
        } else {
            gtk_widget_hide(app->manual_mode_warning_label);
        }
    }

    if (app->disable_manual_mode_btn && GTK_IS_WIDGET(app->disable_manual_mode_btn)) {
        if (manual_mode_enabled) {
            gtk_widget_show(app->disable_manual_mode_btn);
        } else {
            gtk_widget_hide(app->disable_manual_mode_btn);
        }
    }

    /* Activating a profile is not possible in manual mode */
    if (app->fan_activate_btn && GTK_IS_WIDGET(app->fan_activate_btn))
        gtk_widget_set_sensitive(app->fan_activate_btn, !manual_mode_enabled);
}

/* Callback: Disable manual mode button clicked */
static void on_disable_manual_mode_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;

    char ts[32];
    get_timestamp(ts, sizeof(ts));
    g_print("%s [GUI] Disable manual mode button clicked\n", ts);

    /* Re-enable auto mode on daemon */
    const char *response = send_command(app, "SET_AUTO 1");
    if (strncmp(response, "OK", 2) == 0) {
        app->auto_mode_enabled = 1;
        app->in_manual_mode = 0;

        /* Update manual mode UI on Status tab */
        gtk_widget_set_sensitive(app->manual_minus_btn, FALSE);
        gtk_widget_set_sensitive(app->manual_plus_btn, FALSE);
        gtk_label_set_text(GTK_LABEL(app->manual_speed_label), "--");

        /* Reset the switch visually */
        g_signal_handler_block(app->manual_mode_switch, app->manual_mode_signal_id);
        gtk_switch_set_state(GTK_SWITCH(app->manual_mode_switch), FALSE);
        g_signal_handler_unblock(app->manual_mode_switch, app->manual_mode_signal_id);

        /* Reactivate the profile that was active before manual mode */
        if (app->profile_before_manual >= 0) {
            gui_event("%s [GUI] Re-activating profile %d\n", ts, app->profile_before_manual);
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "SET_ACTIVE_PROFILE %d", app->profile_before_manual);
            response = send_command(app, cmd);
            if (strncmp(response, "OK", 2) == 0) {
                /* Update profile list to reflect the reactivated profile */
                refresh_profile_list(app);
                show_status(app, _("Manual mode disabled - returned to auto mode"));
            } else {
                show_status(app, _("Failed to reactivate previous profile"));
            }
        }

        /* Update the profile tab UI */
        update_profile_tab_for_manual_mode(app, 0);
    } else {
        show_status(app, _("Failed to disable manual mode"));
    }
}
