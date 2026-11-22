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

    /* K: keyboard key -> LED lookup (v1.17.3) */
    CHECK(sizeof kbd_position_to_led_id / sizeof kbd_position_to_led_id[0] == KBD_NUM_KEYS &&
          sizeof kbd_key_names / sizeof kbd_key_names[0] == KBD_NUM_KEYS &&
          sizeof kbd_key_positions / sizeof kbd_key_positions[0] == KBD_NUM_KEYS,
          "K: key name, position and default LED tables have the same size");
    int ok_names = 1;
    for (int i = 0; i < KBD_NUM_KEYS; i++)
        if (!kbd_key_names[i] || kbd_key_positions[i].w <= 0) ok_names = 0;
    CHECK(ok_names, "K: every key has a name and a non-empty position");
    static AppData kb;  /* zeroed */
    int leds[120], n;
    memset(kb.kbd_wizard_mapping, -1, sizeof kb.kbd_wizard_mapping);
    n = kbd_leds_for_position(&kb, 0, leds, 120);
    CHECK(n == 1 && leds[0] == kbd_position_to_led_id[0], "K: no wizard mapping -> default table");
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
    kbd_parse_zone_colors(&kb, "ERROR Not connected to keyboard daemon");
    CHECK(!kb.kbd_led_has_color[0], "K: error response clears the colors");

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
