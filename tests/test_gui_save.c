// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_gui_save.c - GUI profile saving (keyboard section preserved, sections
 * kept when a daemon is unreachable, corrupt file untouched) and GUI log
 * routing. Run through tests/test_gui_save.py, which starts a fake fan daemon
 * and a test build of the CPU daemon in the working directory.
 */
#define main gui_main
#include "dcc-gui.c"
#undef main

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("PASS  %s\n", m); else { printf("FAIL  %s\n", m); fails++; } } while (0)

static int connect_sock(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
    return fd;
}

static char path[1024];
static void write_file(const char *content) { g_file_set_contents(path, content, -1, NULL); }
static cJSON *read_json(void) {
    gchar *c = NULL; if (!g_file_get_contents(path, &c, NULL, NULL)) return NULL;
    cJSON *j = cJSON_Parse(c); g_free(c); return j;
}

static const char *SEED =
    "{\"version\":\"1.0\","
    "\"fan_profiles\":{\"active_profile_index\":0,\"profiles\":[{\"name\":\"OldFan\",\"curve_points\":[]}]},"
    "\"cpu_freq_profiles\":{\"active_profile_index\":0,\"cpu_hard_limit_hz\":2600000000,\"profiles\":[{\"name\":\"OldCpu\"}]},"
    "\"keyboard_profiles\":{\"active_profile_index\":0,\"profiles\":[{\"name\":\"MyKeyboard\",\"zones\":[]}]},"
    "\"custom_key\":42}";

int main(int argc, char **argv) {
    (void)argc;
    const char *home = argv[1];
    char cfg[1024];
    snprintf(cfg, sizeof cfg, "%s/.config/dudes-control-center", home);
    g_mkdir_with_parents(cfg, 0755);
    snprintf(path, sizeof path, "%s/profiles.json", cfg);
    setenv("HOME", home, 1);

    char logdir[1024]; snprintf(logdir, sizeof logdir, "%s/logs", home);
    log_init("GUI", LOG_LEVEL_INFO, logdir);
    gui_install_log_handlers();

    static AppData app;  /* zeroed */
    app.socket_fd = connect_sock(SOCKET_PATH);
    app.connected = app.socket_fd >= 0;
    app.cpu_socket_fd = connect_sock(CPU_SOCKET_PATH);
    app.cpu_connected = app.cpu_socket_fd >= 0;
    app.num_profiles = 3; app.active_profile_idx = 1; app.cpu_active_custom_profile_idx = 1;
    CHECK(app.connected && app.cpu_connected, "test fan and CPU daemons reachable");

    /* A: normal save keeps keyboard + unknown keys, updates fan/cpu */
    write_file(SEED);
    gui_save_profiles_to_disk(&app);
    cJSON *j = read_json();
    cJSON *kbd = cJSON_GetObjectItem(j, "keyboard_profiles");
    CHECK(kbd && strstr(cJSON_PrintUnformatted(kbd), "MyKeyboard"), "A: keyboard_profiles preserved");
    CHECK(cJSON_GetObjectItem(j, "custom_key") != NULL, "A: unknown top-level keys preserved");
    char *fan = cJSON_PrintUnformatted(cJSON_GetObjectItem(j, "fan_profiles"));
    CHECK(!strstr(fan, "OldFan") && cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "fan_profiles"), "profiles")) == 3,
          "A: fan section replaced with the daemon's 3 profiles");
    cJSON *cpu = cJSON_GetObjectItem(j, "cpu_freq_profiles");
    CHECK(!strstr(cJSON_PrintUnformatted(cpu), "OldCpu"), "A: cpu section replaced with the daemon's profiles");
    CHECK(cJSON_GetObjectItem(cpu, "cpu_hard_limit_hz") &&
          cJSON_GetObjectItem(cpu, "cpu_hard_limit_hz")->valuedouble == 2600000000.0,
          "A: hard limit carried over when the spinner is unavailable");
    cJSON_Delete(j);

    /* B: CPU daemon disconnected -> CPU section untouched */
    write_file(SEED);
    int keep_cpu = app.cpu_connected; app.cpu_connected = 0;
    gui_save_profiles_to_disk(&app);
    j = read_json();
    CHECK(strstr(cJSON_PrintUnformatted(cJSON_GetObjectItem(j, "cpu_freq_profiles")), "OldCpu") != NULL,
          "B: CPU disconnected -> saved CPU section kept");
    cJSON_Delete(j);
    app.cpu_connected = keep_cpu;

    /* C: fan daemon disconnected -> fan section untouched */
    write_file(SEED);
    int keep_fan = app.connected; app.connected = 0;
    gui_save_profiles_to_disk(&app);
    j = read_json();
    CHECK(strstr(cJSON_PrintUnformatted(cJSON_GetObjectItem(j, "fan_profiles")), "OldFan") != NULL,
          "C: fan disconnected -> saved fan section kept");
    cJSON_Delete(j);
    app.connected = keep_fan;

    /* D: corrupt file is never overwritten */
    write_file("{ this is not json");
    gui_save_profiles_to_disk(&app);
    gchar *c = NULL; g_file_get_contents(path, &c, NULL, NULL);
    CHECK(c && strcmp(c, "{ this is not json") == 0, "D: unparsable file left untouched");
    g_free(c);

    /* E: no file yet -> created */
    unlink(path);
    gui_save_profiles_to_disk(&app);
    j = read_json();
    CHECK(j && cJSON_GetObjectItem(j, "fan_profiles") && cJSON_GetObjectItem(j, "version"), "E: missing file is created");
    cJSON_Delete(j);

    /* F: log routing */
    g_print("[12:00:00.000] [GUI] Hello from g_print\n");
    for (int i = 0; i < 5; i++) g_print("[GUI] periodic status line\n");
    g_printerr("[GUI] Something on stderr\n");
    g_warning("a GTK-style warning");
    log_cleanup();
    char lp[1100]; snprintf(lp, sizeof lp, "%s/GUI.log", logdir);
    gchar *lg = NULL; g_file_get_contents(lp, &lg, NULL, NULL);
    int periodic = 0; for (char *q = lg; (q = strstr(q, "periodic status line")); q++) periodic++;
    CHECK(lg && strstr(lg, "[INFO ] [GUI] Hello from g_print"), "F: g_print goes to GUI.log without the duplicate timestamp");
    CHECK(periodic == 1, "F: repeated message logged once per minute");
    CHECK(lg && strstr(lg, "[WARN ] [GUI] Something on stderr"), "F: g_printerr logged as WARN");
    CHECK(lg && strstr(lg, "a GTK-style warning"), "F: GLib/GTK warnings logged");
    CHECK(lg && strstr(lg, "Saved profiles to") && strstr(lg, "keyboard section preserved"), "F: save result logged");
    CHECK(lg && strstr(lg, "Profile save aborted"), "F: aborted save logged as error");
    g_free(lg);

    /* G: power-mode list matching (v1.17.1) */
    GtkListStore *st = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GtkTreeIter it;
    gtk_list_store_append(st, &it); gtk_list_store_set(st, &it, 0, "Performance", 1, "performance", -1);
    gtk_list_store_append(st, &it); gtk_list_store_set(st, &it, 0, "Balanced", 1, "powersave:balance_performance", -1);
    gtk_list_store_append(st, &it); gtk_list_store_set(st, &it, 0, "Power Save", 1, "powersave:power", -1);
    CHECK(gui_find_power_mode_row(st, "performance", "") == 0, "G: performance -> Performance");
    CHECK(gui_find_power_mode_row(st, "powersave", "balance_performance") == 1, "G: powersave+balance_performance -> Balanced (exact)");
    CHECK(gui_find_power_mode_row(st, "powersave", "power") == 2, "G: powersave+power -> Power Save (exact)");
    CHECK(gui_find_power_mode_row(st, "schedutil", "") == 1, "G: old 'schedutil' profile shows as Balanced");
    CHECK(gui_find_power_mode_row(st, "powersave", "") == 2, "G: old 'powersave' profile shows as Power Save");
    CHECK(gui_find_power_mode_row(st, "", "") == -1, "G: no governor -> no selection");
    GtkListStore *st2 = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    gtk_list_store_append(st2, &it); gtk_list_store_set(st2, &it, 0, "Balanced", 1, "schedutil", -1);
    CHECK(gui_find_power_mode_row(st2, "schedutil", "") == 0, "G: schedutil system: exact schedutil row");
    g_object_unref(st);
    g_object_unref(st2);

    /* M: manual fan speed steps (v1.18.3: down to 0%) */
    CHECK(manual_speed_step(10, -10) == 0, "M: 10% minus -> 0% (fans off allowed)");
    CHECK(manual_speed_step(0, -10) == 0, "M: 0% minus stays at 0%");
    CHECK(manual_speed_step(0, +10) == 10, "M: 0% plus -> 10%");
    CHECK(manual_speed_step(95, +10) == 100 && manual_speed_step(100, +10) == 100, "M: plus caps at 100%");
    CHECK(manual_speed_step(37, -10) == 30, "M: odd value rounds down to the 10% step");

    /* T: CPU daemon replies end with a NUL marker, so a command no longer
     * costs a 200 ms wait (v1.20.1) */
    {
        gint64 t0 = g_get_monotonic_time();
        const char *r = NULL;
        for (int i = 0; i < 10; i++) r = send_cpu_command(&app, "GET_HARD_LIMIT");
        double ms = (g_get_monotonic_time() - t0) / 1000.0 / 10.0;
        printf("      (CPU command round trip: %.1f ms)\n", ms);
        CHECK(r && strncmp(r, "OK HARD_LIMIT=", 14) == 0 && ms < 50.0,
              "T: CPU daemon command answered without the old 200 ms wait");
        r = send_cpu_command(&app, "LIST_CUSTOM_PROFILES_JSON");
        CHECK(r && strchr(r, '{') && r[strlen(r) - 1] != '\0', "T: longer replies still read completely");
    }

    /* C: new CPU profiles start in this system's Balanced mode (v1.22.2) */
    CHECK(!strcmp(cpu_balanced_mode("GOVERNORS OK\nCOUNT=2\nperformance\npowersave\n"),
                  "powersave:balance_performance"),
          "C: intel_pstate (no schedutil) -> powersave + balance_performance");
    CHECK(!strcmp(cpu_balanced_mode("GOVERNORS OK\nCOUNT=3\nperformance\nschedutil\npowersave\n"), "schedutil"),
          "C: schedutil offered -> schedutil");
    CHECK(!strcmp(cpu_balanced_mode("GOVERNORS OK\nCOUNT=1\nschedutil_x\n"), "powersave:balance_performance"),
          "C: only exact governor names count");
    CHECK(!strcmp(cpu_balanced_mode(NULL), "schedutil") && !strcmp(cpu_balanced_mode("ERROR x"), "schedutil"),
          "C: no answer -> left to the daemon");

    /* U: unsaved CPU profile changes are found by comparing the widgets with
     * the snapshot taken when the profile was loaded (v1.23.0). Needs GTK
     * widgets, so only with a display. */
    if (gtk_init_check(NULL, NULL)) {
        static AppData u;   /* zeroed */
        GtkListStore *profiles = gtk_list_store_new(1, G_TYPE_STRING);
        GtkListStore *modes = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
        GtkTreeIter it;
        gtk_list_store_insert_with_values(profiles, &it, -1, 0, "A", -1);
        gtk_list_store_insert_with_values(profiles, &it, -1, 0, "B", -1);
        gtk_list_store_insert_with_values(modes, &it, -1, 0, "Balanced", 1, "powersave:balance_performance", -1);
        gtk_list_store_insert_with_values(modes, &it, -1, 0, "Performance", 1, "performance", -1);
        u.cpu_custom_profile_combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(profiles));
        u.cpu_profile_combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(modes));
        u.cpu_profile_store = modes;
        u.cpu_apply_all_cores_checkbox = gtk_check_button_new();
        gtk_combo_box_set_active(GTK_COMBO_BOX(u.cpu_custom_profile_combo), 1);
        gtk_combo_box_set_active(GTK_COMBO_BOX(u.cpu_profile_combo), 0);
        u.cpu_core_count = 2;
        for (int i = 0; i < 2; i++) {
            u.cpu_core_min_adjustments[i] = GTK_ADJUSTMENT(gtk_adjustment_new(800, 800, 3600, 100, 100, 0));
            u.cpu_core_max_adjustments[i] = GTK_ADJUSTMENT(gtk_adjustment_new(3600, 800, 3600, 100, 100, 0));
        }
        CHECK(!cpu_profile_has_unsaved_changes(&u), "U: no snapshot yet -> nothing unsaved");
        cpu_take_snapshot(&u);
        CHECK(u.cpu_snap_valid && u.cpu_snap_profile == 1 && !cpu_profile_has_unsaved_changes(&u),
              "U: just loaded -> nothing unsaved");
        CHECK(cpu_check_unsaved_changes(&u) == 1, "U: no changes -> no dialog, go on");
        gtk_adjustment_set_value(u.cpu_core_max_adjustments[1], 2400);
        CHECK(cpu_profile_has_unsaved_changes(&u), "U: spinner changed -> unsaved");
        gtk_adjustment_set_value(u.cpu_core_max_adjustments[1], 3600);
        CHECK(!cpu_profile_has_unsaved_changes(&u), "U: spinner changed back -> nothing unsaved");
        gtk_combo_box_set_active(GTK_COMBO_BOX(u.cpu_profile_combo), 1);
        CHECK(cpu_profile_has_unsaved_changes(&u), "U: power mode changed -> unsaved");
        gtk_combo_box_set_active(GTK_COMBO_BOX(u.cpu_profile_combo), 0);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(u.cpu_apply_all_cores_checkbox), TRUE);
        CHECK(cpu_profile_has_unsaved_changes(&u), "U: 'Apply values to all cores' changed -> unsaved");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(u.cpu_apply_all_cores_checkbox), FALSE);
        cpu_take_snapshot(&u);
        u.cpu_core_count = 0;
        cpu_take_snapshot(&u);
        CHECK(!u.cpu_snap_valid && !cpu_profile_has_unsaved_changes(&u), "U: no core grid -> nothing to compare");
    } else {
        printf("SKIP  U: unsaved CPU profile changes (no display)\n");
    }

    /* K: keyboard key -> LED lookup (v1.17.3) */
    CHECK(sizeof kbd_default_led_position / sizeof kbd_default_led_position[0] == 120 &&
          sizeof kbd_key_names / sizeof kbd_key_names[0] == KBD_NUM_KEYS &&
          sizeof kbd_key_positions / sizeof kbd_key_positions[0] == KBD_NUM_KEYS,
          "K: key name, position and default LED tables have the same size");
    int ok_names = 1;
    for (int i = 0; i < KBD_NUM_KEYS; i++)
        if (!kbd_key_names[i] || kbd_key_positions[i].w <= 0) ok_names = 0;
    CHECK(ok_names, "K: every key has a name and a non-empty position");
    static AppData kb;  /* zeroed */
    int leds[120], n;
    /* built-in mapping (1.22.0): LED -> key, several LEDs per key, unused LEDs */
    kbd_mapping_set_defaults(&kb);
    {
        int ok = 1, used[KBD_NUM_KEYS] = {0};
        for (int l = 0; l < 120; l++) {
            int p = kb.kbd_wizard_mapping[l];
            if (p < -1 || p >= KBD_NUM_KEYS) ok = 0;
            if (p >= 0) used[p]++;
        }
        CHECK(ok, "K: built-in mapping only names existing keys");
        int pos_nde = -1, pos_ins = -1, pos_sz = -1, pos_lt = -1, pos_space = -1;
        for (int i = 0; i < KBD_NUM_KEYS; i++) {
            if (!strcmp(kbd_key_names[i], "Numpad ,")) pos_nde = i;
            if (!strcmp(kbd_key_names[i], "Numpad 0")) pos_ins = i;
            if (!strcmp(kbd_key_names[i], "ß") && pos_sz < 0) pos_sz = i;
            if (!strcmp(kbd_key_names[i], "<")) pos_lt = i;
            if (!strcmp(kbd_key_names[i], "Space")) pos_space = i;
        }
        CHECK(kb.kbd_wizard_mapping[32] == -1 && kb.kbd_wizard_mapping[81] == -1,
              "K: built-in mapping: LED 32 and 81 light no key");
        CHECK(kb.kbd_wizard_mapping[82] == pos_lt && kb.kbd_wizard_mapping[31] == pos_sz,
              "K: built-in mapping: <> on LED 82 only, ß on LED 31");
        n = kbd_leds_for_position(&kb, pos_lt, leds, 120);
        CHECK(n == 1 && leds[0] == 82, "K: built-in mapping: <> has one LED");
        n = kbd_leds_for_position(&kb, pos_nde, leds, 120);
        int n2 = kbd_leds_for_position(&kb, pos_ins, leds + 4, 4);
        CHECK(n == 1 && leds[0] != 32 && n2 == 1 && leds[4] != 81,
              "K: built-in mapping: Numpad , and Numpad 0 have their own LEDs");
        n = kbd_leds_for_position(&kb, pos_space, leds, 120);
        CHECK(n >= 2, "K: built-in mapping: Space has several LEDs");
    }
    memset(kb.kbd_wizard_mapping, -1, sizeof kb.kbd_wizard_mapping);
    n = kbd_leds_for_position(&kb, 0, leds, 120);
    CHECK(n == 0, "K: empty wizard mapping -> no LEDs (no fallback to another table)");
    kb.kbd_wizard_mapping[4] = 103;   /* Space: several LEDs */
    kb.kbd_wizard_mapping[17] = 103;
    kb.kbd_wizard_mapping[59] = 103;
    kb.kbd_wizard_mapping[31] = 34;
    n = kbd_leds_for_position(&kb, 103, leds, 120);
    CHECK(n == 3 && leds[0] == 4 && leds[1] == 17 && leds[2] == 59, "K: wizard mapping -> all LEDs of a wide key");
    n = kbd_leds_for_position(&kb, 34, leds, 120);
    CHECK(n == 1 && leds[0] == 31, "K: wizard mapping overrides the default table");
    n = kbd_leds_for_position(&kb, 0, leds, 120);
    CHECK(n == 0, "K: key without a wizard LED gets none (not a stale default)");

    /* K: ISO Enter is L-shaped; its lower part does not cover '#' (v1.21.7) */
    {
        size_t enter = 0, hash = 0, npplus = 0;
        for (size_t i = 0; i < KBD_NUM_KEYS; i++) {
            if (!strcmp(kbd_key_names[i], "Enter") && !enter) enter = i;
            if (!strcmp(kbd_key_names[i], "#")) hash = i;
            if (!strcmp(kbd_key_names[i], "Numpad +") && !npplus) npplus = i;
        }
        const KeyPos *h = &kbd_key_positions[hash], *e = &kbd_key_positions[enter];
        double px = e->x + 3, py = h->y + h->h / 2.0;   /* left edge of Enter, in the '#' row */
        CHECK(enter && hash && !kbd_key_contains(enter, px, py) && kbd_key_contains(hash, px, py),
              "K: click on the right part of '#' selects '#', not Enter");
        CHECK(kbd_key_contains(enter, e->x + 3, e->y + 10) && kbd_key_contains(enter, e->x + e->w - 3, py),
              "K: Enter still covers its upper row and the right part of its lower row");
        double lx, sy;
        kbd_key_shape(npplus, &lx, &sy);
        CHECK(npplus && lx == kbd_key_positions[npplus].x, "K: other tall keys (Numpad +) stay rectangles");
    }

    /* K: entries of one key form a group; Numpad Enter stays a rectangle (v1.21.8) */
    {
        size_t ne[3], n = 0, en[2], m = 0;
        for (size_t i = 0; i < KBD_NUM_KEYS; i++) {
            if (!strcmp(kbd_key_names[i], "Numpad Enter") && n < 3) ne[n++] = i;
            if (!strcmp(kbd_key_names[i], "Enter") && m < 2) en[m++] = i;
        }
        CHECK(n == 3 && kbd_key_group_primary(ne[1]) == ne[0] && kbd_key_group_primary(ne[2]) == ne[0] &&
              m == 2 && kbd_key_group_primary(en[1]) == en[0] && kbd_key_group_primary(en[0]) == en[0],
              "K: the 3 Numpad Enter entries (and the 2 Enter entries) are one key");
        double lx, sy;
        kbd_key_shape(ne[0], &lx, &sy);
        CHECK(lx == kbd_key_positions[ne[0]].x, "K: Numpad Enter is not L-shaped (its own lower entry is no neighbour)");
        static AppData g;  /* zeroed */
        memset(g.kbd_wizard_mapping, -1, sizeof g.kbd_wizard_mapping);
        g.kbd_wizard_mapping[10] = (int)ne[0];
        g.kbd_wizard_mapping[11] = (int)ne[2];
        int leds[120];
        int k = kbd_leds_for_key(&g, ne[0], leds, 120);
        CHECK(k == 2, "K: a key's LEDs include those mapped to any of its entries");
    }

    /* K: key colors from the daemon's GET_ZONES (v1.18.0) */
    kbd_parse_zone_colors(&kb, "ZONES_START COUNT=2\n"
                               "ZONE 0 name=Base color=255,255,255 keys=3 0 1 2\n"
                               "ZONE 1 name=My zone color=255,0,0 keys=2 1 119\n"
                               "ZONES_END\n");
    CHECK(kb.kbd_led_has_color[0] && kb.kbd_led_rgb[0][1] == 255, "K: base zone color read");
    CHECK(kb.kbd_led_rgb[1][0] == 255 && kb.kbd_led_rgb[1][1] == 0 && kb.kbd_led_rgb[119][0] == 255,
          "K: later zone wins, names with spaces parsed");
    CHECK(!kb.kbd_led_has_color[3], "K: LED in no zone has no color");
    CHECK(kbd_parse_is_on("STATUS OK\nLED_COUNT=120\nIS_ON=0\n") == 0 &&
          kbd_parse_is_on("STATUS OK\nIS_ON=1\n") == 1 && kbd_parse_is_on("ERROR x") == -1,
          "K: backlight power state read from STATUS (v1.18.6)");
    {
        int lv = -1, mx = -1;
        CHECK(kbd_parse_brightness("STATUS OK\nIS_ON=1\nBRIGHTNESS=4\nBRIGHTNESS_MAX=10\n", &lv, &mx) == 0 &&
              lv == 4 && mx == 10 && kbd_parse_brightness("STATUS OK\nIS_ON=1\n", &lv, &mx) == -1,
              "K: keyboard brightness read from STATUS (v1.20.0)");
    }
    kbd_parse_zone_colors(&kb, "ERROR Not connected to keyboard daemon");
    CHECK(!kb.kbd_led_has_color[0], "K: error response clears the colors");

    /* Z: zone editor model (v1.24.0) */
    {
        static KbdProfileEdit e, f;
        CHECK(kbd_edit_parse(&e, "PROFILE_START COUNT=2 name=Gaming\n"
                                 "BASE color=0,0,50\n"
                                 "ZONE 0 name=W A S D color=255,0,0 keys=4 10 11 12 13\n"
                                 "ZONE 1 name=Arrows color=0,255,0 keys=2 13 119\n"
                                 "PROFILE_END\n") == 0 &&
              e.has_base && e.base[2] == 50 && e.num_zones == 2 && !strcmp(e.zones[0].name, "W A S D") &&
              e.zones[0].n == 4 && e.zones[1].leds[1] == 119,
              "Z: GET_PROFILE read: base color, zone names with spaces, LEDs");
        CHECK(kbd_edit_parse(&f, "ERROR Profile not found") == -1, "Z: error answer is not a profile");
        unsigned char rgb[120][3], has[120];
        kbd_edit_colors(&e, rgb, has);
        CHECK(has[0] && rgb[0][2] == 50 && rgb[10][0] == 255 && rgb[13][1] == 255 && rgb[119][1] == 255,
              "Z: colors: base everywhere, zones on top, later zone wins");
        GString *g = g_string_new(NULL);
        kbd_edit_encode(&e, g);
        CHECK(!strcmp(g->str, "0,0,50::W A S D|255,0,0|10,11,12,13;Arrows|0,255,0|13,119"),
              "Z: PREVIEW encoding");
        g_string_free(g, TRUE);
        f = e;
        CHECK(kbd_edit_equal(&e, &f), "Z: unchanged copy is equal (nothing unsaved)");
        int key[2] = { 20, 21 };
        CHECK(kbd_edit_toggle_leds(&f, 0, key, 2) == 1 && f.zones[0].n == 6 && !kbd_edit_equal(&e, &f),
              "Z: click on a key adds its LEDs to the zone -> unsaved");
        int k13[1] = { 13 };
        CHECK(kbd_edit_toggle_leds(&f, 0, k13, 1) == 0 && !kbd_zone_has_led(&f.zones[0], 13),
              "Z: click on a key of the zone removes it");
        CHECK(kbd_edit_toggle_leds(&f, 0, k13, 1) == 1 && !kbd_zone_has_led(&f.zones[1], 13),
              "Z: adding a key takes it out of the other zones");
        CHECK(kbd_edit_zone_at_led(&f, 13) == 0 && kbd_edit_zone_at_led(&f, 119) == 1 &&
              kbd_edit_zone_at_led(&f, 50) == -1, "Z: zone shown by an LED (none: base color)");
        CHECK(kbd_zone_name_problem(&e, "Arrows", -1) && !kbd_zone_name_problem(&e, "Arrows", 1) &&
              kbd_zone_name_problem(&e, "a|b", -1) && kbd_zone_name_problem(&e, "a;b", -1) &&
              kbd_zone_name_problem(&e, "a::b", -1) && kbd_zone_name_problem(&e, "", -1) &&
              !kbd_zone_name_problem(&e, "Numpad", -1),
              "Z: zone names: unique, not empty, without | ; ::");
        memset(&f, 0, sizeof f);
        g = g_string_new(NULL);
        kbd_edit_encode(&f, g);
        CHECK(!strcmp(g->str, "-::"), "Z: empty profile -> no base, no zones");
        g_string_free(g, TRUE);
        /* dragged rectangle (v1.25.0): keys whose center is inside */
        {
            size_t ks[KBD_NUM_KEYS];
            int nk = kbd_keys_in_rect(330, 220, 120, 150, ks, KBD_NUM_KEYS);   /* corners in any order */
            int q = 0, w = 0, ee = 0, other = 0;
            for (int k = 0; k < nk; k++) {
                const char *nm = kbd_key_names[ks[k]];
                if (!strcmp(nm, "Q")) q = 1; else if (!strcmp(nm, "W")) w = 1;
                else if (!strcmp(nm, "E")) ee = 1; else other = 1;
            }
            CHECK(nk == 3 && q && w && ee && !other, "Z: rectangle over Q W E selects exactly those keys");
            nk = kbd_keys_in_rect(900, 150, 1045, 290, ks, KBD_NUM_KEYS);
            int enters = 0;
            for (int k = 0; k < nk; k++) if (!strcmp(kbd_key_names[ks[k]], "Enter")) enters++;
            CHECK(enters == 1, "Z: a key with several entries (Enter) counts once");
            CHECK(kbd_keys_in_rect(0, 0, 5, 5, ks, KBD_NUM_KEYS) == 0, "Z: rectangle without key centers selects nothing");
            CHECK(kbd_keys_in_rect(0, 0, 2000, 1000, ks, KBD_NUM_KEYS) > 90, "Z: rectangle over the whole picture selects all keys");
        }
        kbd_parse_zone_colors(&kb, "ZONES_START COUNT=0\nBASE color=1,2,3\nZONES_END\n");
        CHECK(kb.kbd_led_has_color[77] && kb.kbd_led_rgb[77][2] == 3, "Z: GET_ZONES base color shown on the picture");
    }

    /* R: keyboard daemon restarted -> the GUI reconnects and resends (v1.18.6) */
    {
        unlink(KBD_SOCKET_PATH);
        int lst = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(a.sun_path, sizeof a.sun_path, "%s", KBD_SOCKET_PATH);
        bind(lst, (struct sockaddr *)&a, sizeof a);
        listen(lst, 4);
        static AppData ka;  /* zeroed */
        ka.kbd_socket_fd = -1;
        memset(ka.kbd_wizard_mapping, -1, sizeof ka.kbd_wizard_mapping);
        CHECK(kbd_ensure_connected(&ka) == 1, "R: GUI connects to the keyboard daemon");
        int c1 = accept(lst, NULL, NULL);
        close(c1);                       /* the daemon goes away ... */
        close(lst);
        unlink(KBD_SOCKET_PATH);
        lst = socket(AF_UNIX, SOCK_STREAM, 0);   /* ... and comes back */
        bind(lst, (struct sockaddr *)&a, sizeof a);
        listen(lst, 4);
        send_keyboard_command(&ka, "SET_KEYS_COLOR::1,2,3::5");
        int c2 = accept4(lst, NULL, NULL, SOCK_NONBLOCK);
        char got[8192] = "";
        ssize_t n = c2 >= 0 ? recv(c2, got, sizeof got - 1, 0) : -1;
        if (n > 0) got[n] = '\0';
        CHECK(c2 >= 0 && strstr(got, "SET_KEYS_COLOR::1,2,3::5") != NULL,
              "R: after a daemon restart the command is resent on a new connection");
        if (c2 >= 0) close(c2);
        close(lst);
        unlink(KBD_SOCKET_PATH);
        if (ka.kbd_socket_fd >= 0) close(ka.kbd_socket_fd);
    }

    /* L: About page links can be clicked repeatedly (v1.19.1: was a double free).
     * The scheme is unknown, so nothing is opened. ASan aborts on a use after free. */
    {
        gchar *url = g_strdup("dcc-test-no-handler://about");
        GdkEventButton ev = { .type = GDK_BUTTON_PRESS, .button = 1 };
        on_link_clicked(NULL, &ev, url);
        on_link_clicked(NULL, &ev, url);
        ev.button = 3;
        gboolean right = on_link_clicked(NULL, &ev, url);
        ev.button = 1; ev.type = GDK_2BUTTON_PRESS;
        gboolean dbl = on_link_clicked(NULL, &ev, url);
        CHECK(strcmp(url, "dcc-test-no-handler://about") == 0 && !right && !dbl,
              "L: repeated left/right clicks keep the link URL; double-click event ignored");
        g_free(url);   /* owned by the caller (the widget), freed exactly once */
    }

    printf("\n%d failure(s)\n", fails);
    return fails != 0;
}
