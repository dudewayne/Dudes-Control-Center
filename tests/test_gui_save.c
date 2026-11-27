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
#include <sys/prctl.h>
#include <sys/wait.h>

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("PASS  %s\n", m); else { printf("FAIL  %s\n", m); fails++; } } while (0)

/* --- tab switch refused with real clicks (v1.38.1) --- */
static int tab_asked;
static void tab_on_switch(GtkNotebook *nb, GtkWidget *page, guint num, gpointer d) {
    AppData *a = d;
    (void)page;
    if (a->switching_page) return;
    if (gtk_notebook_get_current_page(nb) == 0 && num != 0) {   /* "unsaved changes": Cancel */
        tab_asked++;
        notebook_refuse_switch(a, nb, 0, (int)num);
    }
}
static void tab_pump(void) {
    for (int k = 0; k < 4; k++) { g_usleep(50000); while (gtk_events_pending()) gtk_main_iteration_do(FALSE); }
}
static void tab_click(GtkWidget *nb, int n) {
    GtkWidget *lab = gtk_notebook_get_tab_label(GTK_NOTEBOOK(nb), gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), n));
    GtkAllocation al;
    gtk_widget_get_allocation(lab, &al);
    GdkWindow *win = gtk_widget_get_window(nb), *ev = NULL;
    for (GList *l = gdk_window_peek_children(win); l; l = l->next) {
        gpointer ud = NULL;
        gdk_window_get_user_data(l->data, &ud);
        if (ud == nb) ev = l->data;              /* the notebook's tab input window */
    }
    int ex = 0, ey = 0;
    if (ev) gdk_window_get_position(ev, &ex, &ey); else ev = win;
    for (int t = 0; t < 2; t++) {
        GdkEvent *e = gdk_event_new(t ? GDK_BUTTON_RELEASE : GDK_BUTTON_PRESS);
        e->button.window = g_object_ref(ev);
        e->button.button = 1;
        e->button.x = al.x + al.width / 2 - ex;
        e->button.y = al.y + al.height / 2 - ey;
        e->button.time = GDK_CURRENT_TIME;
        gdk_event_set_device(e, gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default())));
        gtk_main_do_event(e);
        gdk_event_free(e);
    }
    tab_pump();
}

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
    /* GLib takes XDG_CONFIG_HOME over HOME (set on CI runners): keep all
     * config and data writes inside the test home (v1.40.0) */
    char xdg[1100];
    snprintf(xdg, sizeof xdg, "%s/.config", home);
    setenv("XDG_CONFIG_HOME", xdg, 1);
    snprintf(xdg, sizeof xdg, "%s/.local/share", home);
    setenv("XDG_DATA_HOME", xdg, 1);

    char logdir[1024]; snprintf(logdir, sizeof logdir, "%s/logs", home);
    log_init("GUI", LOG_LEVEL_INFO, logdir);
    kbd_layout_load("de");   /* keyboard tests use the German layout (1.28.0) */
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
    gui_event("[12:00:00.000] [GUI] Hello from gui_event\n");
    for (int i = 0; i < 5; i++) gui_event("[GUI] periodic status line\n");
    g_print("[GUI] trace line from g_print\n");             /* DEBUG since 1.34.3 */
    g_print("[GUI] Failed to do something\n");              /* raised to ERROR */
    g_printerr("[GUI] Something on stderr\n");
    g_warning("a GTK-style warning");
    log_cleanup();
    char lp[1100]; snprintf(lp, sizeof lp, "%s/GUI.log", logdir);
    gchar *lg = NULL; g_file_get_contents(lp, &lg, NULL, NULL);
    int periodic = 0; for (char *q = lg; (q = strstr(q, "periodic status line")); q++) periodic++;
    CHECK(lg && strstr(lg, "[INFO ] [GUI] Hello from gui_event"), "F: events go to GUI.log without the duplicate timestamp");
    CHECK(lg && !strstr(lg, "trace line from g_print"), "F: g_print traces are DEBUG, not in the log at INFO (v1.34.3)");
    CHECK(lg && strstr(lg, "[ERROR] [GUI] Failed to do something"), "F: g_print with 'Failed' is still logged as an error");
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

    /* I: UI languages (v1.31.0): catalogs from build-test/locale */
    {
        /* glibc notices a changed LANGUAGE only after setlocale() */
        #define RELOAD_LANG() do { char cur_[64]; snprintf(cur_, sizeof cur_, "%s", setlocale(LC_MESSAGES, NULL)); \
                                   setlocale(LC_MESSAGES, "C"); setlocale(LC_MESSAGES, cur_); } while (0)
        const char *old_lang = getenv("LANGUAGE");
        char *saved = old_lang ? strdup(old_lang) : NULL;
        setlocale(LC_MESSAGES, "C");
        ui_language_ensure_messages_locale();   /* as the GUI does in the C locale */
        bindtextdomain(DCC_TEXT_DOMAIN, DCC_LOCALE_DIR);
        bind_textdomain_codeset(DCC_TEXT_DOMAIN, "UTF-8");
        textdomain(DCC_TEXT_DOMAIN);
        struct { const char *lang, *want; } cases[] = {
            { "de", "Einstellungen" }, { "es", "Ajustes" }, { "ja", "\u8a2d\u5b9a" }, { "ko", "\uc124\uc815" },
        };
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            setenv("LANGUAGE", cases[i].lang, 1);
            RELOAD_LANG();
            const char *got = _("Settings");
            if (strcmp(got, cases[i].want)) { printf("      %s: Settings -> %s\n", cases[i].lang, got); ok = 0; }
        }
        CHECK(ok, "I: 'Settings' in German, Spanish, Japanese and Korean");
        setenv("LANGUAGE", "en_GB", 1);
        RELOAD_LANG();
        CHECK(!strcmp(_("Custom color"), "Custom colour"), "I: English (UK) spelling (colour)");
        setenv("LANGUAGE", "en", 1);
        RELOAD_LANG();
        CHECK(!strcmp(_("Settings"), "Settings"), "I: 'en' shows the original English texts");
        char buf[128];
        setenv("LANGUAGE", "de", 1);
        RELOAD_LANG();
        snprintf(buf, sizeof buf, _("Profile '%s' activated"), "Silent");
        CHECK(!strcmp(buf, "Profil 'Silent' aktiviert"), "I: translated format string keeps its placeholder");
        if (saved) { setenv("LANGUAGE", saved, 1); free(saved); } else unsetenv("LANGUAGE");
        setlocale(LC_MESSAGES, "C");
        CHECK(UI_NUM_LANGUAGES == 7, "I: language list: system, US, UK, German, Spanish, Japanese, Korean");
    }

    /* G: log views keep only complete, valid lines (v1.32.1) */
    {
        /* chunk that starts in the middle of "°" and of a line, and ends inside a line */
        const char raw[] = "\xb0" "C] cut line\n[1] 66\xc2\xb0" "C ok\n[2] unfinished \xc3";
        char *b = malloc(sizeof raw);
        memcpy(b, raw, sizeof raw);
        size_t used = log_complete_lines(&b, sizeof raw - 1, 1);
        CHECK(used == strlen("\xb0" "C] cut line\n[1] 66\xc2\xb0" "C ok\n") && !strcmp(b, "[1] 66\xc2\xb0" "C ok\n") &&
              g_utf8_validate(b, -1, NULL),
              "G: partial first and last lines dropped, the rest is valid UTF-8");
        free(b);
        const char bad[] = "[3] broken \xff byte\n";
        b = malloc(sizeof bad);
        memcpy(b, bad, sizeof bad);
        used = log_complete_lines(&b, sizeof bad - 1, 0);
        CHECK(used == sizeof bad - 1 && g_utf8_validate(b, -1, NULL) && strstr(b, "broken") && strstr(b, "byte"),
              "G: invalid bytes in a line are replaced, the line is kept");
        free(b);
        b = malloc(16);
        strcpy(b, "no newline");
        CHECK(log_complete_lines(&b, 10, 0) == 0 && b[0] == '\0', "G: no complete line yet -> nothing used");
        free(b);
    }

    /* A: appearance (v1.29.0): theme variants and light pictures */
    {
        char c[4][128];
        int n = theme_variant_candidates("Mint-Y-Aqua", 1, c);
        CHECK(n >= 1 && !strcmp(c[0], "Mint-Y-Dark-Aqua"), "A: dark variant of Mint-Y-Aqua is Mint-Y-Dark-Aqua");
        n = theme_variant_candidates("Mint-Y-Dark-Aqua", 0, c);
        CHECK(n >= 1 && !strcmp(c[0], "Mint-Y-Aqua"), "A: light variant of Mint-Y-Dark-Aqua is Mint-Y-Aqua");
        n = theme_variant_candidates("Mint-L-Darker-Aqua", 0, c);
        CHECK(n >= 1 && !strcmp(c[0], "Mint-L-Aqua"), "A: light variant of Mint-L-Darker-Aqua is Mint-L-Aqua");
        CHECK(theme_variant_candidates("Mint-Y-Dark-Aqua", 1, c) == 0 && theme_variant_candidates("Adwaita", 0, c) == 0,
              "A: a theme that already is the wanted variant needs no other");
        n = theme_variant_candidates("Adwaita", 1, c);
        CHECK(n >= 1 && !strcmp(c[0], "Adwaita-Dark"), "A: single-part theme name -> name-Dark");
        int all = 1;
        const char *codes[] = { "de", "uk", "es", "jp", "kr" };
        for (int i = 0; i < 5; i++) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s/keyboard_light.svg", DCC_LAYOUT_DIR, codes[i]);
            if (access(path, R_OK) != 0) all = 0;
        }
        CHECK(all, "A: every layout has a light keyboard picture");
    }

    /* L: keyboard layouts from keys.csv (v1.28.0) */
    {
        const char *codes[] = { "de", "uk", "es", "jp", "kr" };
        const size_t counts[] = { 103, 103, 103, 104, 102 };
        int all = 1;
        for (int i = 0; i < 5; i++) {
            static KbdLayoutData L;
            if (kbd_layout_parse(codes[i], &L) != 0 || L.n != counts[i]) {
                printf("      layout %s: %zu keys\n", codes[i], L.n);
                all = 0;
            }
        }
        CHECK(all, "L: all five layouts load with their number of keys (DE/UK/ES 103, JP 104, KR 102)");
        static KbdLayoutData L;
        CHECK(kbd_layout_parse("xx", &L) == -1 && kbd_layout_parse("../de", &L) == -1, "L: unknown layout codes are refused");
        CHECK(kbd_layout_load("uk") == 0 && kbd_layout_index_of(";") >= 0 && kbd_layout_index_of("Enter") >= 0,
              "L: quoted CSV fields read (the UK key named ';')");
        int q = kbd_legacy_to_current(42);     /* legacy "Q" */
        CHECK(q >= 0 && !strcmp(kbd_key_names[q], "Q"), "L: German key -> UK key at the same position (Q)");
        static AppData lm;
        kbd_mapping_set_defaults(&lm);
        int mapped = 0;
        for (int l = 0; l < 120; l++) if (lm.kbd_wizard_mapping[l] >= 0) mapped++;
        CHECK(mapped > 90, "L: UK built-in mapping derived from the German one (most LEDs have a key)");
        /* mapping file per layout */
        lm.kbd_wizard_mapping[5] = kbd_layout_index_of("Enter");
        kbd_save_led_mapping_csv(&lm);
        static AppData lr;
        kbd_load_led_mapping_csv(&lr);
        CHECK(!memcmp(lr.kbd_wizard_mapping, lm.kbd_wizard_mapping, sizeof lm.kbd_wizard_mapping),
              "L: UK LED mapping saved to and read from its own file");
        CHECK(kbd_layout_load("jp") == 0 && kbd_layout_index_of("Henkan") >= 0, "L: Japanese layout has Henkan");
        kbd_mapping_set_defaults(&lm);
        int a = kbd_layout_index_of("A"), led_a = -1;
        for (int l = 0; l < 120; l++) if (lm.kbd_wizard_mapping[l] == a) led_a = l;
        CHECK(a >= 0 && led_a >= 0, "L: Japanese layout gets a derived LED for the letter keys (A)");
        /* row matching (1.28.0): keys with a German counterpart get its LED */
        {
            const char *codes[] = { "uk", "es", "jp", "kr" };
            for (int c = 0; c < 4; c++) {
                kbd_layout_load(codes[c]);
                static AppData dm;
                kbd_mapping_set_defaults(&dm);
                char list[1024] = "";
                for (size_t k = 0; k < kbd_num_keys; k++) {
                    int has = 0;
                    for (int l = 0; l < 120; l++) if (dm.kbd_wizard_mapping[l] == (int)k) has = 1;
                    if (!has && strlen(list) < 900) { strcat(list, " ["); strcat(list, kbd_key_names[k]); strcat(list, "]"); }
                }
                printf("INFO  layout %s, keys without LED:%s\n", codes[c], list[0] ? list : " none");
            }
            kbd_layout_load("jp");
            static AppData jm;
            kbd_mapping_set_defaults(&jm);
            int lb = kbd_layout_index_of("["), sl = kbd_layout_index_of("/"), yen = kbd_layout_index_of("\\");   /* Ro key */
            int has_lb = 0, has_sl = 0, has_yen = 0;
            for (int l = 0; l < 120; l++) {
                if (jm.kbd_wizard_mapping[l] == lb) has_lb = 1;
                if (jm.kbd_wizard_mapping[l] == sl) has_sl = 1;
                if (yen >= 0 && jm.kbd_wizard_mapping[l] == yen) has_yen = 1;
            }
            CHECK(lb >= 0 && sl >= 0 && has_lb && has_sl, "L: Japanese '[' and '/' get the LEDs of the German keys at their place");
            CHECK(yen >= 0 && !has_yen, "L: Japanese '\\' (Ro, no German counterpart) has no LED until the wizard ran");
            int rc = kbd_layout_index_of("Ctrl right"), up = kbd_layout_index_of("Arrow up"), has_rc = 0, has_up = 0;
            for (int l = 0; l < 120; l++) {
                if (jm.kbd_wizard_mapping[l] == rc) has_rc = 1;
                if (jm.kbd_wizard_mapping[l] == up) has_up = 1;
            }
            CHECK(has_rc && has_up, "L: Japanese Ctrl right and Arrow up (one row lower) get the German keys' LEDs");
            int dup = 0;
            for (size_t k = 0; k < kbd_num_keys; k++) {
                /* a target key gets the LEDs of at most one German key */
                int src = -1;
                for (int l = 0; l < 120; l++) {
                    if (jm.kbd_wizard_mapping[l] != (int)k) continue;
                    int legacy = kbd_default_led_position[l];
                    int g = legacy >= 0 ? legacy : -1;
                    const char *nm = g >= 0 ? kbd_legacy_key_names[g] : "";
                    static char last[64];
                    if (src < 0) { src = 1; snprintf(last, sizeof last, "%s", nm); }
                    else if (strcmp(last, nm)) dup = 1;
                }
            }
            CHECK(!dup, "L: no Japanese key gets the LEDs of two different German keys");
            kbd_layout_load("kr");
            static AppData km;
            kbd_mapping_set_defaults(&km);
            int bs = kbd_layout_index_of("\\"), has_bs = 0;
            for (int l = 0; l < 120; l++) if (bs >= 0 && km.kbd_wizard_mapping[l] == bs) has_bs = 1;
            CHECK(bs >= 0 && !has_bs, "L: Korean '\\' above Enter (no German counterpart) has no LED until the wizard ran");
        }

        /* German: an old mapping file (before 1.28.0) is converted once */
        CHECK(kbd_layout_load("de") == 0, "L: back to German");
        char old[1024], neu[1024];
        snprintf(old, sizeof old, "%s/.config/dudes-control-center/keyboard_led_mapping.csv", home);
        snprintf(neu, sizeof neu, "%s/.config/dudes-control-center/keyboard_led_mapping_de.csv", home);
        unlink(neu);
        g_file_set_contents(old, "led_id;position;led_index;numbering=name\n31;31;\n82;80;\n32;;\n35;35;\n34;34;\n", -1, NULL);
        static AppData lo;
        kbd_load_led_mapping_csv(&lo);
        int sz = kbd_layout_index_of("\u00df"), lt = kbd_layout_index_of("<"), bs = kbd_layout_index_of("Backspace");
        CHECK(lo.kbd_wizard_mapping[31] == sz && lo.kbd_wizard_mapping[82] == lt && lo.kbd_wizard_mapping[32] == -1 &&
              lo.kbd_wizard_mapping[34] == bs && lo.kbd_wizard_mapping[35] == bs && access(neu, F_OK) == 0,
              "L: old German mapping converted (both Backspace LEDs on the one Backspace key), new file written");
    }

    /* K: keyboard key -> LED lookup (v1.17.3) */
    CHECK(sizeof kbd_default_led_position / sizeof kbd_default_led_position[0] == 120 && kbd_num_keys == 103,
          "K: default LED table for 120 LEDs, German layout has 103 keys");
    int ok_names = 1;
    for (int i = 0; i < (int)kbd_num_keys; i++)
        if (!kbd_key_names[i] || kbd_key_positions[i].w <= 0) ok_names = 0;
    CHECK(ok_names, "K: every key has a name and a non-empty position");
    static AppData kb;  /* zeroed */
    int leds[120], n;
    /* built-in mapping (1.22.0): LED -> key, several LEDs per key, unused LEDs */
    kbd_mapping_set_defaults(&kb);
    {
        int ok = 1, used[KBD_MAX_KEYS] = {0};
        for (int l = 0; l < 120; l++) {
            int p = kb.kbd_wizard_mapping[l];
            if (p < -1 || p >= (int)kbd_num_keys) ok = 0;
            if (p >= 0) used[p]++;
        }
        CHECK(ok, "K: built-in mapping only names existing keys");
        int pos_nde = -1, pos_ins = -1, pos_sz = -1, pos_lt = -1, pos_space = -1;
        for (int i = 0; i < (int)kbd_num_keys; i++) {
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
        for (size_t i = 0; i < (int)kbd_num_keys; i++) {
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

    /* K: one entry per key since 1.28.0; Numpad Enter stays a rectangle */
    {
        size_t ne[3], n = 0, m = 0;
        for (size_t i = 0; i < kbd_num_keys; i++) {
            if (!strcmp(kbd_key_names[i], "Numpad Enter") && n < 3) ne[n++] = i;
            if (!strcmp(kbd_key_names[i], "Enter")) m++;
        }
        CHECK(n == 1 && m == 1 && kbd_key_group_primary(ne[0]) == ne[0],
              "K: Numpad Enter and Enter are one entry each");
        double lx, sy;
        kbd_key_shape(ne[0], &lx, &sy);
        CHECK(lx == kbd_key_positions[ne[0]].x, "K: Numpad Enter is not L-shaped");
        static AppData g;  /* zeroed */
        memset(g.kbd_wizard_mapping, -1, sizeof g.kbd_wizard_mapping);
        g.kbd_wizard_mapping[10] = (int)ne[0];
        g.kbd_wizard_mapping[11] = (int)ne[0];
        int leds[120];
        int k = kbd_leds_for_key(&g, ne[0], leds, 120);
        CHECK(k == 2, "K: a key with two LEDs gets both");
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

    /* S: components switched off in the Settings tab (v1.27.0) */
    {
        g_mkdir_with_parents("components", 0755);
        unlink("components/disabled-fan"); unlink("components/disabled-cpu"); unlink("components/disabled-keyboard");
        CHECK(!dcc_component_disabled(COMP_FAN) && !dcc_component_disabled(COMP_CPU) && !dcc_component_disabled(COMP_KEYBOARD),
              "S: no flag files -> every component on");
        g_file_set_contents("components/disabled-keyboard", "", 0, NULL);
        CHECK(dcc_component_disabled(COMP_KEYBOARD) && !dcc_component_disabled(COMP_FAN),
              "S: flag file disabled-keyboard -> only the keyboard is off");
        static AppData sa;   /* zeroed */
        sa.kbd_socket_fd = -1;
        connect_to_keyboard_daemon(&sa);
        CHECK(!sa.kbd_connected && sa.kbd_socket_fd == -1, "S: a switched-off component is not connected");
        unlink("components/disabled-keyboard");
        CHECK(!dcc_component_disabled(COMP_KEYBOARD) && !dcc_component_disabled(-1) && !dcc_component_disabled(7),
              "S: flag removed -> on again; invalid component numbers are 'on'");
    }

    /* W: a cancelled tab switch asks again on the next click of that tab (v1.38.1) */
    if (gtk_init_check(NULL, NULL)) {
        static AppData ta;
        GtkWidget *w = gtk_offscreen_window_new();
        ta.notebook = gtk_notebook_new();
        for (int i = 0; i < 3; i++) {
            char tl[16];
            snprintf(tl, sizeof tl, "Tab %d", i);
            gtk_notebook_append_page(GTK_NOTEBOOK(ta.notebook), gtk_label_new(tl), gtk_label_new(tl));
        }
        g_signal_connect(ta.notebook, "switch-page", G_CALLBACK(tab_on_switch), &ta);
        gtk_container_add(GTK_CONTAINER(w), ta.notebook);
        gtk_window_set_default_size(GTK_WINDOW(w), 400, 200);
        gtk_widget_show_all(w);
        tab_pump();
        tab_click(ta.notebook, 2);
        tab_click(ta.notebook, 2);
        CHECK(tab_asked == 2 && gtk_notebook_get_current_page(GTK_NOTEBOOK(ta.notebook)) == 0,
              "W: Cancel keeps the tab, and clicking the same tab again asks again");
        gtk_widget_destroy(w);
    }

    /* Q: single instance ignores zombies and other programs (v1.38.0) */
    {
        pid_t z = fork();
        if (z == 0) _exit(0);                 /* exits, not reaped yet: a zombie */
        usleep(200000);
        int zombie_seen = gui_pid_alive(z);
        pid_t g = fork();
        if (g == 0) { prctl(PR_SET_NAME, "dcc-gui"); pause(); _exit(0); }
        usleep(200000);
        int gui_seen = gui_pid_alive(g);
        int other_seen = gui_pid_alive(getpid());   /* this test program */
        kill(g, SIGKILL);
        waitpid(g, NULL, 0);
        waitpid(z, NULL, 0);
        CHECK(!zombie_seen, "Q: a quit GUI that is still a zombie does not count as running");
        CHECK(gui_seen, "Q: a running dcc-gui process counts as running");
        CHECK(!other_seen && !gui_pid_alive(999999), "Q: another program with that PID or no process: not running");
    }

    /* V: refresh interval setting (v1.40.0) */
    {
        CHECK(refresh_sec_load() == 2, "V: no setting -> 2 s (the old Status interval)");
        refresh_sec_save(5);
        CHECK(refresh_sec_load() == 5, "V: saved interval read back");
        cJSON *cj = config_read();
        CHECK(cJSON_GetObjectItem(cj, "refresh_interval") && cJSON_GetObjectItem(cj, "refresh_interval")->valueint == 5,
              "V: stored as refresh_interval in config.json");
        cJSON_Delete(cj);
        refresh_sec_save(7);
        CHECK(refresh_sec_load() == 2, "V: a value that is not offered -> 2 s");
        CHECK(refresh_kbd_ticks(1) == 6 && refresh_kbd_ticks(2) == 3 && refresh_kbd_ticks(3) == 2 &&
              refresh_kbd_ticks(5) == 2 && refresh_kbd_ticks(10) == 1,
              "V: keyboard sync stays about every 6 s (at least once per refresh)");
        static AppData ra;
        ra.refresh_sec = 3;
        refresh_timers_start(&ra);
        guint t1 = ra.refresh_timer_id, c1 = ra.cpu_refresh_timer_id;
        ra.refresh_sec = 1;
        refresh_timers_start(&ra);
        CHECK(t1 && c1 && ra.refresh_timer_id && ra.cpu_refresh_timer_id &&
              !g_main_context_find_source_by_id(NULL, t1) && !g_main_context_find_source_by_id(NULL, c1),
              "V: changing the interval replaces both timers (old ones removed)");
        g_source_remove(ra.refresh_timer_id);
        g_source_remove(ra.cpu_refresh_timer_id);
        cj = config_read();
        cJSON_DeleteItemFromObject(cj, "refresh_interval");
        config_write(cj);
        cJSON_Delete(cj);
    }

    /* X: start minimized to tray, desktop entries, second start (v1.40.0) */
    {
        CHECK(start_minimized_load() == 1 && !start_minimized_is_set(), "X: not set -> start minimized (as before)");
        start_minimized_save(0);
        CHECK(start_minimized_load() == 0 && start_minimized_is_set(), "X: off saved and read back");
        start_minimized_save(1);
        CHECK(start_minimized_load() == 1, "X: on saved and read back");

        int ch = 0;
        gchar *r = desktop_exec_rewrite("[Desktop Entry]\nName=DCC\nExec=/usr/local/bin/dcc-gui\nIcon=x\n", 1, &ch);
        CHECK(ch && !strcmp(r, "[Desktop Entry]\nName=DCC\nExec=/usr/local/bin/dcc-gui --minimized\nIcon=x\n"),
              "X: on adds --minimized to the Exec line");
        g_free(r);
        r = desktop_exec_rewrite("[Desktop Entry]\nExec=dcc-gui --debug --minimized\n", 0, &ch);
        CHECK(ch && !strcmp(r, "[Desktop Entry]\nExec=dcc-gui --debug\n"), "X: off removes it, other options kept");
        g_free(r);
        r = desktop_exec_rewrite("[Desktop Entry]\nExec=dcc-gui --show\n", 1, &ch);
        CHECK(ch && !strcmp(r, "[Desktop Entry]\nExec=dcc-gui --minimized\n"), "X: --show replaced");
        g_free(r);
        r = desktop_exec_rewrite("[Desktop Entry]\nExec=/usr/bin/plank\n", 1, &ch);
        CHECK(!ch && !strcmp(r, "[Desktop Entry]\nExec=/usr/bin/plank\n"), "X: other programs untouched");
        g_free(r);
        r = desktop_exec_rewrite("[Desktop Entry]\nExec=dcc-gui --minimized\n[Desktop Action x]\nExec=dcc-gui --show\n", 1, &ch);
        CHECK(!ch && strstr(r, "Action x]\nExec=dcc-gui --show"), "X: only [Desktop Entry] changes; unchanged -> not changed");
        g_free(r);
        r = desktop_exec_rewrite("[Desktop Entry]\nExec=dcc-gui-other\n", 1, &ch);
        CHECK(!ch, "X: a program that only starts with dcc-gui is untouched");
        g_free(r);

        char dx[1100], sysf[1200], apps[1200], aut[1200], uf[1300], af[1300], of[1300], lf[1300];
        snprintf(dx, sizeof dx, "%s/desk", home);
        snprintf(sysf, sizeof sysf, "%s/sys.desktop", dx);
        snprintf(apps, sizeof apps, "%s/applications", dx);
        snprintf(aut, sizeof aut, "%s/autostart", dx);
        snprintf(uf, sizeof uf, "%s/" DESKTOP_FILE_NAME, apps);
        snprintf(af, sizeof af, "%s/dudes-control-center-1.desktop", aut);
        snprintf(of, sizeof of, "%s/plank.desktop", aut);
        snprintf(lf, sizeof lf, "%s/link.desktop", aut);
        g_mkdir_with_parents(aut, 0755);
        g_file_set_contents(sysf, "[Desktop Entry]\nName=Dudes Control Center\nExec=/usr/local/bin/dcc-gui\n", -1, NULL);
        g_file_set_contents(af, "[Desktop Entry]\nExec=/usr/local/bin/dcc-gui\nX-GNOME-Autostart-enabled=true\n", -1, NULL);
        g_file_set_contents(of, "[Desktop Entry]\nExec=plank\n", -1, NULL);
        symlink(sysf, lf);
        desktop_entries_apply(1, sysf, apps, aut);
        gchar *c = NULL;
        g_file_get_contents(uf, &c, NULL, NULL);
        CHECK(c && strstr(c, "Exec=/usr/local/bin/dcc-gui --minimized\n") && strstr(c, DESKTOP_MARKER) &&
              strstr(c, "Name=Dudes Control Center"), "X: on -> menu entry copy with --minimized in the user's applications");
        g_free(c); c = NULL;
        g_file_get_contents(af, &c, NULL, NULL);
        CHECK(c && !strcmp(c, "[Desktop Entry]\nExec=/usr/local/bin/dcc-gui --minimized\nX-GNOME-Autostart-enabled=true\n"),
              "X: on -> autostart entry gets --minimized, rest unchanged");
        g_free(c); c = NULL;
        g_file_get_contents(of, &c, NULL, NULL);
        CHECK(c && !strcmp(c, "[Desktop Entry]\nExec=plank\n"), "X: other autostart entries untouched");
        g_free(c); c = NULL;
        struct stat lst;
        g_file_get_contents(sysf, &c, NULL, NULL);
        CHECK(lstat(lf, &lst) == 0 && S_ISLNK(lst.st_mode) && c && !strstr(c, "--minimized"),
              "X: symlinked entries are not followed");
        g_free(c); c = NULL;
        desktop_entries_apply(0, sysf, apps, aut);
        g_file_get_contents(af, &c, NULL, NULL);
        CHECK(access(uf, F_OK) != 0 && c && !strstr(c, "--minimized"),
              "X: off -> our menu copy removed (system entry starts without options), autostart without --minimized");
        g_free(c); c = NULL;
        g_file_set_contents(uf, "[Desktop Entry]\nName=Mine\nExec=dcc-gui\n", -1, NULL);
        desktop_entries_apply(1, sysf, apps, aut);
        g_file_get_contents(uf, &c, NULL, NULL);
        CHECK(c && !strcmp(c, "[Desktop Entry]\nName=Mine\nExec=dcc-gui --minimized\n"),
              "X: the user's own menu copy keeps everything but the Exec option");
        g_free(c); c = NULL;
        desktop_entries_apply(0, sysf, apps, aut);
        CHECK(access(uf, F_OK) == 0, "X: ... and is never removed");

        char rt[1100];
        snprintf(rt, sizeof rt, "%s/run", home);
        g_mkdir_with_parents(rt, 0700);
        setenv("XDG_RUNTIME_DIR", rt, 1);
        CHECK(!instance_send_show(), "X: no running GUI -> second start just quits");
        static AppData ia;   /* no window: the request is read but shows nothing */
        instance_listen(&ia);
        CHECK(ia.instance_watch && instance_send_show(), "X: running GUI -> second start reaches it");
        for (int k = 0; k < 5; k++) while (g_main_context_iteration(NULL, FALSE)) {}
        g_source_remove(ia.instance_watch);
        start_minimized_save(1);
        cJSON *cj = config_read();
        cJSON_DeleteItemFromObject(cj, "start_minimized");
        config_write(cj);
        cJSON_Delete(cj);
    }

    /* Y: System section (v1.39.0) */
    {
        g_mkdir_with_parents("si/dmi", 0755);
        g_file_set_contents("si/dmi/sys_vendor", "Notebook\n", -1, NULL);
        g_file_set_contents("si/dmi/product_name", "X58xWNx\n", -1, NULL);
        g_file_set_contents("si/dmi/board_name", "X58xWNx\n", -1, NULL);
        g_file_set_contents("si/dmi/bios_vendor", "INSYDE Corp.\n", -1, NULL);
        g_file_set_contents("si/dmi/bios_version", "1.07.05\n", -1, NULL);
        g_file_set_contents("si/dmi/bios_date", "04/25/2025\n", -1, NULL);
        g_file_set_contents("si/meminfo", "MemTotal:       65506112 kB\nMemFree: 1 kB\n", -1, NULL);
        const char *bl[][3] = { { "nvme1n1", "Samsung SSD 980 PRO 2TB   ", "3907029168" },
                                { "nvme0n1", "Samsung SSD 990 PRO 4TB", "7814037168" },
                                { "nvme10n1", "Big", "1000215216" }, { "loop0", "", "100" } };
        for (int i = 0; i < 4; i++) {
            char d[96], f[128];
            snprintf(d, sizeof d, "si/block/%s/device", bl[i][0]);
            g_mkdir_with_parents(d, 0755);
            snprintf(f, sizeof f, "si/block/%s/size", bl[i][0]);
            g_file_set_contents(f, bl[i][2], -1, NULL);
            if (bl[i][1][0]) { snprintf(f, sizeof f, "%s/model", d); g_file_set_contents(f, bl[i][1], -1, NULL); }
        }
        g_file_set_contents("si/os-release", "NAME=\"Linux Mint\"\nPRETTY_NAME=\"Linux Mint 22.3\"\n", -1, NULL);
        g_mkdir_with_parents("si/pci/0000:02:00.0", 0755);
        g_file_set_contents("si/pci/0000:02:00.0/class", "0x030000\n", -1, NULL);
        g_file_set_contents("si/pci/0000:02:00.0/vendor", "0x10de\n", -1, NULL);
        g_file_set_contents("si/pci/0000:02:00.0/device", "0x2c58\n", -1, NULL);
        g_mkdir_with_parents("si/nv/0000:02:00.0", 0755);
        g_file_set_contents("si/nv/0000:02:00.0/information", "Model: \t\t NVIDIA GeForce RTX 5090 Laptop GPU\nIRQ: 16\n", -1, NULL);
        g_mkdir_with_parents("si/pci/0000:00:02.0", 0755);
        g_file_set_contents("si/pci/0000:00:02.0/class", "0x030000\n", -1, NULL);
        g_file_set_contents("si/pci/0000:00:02.0/vendor", "0x8086\n", -1, NULL);
        g_file_set_contents("si/pci/0000:00:02.0/device", "0x7d67\n", -1, NULL);
        g_mkdir_with_parents("si/pci/0000:00:1f.3", 0755);                  /* audio: not graphics */
        g_file_set_contents("si/pci/0000:00:1f.3/class", "0x040300\n", -1, NULL);
        g_file_set_contents("si/pci.ids", "# test\n8086  Intel Corporation\n\t7d67  Arrow Lake-S [Intel Graphics]\n"
                            "10de  NVIDIA Corporation\n\t2c58  GB203M [GeForce RTX 5090 Max-Q / Mobile]\n", -1, NULL);
        SysPaths sp = { "si/dmi", "si/meminfo", "si/block", "si/os-release", "si/pci", "si/nv", "si/pci.ids" };
        SysInfo si;
        sysinfo_read(&sp, &si);
        CHECK(!strcmp(si.model, "X58xWNx"), "Y: model without the placeholder vendor 'Notebook'");
        CHECK(!strcmp(si.bios, "1.07.05 (04/25/2025, INSYDE Corp.)"), "Y: BIOS version, date and vendor");
        CHECK(strstr(si.ram, "64") && (strstr(si.ram, "62.5") || strstr(si.ram, "62,5")), "Y: 64 GB installed, 62.5 GB usable");
        CHECK(!strcmp(si.drives, "SSD 1: Samsung SSD 990 PRO 4TB, 4 TB\nSSD 2: Samsung SSD 980 PRO 2TB, 2 TB\nSSD 11: Big, 512 GB"),
              "Y: drives in controller order (nvme10 after nvme1), size in TB/GB, loop devices left out");
        CHECK(!strcmp(si.os, "Linux Mint 22.3") && si.kernel[0], "Y: operating system and kernel");
        CHECK(strstr(si.gpus, "NVIDIA GeForce RTX 5090 Laptop GPU") && strstr(si.gpus, "Intel Graphics") &&
              strstr(si.gpus, " + ") && !strstr(si.gpus, "040300"),
              "Y: graphics: NVIDIA from its driver, Intel from pci.ids, audio left out");
        /* placeholder vendor and differing board name */
        g_file_set_contents("si/dmi/sys_vendor", "ASUSTeK COMPUTER INC.\n", -1, NULL);
        g_file_set_contents("si/dmi/product_name", "ROG Strix\n", -1, NULL);
        g_file_set_contents("si/dmi/board_name", "G834JY\n", -1, NULL);
        sysinfo_read(&sp, &si);
        CHECK(strstr(si.model, "ASUSTeK COMPUTER INC. ROG Strix") && strstr(si.model, "G834JY"), "Y: real vendor and board shown");
        char b[32];
        fmt_bytes(1920383410176ULL, b, sizeof b);
        CHECK(!strcmp(b, "1.9 TB") || !strcmp(b, "1,9 TB"), "Y: 1.9 TB keeps its decimal");
    }

    /* N: processor name and P/E cores (v1.38.0) */
    {
        char n[96];
        cpu_name_clean(" Intel(R) Core(TM) Ultra 9 275HX", n, sizeof n);
        CHECK(!strcmp(n, "Intel Core Ultra 9 275HX"), "N: (R) and (TM) removed");
        cpu_name_clean("Intel(R) Core(TM) i7-8750H CPU @ 2.20GHz", n, sizeof n);
        CHECK(!strcmp(n, "Intel Core i7-8750H"), "N: Intel's ' CPU @ x GHz' removed");
        cpu_name_clean("AMD Ryzen 9 7945HX with Radeon Graphics  ", n, sizeof n);
        CHECK(!strcmp(n, "AMD Ryzen 9 7945HX with Radeon Graphics"), "N: AMD name kept, trailing blanks removed");
        char mark[32] = { 0 };
        CHECK(cpulist_mark("0-3,8,10-11\n", mark, 32, 'P') == 7 && mark[3] == 'P' && mark[8] == 'P' && !mark[9] && mark[11] == 'P',
              "N: CPU list '0-3,8,10-11' read");
        /* hybrid CPU with Hyper-Threading on the P-cores: 2 P-cores (4 CPUs), 2 E-cores */
        g_mkdir_with_parents("sd/cpu_core", 0755);
        g_mkdir_with_parents("sd/cpu_atom", 0755);
        g_file_set_contents("sd/cpu_core/cpus", "0-3\n", -1, NULL);
        g_file_set_contents("sd/cpu_atom/cpus", "4-5\n", -1, NULL);
        const char *sib[6] = { "0-1\n", "0-1\n", "2-3\n", "2-3\n", "4\n", "5\n" };
        for (int c = 0; c < 6; c++) {
            char d[64], f[96];
            snprintf(d, sizeof d, "sd/system/cpu/cpu%d/topology", c);
            g_mkdir_with_parents(d, 0755);
            snprintf(f, sizeof f, "%s/core_cpus_list", d);
            g_file_set_contents(f, sib[c], -1, NULL);
        }
        g_file_set_contents("cpuinfo", "processor\t: 0\nmodel name\t: Intel(R) Core(TM) Test 1234H\nflags\t\t: fpu\n", -1, NULL);
        CpuIdent ci;
        cpu_ident_read("cpuinfo", "sd", &ci);
        char t[160];
        cpu_ident_text(&ci, t, sizeof t);
        CHECK(ci.hybrid && ci.p_cores == 2 && ci.e_cores == 2 && ci.type[1] == 'P' && ci.type[5] == 'E',
              "N: hybrid CPU: 2 P-cores (Hyper-Threading counted once), 2 E-cores");
        CHECK(strstr(t, "Intel Core Test 1234H") && strstr(t, "2") && strstr(t, "P"), "N: Processor text with name and core counts");
        cpu_ident_read("cpuinfo", "nowhere", &ci);
        cpu_ident_text(&ci, t, sizeof t);
        CHECK(!ci.hybrid && !strcmp(t, "Intel Core Test 1234H") && !ci.type[0], "N: not hybrid: name only, no P/E labels");
    }

    /* O: fan control state from the fan daemon (v1.41.0) */
    if (gtk_init_check(NULL, NULL)) {
        static AppData ea;
        ea.socket_fd = ea.cpu_socket_fd = ea.kbd_socket_fd = -1;
        GtkWidget *ew = gtk_offscreen_window_new();
        gtk_container_add(GTK_CONTAINER(ew), create_status_page(&ea));
        gtk_widget_show_all(ew);
        parse_status(&ea, "STATUS OK\nCPU_TEMP=48\nGPU_TEMP=0\nCPU_FAN_RPM=0\nGPU_FAN_RPM=0\nFAN_DUTY=50\nAUTO_MODE=1\n"
                          "EC_ACCESS=none\nEC_DIRECT=disabled\nEC_REASON=no dcc_fan module and direct EC access is not enabled\nGPU_STATE=none\n");
        gchar *tip = gtk_widget_get_tooltip_text(ea.fan_ec_label);
        CHECK(gtk_widget_get_visible(ea.fan_ec_label) && strstr(gtk_label_get_text(GTK_LABEL(ea.fan_ec_label)), "firmware") &&
              tip && strstr(tip, "not enabled"), "O: no EC access -> 'firmware controls the fans', the reason as tooltip");
        g_free(tip);
        CHECK(!gtk_widget_get_visible(ea.manual_row) && !strcmp(gtk_label_get_text(GTK_LABEL(ea.cpu_fan_rpm_label)), "CPU Fan: --"),
              "O: no EC access -> manual row hidden, fan values unknown");
        parse_status(&ea, "STATUS OK\nCPU_TEMP=48\nCPU_FAN_RPM=3000\nAUTO_MODE=1\nEC_ACCESS=ports\nEC_DIRECT=enabled\nGPU_STATE=none\n");
        CHECK(gtk_widget_get_visible(ea.fan_ec_label) && strstr(gtk_label_get_text(GTK_LABEL(ea.fan_ec_label)), "Direct EC access") &&
              gtk_widget_get_visible(ea.manual_row), "O: direct EC access -> shown in Fan Speed, manual row back");
        parse_status(&ea, "STATUS OK\nCPU_TEMP=48\nCPU_FAN_RPM=3000\nAUTO_MODE=1\nEC_ACCESS=module\nGPU_STATE=none\n");
        CHECK(!gtk_widget_get_visible(ea.fan_ec_label) && gtk_widget_get_visible(ea.manual_row), "O: dcc_fan module -> no note");
        parse_status(&ea, "STATUS OK\nCPU_TEMP=48\nCPU_FAN_RPM=2900\nAUTO_MODE=1\nEC_ACCESS=hwmon\nHWMON_FANS=enabled\n"
                          "HWMON_FANS_DRIVER=thinkpad\nGPU_STATE=none\n");
        CHECK(gtk_widget_get_visible(ea.fan_ec_label) && strstr(gtk_label_get_text(GTK_LABEL(ea.fan_ec_label)), "thinkpad") &&
              gtk_widget_get_visible(ea.manual_row), "O: generic fan control -> named in Fan Speed, manual row shown");
        /* AMD GPUs (1.42.0) */
        parse_status(&ea, "STATUS OK\nCPU_TEMP=48\nAUTO_MODE=1\nEC_ACCESS=module\nGPU_STATE=awake\nGPU_NAME=AMD Radeon 780M Graphics\n"
                          "GPU_POWER_W=9.0\nGPU_CLOCK_MHZ=800\nGPU_MEM_CLOCK_MHZ=-1\nGPU_UTIL=4\nGPU_VRAM_USED_MB=-1\n"
                          "GPU_VRAM_TOTAL_MB=-1\nGPU_PSTATE=\n");
        gchar *want_pw = g_strdup_printf("GPU: %.1f W", 9.0);   /* decimal sign of the locale */
        CHECK(!strcmp(gtk_label_get_text(GTK_LABEL(ea.gpu_clock_label)), "Clock: 800 MHz") &&
              !strcmp(gtk_label_get_text(GTK_LABEL(ea.gpu_load_label)), "Load: 4 %") &&
              !strcmp(gtk_label_get_text(GTK_LABEL(ea.pw_gpu_label)), want_pw),
              "O: integrated AMD GPU: clock without memory clock, load without power state");
        g_free(want_pw);
        parse_status(&ea, "STATUS OK\nCPU_TEMP=48\nAUTO_MODE=1\nEC_ACCESS=module\nGPU_STATE=none\n");
        CHECK(!strcmp(gtk_label_get_text(GTK_LABEL(ea.gpu_name_label)), "No NVIDIA or AMD GPU"),
              "O: no supported GPU -> 'No NVIDIA or AMD GPU'");
        gtk_widget_destroy(ew);

        /* the warning opens without a nested main loop (it froze the GUI in the 1.41.0 test) */
        static AppData sa;
        sa.socket_fd = sa.cpu_socket_fd = sa.kbd_socket_fd = -1;
        sa.window = gtk_offscreen_window_new();
        GtkWidget *sp = create_settings_page(&sa);
        gtk_container_add(GTK_CONTAINER(sa.window), sp);
        gtk_widget_show_all(sa.window);
        g_strlcpy(sa.ec_access, "none", sizeof sa.ec_access);
        settings_update_ec(&sa);
        CHECK(on_settings_ec_switch(GTK_SWITCH(sa.settings_ec_switch), TRUE, &sa) && sa.ec_dialog &&
              !strcmp(gtk_window_get_title(GTK_WINDOW(sa.ec_dialog)), "Direct EC access"),
              "O: switching on returns at once with the titled warning open");
        settings_update_ec(&sa);                      /* a refresh while it is open */
        CHECK(!gtk_widget_get_sensitive(sa.settings_ec_switch) && !sa.settings_busy,
              "O: while the warning is open the switch waits, nothing runs");
        CHECK(on_settings_ec_switch(GTK_SWITCH(sa.settings_ec_switch), TRUE, &sa) && sa.ec_dialog,
              "O: a second click does not open a second warning");
        gtk_dialog_response(GTK_DIALOG(sa.ec_dialog), GTK_RESPONSE_CANCEL);
        CHECK(!sa.ec_dialog && !sa.settings_busy && gtk_widget_get_sensitive(sa.settings_ec_switch) &&
              !gtk_switch_get_active(GTK_SWITCH(sa.settings_ec_switch)),
              "O: Cancel closes it, the switch stays off, no pkexec");
        /* generic fan control (1.43.0) */
        CHECK(on_settings_hw_switch(GTK_SWITCH(sa.settings_hw_switch), TRUE, &sa) && sa.ec_dialog &&
              !strcmp(gtk_window_get_title(GTK_WINDOW(sa.ec_dialog)), "Generic fan control") &&
              !gtk_widget_get_sensitive(sa.settings_ec_switch),
              "O: generic fan control: the short warning opens, the other switch waits");
        gtk_dialog_response(GTK_DIALOG(sa.ec_dialog), GTK_RESPONSE_CANCEL);
        CHECK(!sa.ec_dialog && !gtk_switch_get_active(GTK_SWITCH(sa.settings_hw_switch)) &&
              gtk_widget_get_sensitive(sa.settings_hw_switch), "O: generic fan control: Cancel leaves it off");
        g_strlcpy(sa.ec_access, "hwmon", sizeof sa.ec_access);
        g_strlcpy(sa.hw_driver, "thinkpad", sizeof sa.hw_driver);
        settings_update_ec(&sa);
        CHECK(!strcmp(gtk_label_get_text(GTK_LABEL(sa.settings_hw_state)), "On, in use: thinkpad") &&
              !strcmp(gtk_label_get_text(GTK_LABEL(sa.settings_ec_state)), "Not needed: generic fan control in use"),
              "O: generic fan control in use: both rows say so");
        gtk_widget_destroy(sa.window);

        /* Keyboard tab per kind of keyboard backlight (1.44.0) */
        static AppData ka;
        ka.socket_fd = ka.cpu_socket_fd = ka.kbd_socket_fd = -1;
        GtkWidget *kw = gtk_offscreen_window_new();
        gtk_container_add(GTK_CONTAINER(kw), create_keyboard_page(&ka));
        gtk_widget_show_all(kw);
        CHECK(gtk_widget_get_visible(ka.kbd_zone_frame) && gtk_widget_get_visible(ka.kbd_wizard_box) &&
              !gtk_widget_get_visible(ka.kbd_kind_label), "O: keyboard: before the daemon answers everything is shown");
        kbd_apply_kind(&ka, "STATUS OK\nLED_COUNT=1\nKBD_TYPE=rgb\nKBD_DRIVER=asus-nb-wmi\n");
        gtk_widget_show_all(kw);                      /* showing the window again must not undo it */
        CHECK(ka.kbd_kind == KBD_UI_RGB && gtk_widget_get_visible(ka.kbd_preset_frame) &&
              gtk_widget_get_visible(ka.kbd_profile_frame) && !gtk_widget_get_visible(ka.kbd_zone_frame) &&
              !gtk_widget_get_visible(ka.kbd_wizard_box) && !gtk_widget_get_visible(ka.kbd_whole_btn) &&
              !strcmp(gtk_label_get_text(GTK_LABEL(ka.kbd_kind_label)),
                      "Keyboard: one color for the whole keyboard (asus-nb-wmi driver)"),
              "O: keyboard: one color -> profiles and colors, no key picture, zones or LED Wizard");
        CHECK(gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(ka.kbd_fx_combo)), NULL) == 3 &&
              !gtk_widget_get_visible(ka.kbd_fx_dir), "O: keyboard: one color -> no Wave and no direction (1.44.1)");
        kbd_apply_kind(&ka, "STATUS OK\nLED_COUNT=3\nKBD_TYPE=rgb\nKBD_DRIVER=\n");
        CHECK(!strcmp(gtk_label_get_text(GTK_LABEL(ka.kbd_kind_label)), "Keyboard: 3 color zones, one color for all of them"),
              "O: keyboard: a few zones without a driver name");
        kbd_apply_kind(&ka, "STATUS OK\nLED_COUNT=1\nKBD_TYPE=white\nKBD_DRIVER=thinkpad_acpi\n");
        gtk_widget_show_all(kw);
        CHECK(ka.kbd_kind == KBD_UI_WHITE && gtk_widget_get_visible(ka.kbd_backlight_frame) &&
              !gtk_widget_get_visible(ka.kbd_preset_frame) && !gtk_widget_get_visible(ka.kbd_profile_frame) &&
              !gtk_widget_get_visible(ka.kbd_zone_frame) && !gtk_widget_get_visible(ka.kbd_off_note),
              "O: keyboard: single color -> only on/off and brightness");
        kbd_apply_kind(&ka, "STATUS OK\nLED_COUNT=0\nKBD_TYPE=none\nKBD_DRIVER=\n");
        CHECK(!gtk_widget_get_visible(ka.kbd_backlight_frame) && gtk_widget_get_visible(ka.kbd_kind_label),
              "O: keyboard: none found -> only the note");
        kbd_apply_kind(&ka, "STATUS OK\nLED_COUNT=120\nKBD_TYPE=per-key\nKBD_DRIVER=ite_829x\n");
        CHECK(ka.kbd_kind == KBD_UI_PER_KEY && gtk_widget_get_visible(ka.kbd_zone_frame) &&
              gtk_widget_get_visible(ka.kbd_wizard_box) && gtk_widget_get_visible(ka.kbd_whole_btn) &&
              gtk_widget_get_visible(ka.kbd_backlight_frame),
              "O: keyboard: per-key RGB -> everything again");
        CHECK(gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(ka.kbd_fx_combo)), NULL) == 4 &&
              gtk_widget_get_visible(ka.kbd_fx_dir), "O: keyboard: per-key RGB -> Wave and direction back");

        /* System report dialog (1.45.0): not modal, one at a time, Copy */
        report_dialog_open(NULL);
        GtkWidget *rd = report_dialog;
        GtkTextBuffer *rtb = rd ? g_object_get_data(G_OBJECT(rd), "report-buffer") : NULL;
        gchar *rtext = NULL;
        if (rtb) { GtkTextIter a1, e1; gtk_text_buffer_get_bounds(rtb, &a1, &e1); rtext = gtk_text_buffer_get_text(rtb, &a1, &e1, FALSE); }
        CHECK(rd && !gtk_window_get_modal(GTK_WINDOW(rd)) && rtext && g_str_has_prefix(rtext, "Dudes Control Center system report") &&
              strstr(rtext, "Version: " APP_VERSION), "O: System Report opens a non-modal dialog with the report");
        g_free(rtext);
        report_dialog_open(NULL);
        CHECK(report_dialog == rd, "O: a second click shows the same dialog");
        gtk_dialog_response(GTK_DIALOG(rd), 1);
        CHECK(report_dialog == rd, "O: Copy keeps the dialog open");
        gtk_dialog_response(GTK_DIALOG(rd), GTK_RESPONSE_CLOSE);
        CHECK(report_dialog == NULL, "O: Close closes it");
        gtk_widget_destroy(kw);
    } else {
        printf("SKIP  O: no display\n");
    }

    /* P: Status tab power, battery, NVMe, GPU (v1.37.0) */
    {
        g_mkdir_with_parents("ps/BAT0", 0755);
        g_mkdir_with_parents("ps/AC", 0755);
        g_file_set_contents("ps/AC/type", "Mains\n", -1, NULL);
        g_file_set_contents("ps/AC/online", "0\n", -1, NULL);
        g_file_set_contents("ps/BAT0/type", "Battery\n", -1, NULL);
        g_file_set_contents("ps/BAT0/status", "Discharging\n", -1, NULL);
        g_file_set_contents("ps/BAT0/capacity", "64\n", -1, NULL);
        g_file_set_contents("ps/BAT0/current_now", "2000000\n", -1, NULL);    /* 2 A */
        g_file_set_contents("ps/BAT0/voltage_now", "15000000\n", -1, NULL);   /* 15 V */
        g_file_set_contents("ps/BAT0/charge_now", "4500000\n", -1, NULL);     /* 4.5 Ah */
        g_file_set_contents("ps/BAT0/charge_full", "5926000\n", -1, NULL);
        g_file_set_contents("ps/BAT0/charge_full_design", "6300000\n", -1, NULL);
        g_file_set_contents("ps/BAT0/cycle_count", "0\n", -1, NULL);
        StatusBattery b;
        status_battery_read("ps", &b);
        char c[128], st[128], h[128];
        status_battery_texts(&b, c, st, h, sizeof c);
        CHECK(b.present && b.on_ac == 0 && b.capacity == 64 && b.power_w > 29.9 && b.power_w < 30.1 &&
              b.hours > 2.24 && b.hours < 2.26 && b.health == 94,
              "P: battery from charge/current/voltage: 30 W, 2.25 h left, 94 % health");
        CHECK(strstr(st, "30.0 W") || strstr(st, "30,0 W"), "P: discharging text shows the watts");
        CHECK(strstr(st, "2:15 h") && !strstr(h, "cycles"), "P: time left as h:mm, 0 cycles left out");
        /* energy-based battery (uWh/uW), charging */
        g_mkdir_with_parents("ps2/BAT1", 0755);
        g_file_set_contents("ps2/BAT1/type", "Battery\n", -1, NULL);
        g_file_set_contents("ps2/BAT1/status", "Charging\n", -1, NULL);
        g_file_set_contents("ps2/BAT1/capacity", "50\n", -1, NULL);
        g_file_set_contents("ps2/BAT1/power_now", "40000000\n", -1, NULL);     /* 40 W */
        g_file_set_contents("ps2/BAT1/energy_now", "40000000\n", -1, NULL);    /* 40 Wh */
        g_file_set_contents("ps2/BAT1/energy_full", "80000000\n", -1, NULL);
        g_file_set_contents("ps2/BAT1/energy_full_design", "80000000\n", -1, NULL);
        g_file_set_contents("ps2/BAT1/cycle_count", "120\n", -1, NULL);
        status_battery_read("ps2", &b);
        status_battery_texts(&b, c, st, h, sizeof c);
        CHECK(b.power_w > 39.9 && b.power_w < 40.1 && b.hours > 0.99 && b.hours < 1.01 && strstr(st, "1:00 h") &&
              strstr(h, "100 %") && strstr(h, "120"), "P: energy-based battery, charging: full in 1:00 h, cycles shown");
        status_battery_read("nowhere", &b);
        status_battery_texts(&b, c, st, h, sizeof c);
        CHECK(!b.present && !strcmp(c, _("No battery")), "P: no battery");
        /* NVMe drives from hwmon */
        /* sensors in hwmon order nvme1, nvme0 (like on the X58xWNx); device -> controller */
        g_mkdir_with_parents("ctrl/nvme0", 0755);
        g_mkdir_with_parents("ctrl/nvme1", 0755);
        g_mkdir_with_parents("hw/hwmon1", 0755);
        g_mkdir_with_parents("hw/hwmon2", 0755);
        g_mkdir_with_parents("hw/hwmon3", 0755);
        g_file_set_contents("hw/hwmon1/name", "coretemp\n", -1, NULL);
        g_file_set_contents("hw/hwmon2/name", "nvme\n", -1, NULL);
        g_file_set_contents("hw/hwmon2/temp1_input", "32150\n", -1, NULL);
        symlink("../../ctrl/nvme1", "hw/hwmon2/device");
        g_file_set_contents("hw/hwmon3/name", "nvme\n", -1, NULL);
        g_file_set_contents("hw/hwmon3/temp1_input", "35850\n", -1, NULL);
        symlink("../../ctrl/nvme0", "hw/hwmon3/device");
        char nv[256];
        status_nvme_text("hw", nv, sizeof nv);
        CHECK(!strcmp(nv, "SSD 1: 36°C\nSSD 2: 32°C"), "P: NVMe temperatures as SSD n by controller, sorted, other sensors ignored");
        /* GPU fields of the fan daemon's STATUS */
        StatusGpu g = { "", "", -1, -1, -1, -1, -1, -1, "" };
        status_gpu_field(&g, "STATE=awake");
        status_gpu_field(&g, "NAME=NVIDIA GeForce RTX 5090 Laptop GPU");
        status_gpu_field(&g, "POWER_W=26.3");
        status_gpu_field(&g, "VRAM_TOTAL_MB=24463");
        CHECK(!strcmp(g.state, "awake") && g.power > 26.29 && g.power < 26.31 && g.vram_total == 24463,
              "P: GPU fields of STATUS parsed");
        /* daemon numbers keep their decimals in a comma locale */
        char *old = g_strdup(setlocale(LC_NUMERIC, NULL));
        static const char *const comma[] = { "de_DE.UTF-8", "de_AT.UTF-8", "de_CH.UTF-8", "fr_FR.UTF-8", "es_ES.UTF-8", NULL };
        int have = 0;
        for (int i = 0; comma[i] && !have; i++) have = setlocale(LC_NUMERIC, comma[i]) != NULL;
        if (have) {
            CHECK(atof("26.3") < 26.29 && num_c("26.3") > 26.29 && num_c("26.3") < 26.31,
                  "P: 26.3 from a daemon stays 26.3 in a decimal-comma locale (atof gave 26)");
        } else {
            printf("SKIP  P: no decimal-comma locale installed\n");
        }
        setlocale(LC_NUMERIC, old);
        g_free(old);
    }

    /* E: effects in the zone editor model (v1.35.0) */
    {
        static KbdProfileEdit e, f;
        CHECK(kbd_edit_parse(&e, "PROFILE_START COUNT=1 name=Fx\n"
                                 "BASE color=1,2,3 effect=wave:4:up\n"
                                 "ZONE 0 name=W A S D color=255,0,0 effect=breathing:2 keys=2 10 11\n"
                                 "PROFILE_END\n") == 0 &&
              !strcmp(e.base_fx, "wave:4:up") && !strcmp(e.zones[0].fx, "breathing:2") &&
              !strcmp(e.zones[0].name, "W A S D") && e.zones[0].n == 2 && e.zones[0].leds[1] == 11,
              "E: GET_PROFILE with effects read: base and zone effect, name and keys unchanged");
        GString *g = g_string_new(NULL);
        kbd_edit_encode(&e, g);
        CHECK(!strcmp(g->str, "1,2,3@wave:4:up::W A S D|255,0,0@breathing:2|10,11"), "E: PREVIEW encoding with effects");
        g_string_free(g, TRUE);
        f = e;
        snprintf(f.zones[0].fx, sizeof f.zones[0].fx, "breathing:3");
        CHECK(kbd_edit_equal(&e, &e) && !kbd_edit_equal(&e, &f), "E: a changed effect counts as an unsaved change");
        int t, sp, d;
        char out[24];
        kbd_fx_split("wave:7:left", &t, &sp, &d);
        kbd_fx_join(t, sp, d, out, sizeof out);
        CHECK(t == 3 && sp == 7 && d == 1 && !strcmp(out, "wave:7:left"), "E: effect text split and joined");
        kbd_fx_join(0, 9, 2, out, sizeof out);
        kbd_fx_split("", &t, &sp, &d);
        CHECK(out[0] == '\0' && t == 0 && sp == 5, "E: Static is stored as no effect, defaults for empty text");
        kbd_fx_join(2, 10, 0, out, sizeof out);
        CHECK(!strcmp(out, "rainbow:10"), "E: Rainbow has no direction");
    }

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
            size_t ks[KBD_MAX_KEYS];
            int nk = kbd_keys_in_rect(330, 220, 120, 150, ks, KBD_MAX_KEYS);   /* corners in any order */
            int q = 0, w = 0, ee = 0, other = 0;
            for (int k = 0; k < nk; k++) {
                const char *nm = kbd_key_names[ks[k]];
                if (!strcmp(nm, "Q")) q = 1; else if (!strcmp(nm, "W")) w = 1;
                else if (!strcmp(nm, "E")) ee = 1; else other = 1;
            }
            CHECK(nk == 3 && q && w && ee && !other, "Z: rectangle over Q W E selects exactly those keys");
            nk = kbd_keys_in_rect(900, 150, 1045, 290, ks, KBD_MAX_KEYS);
            int enters = 0;
            for (int k = 0; k < nk; k++) if (!strcmp(kbd_key_names[ks[k]], "Enter")) enters++;
            CHECK(enters == 1, "Z: a key with several entries (Enter) counts once");
            CHECK(kbd_keys_in_rect(0, 0, 5, 5, ks, KBD_MAX_KEYS) == 0, "Z: rectangle without key centers selects nothing");
            CHECK(kbd_keys_in_rect(0, 0, 2000, 1000, ks, KBD_MAX_KEYS) > 90, "Z: rectangle over the whole picture selects all keys");
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

    /* A: About logo follows the appearance (v1.34.4), transparent background */
    if (gtk_init_check(NULL, NULL)) {
        GdkPixbuf *d = about_logo_load(1), *l = about_logo_load(0);
        CHECK(d && l && gdk_pixbuf_get_width(d) == 230 && gdk_pixbuf_get_has_alpha(d) && gdk_pixbuf_get_has_alpha(l),
              "A: dark and light About logos load at 230 px with alpha");
        if (d && l) {
            const guchar *pd = gdk_pixbuf_get_pixels(d), *pl = gdk_pixbuf_get_pixels(l);
            CHECK(pd[3] == 0 && pl[3] == 0, "A: logo corners are transparent (no square on the window)");
        }
        about_logo_dark = 1;
        GtkWidget *about = create_about_page();
        g_object_ref_sink(about);
        GdkPixbuf *shown = gtk_image_get_pixbuf(GTK_IMAGE(about_logo_image));
        about_logo_set(0);
        GdkPixbuf *shown2 = gtk_image_get_pixbuf(GTK_IMAGE(about_logo_image));
        CHECK(shown && shown2 && shown2 != shown, "A: switching to light mode replaces the About logo");
        g_object_unref(about);
        about_logo_image = NULL;
        if (d) g_object_unref(d);
        if (l) g_object_unref(l);
    }

    /* H: Help tab (v1.34.0): Markdown shown with text tags */
    if (gtk_init_check(NULL, NULL)) {
        GtkTextBuffer *b = gtk_text_buffer_new(NULL);
        md_render(b,
            "# Title\n\n"
            "![badge](https://img.example/b.svg)\n"
            "Some **bold** and *italic* text with `co*de` and a [link](https://example.com).\n"
            "- item one\n"
            "  - nested\n"
            "1. first\n"
            "> quoted\n"
            "```\n"
            "raw **not bold**\n"
            "```\n"
            "| A | B |\n|---|---|\n| `x` | **y** |\n"
            "See [License](LICENSE), [docs](docs/a.md) and [Title](#title).\n"
            "## [1.0.0] - 2026-01-01\n"
            "[![Shot](docs/s.png)](docs/s.png)\n"
            "\n"
            "[1.0.0]: https://example.com/compare\n");
        GtkTextIter s, e;
        gtk_text_buffer_get_bounds(b, &s, &e);
        gchar *t = gtk_text_buffer_get_text(b, &s, &e, FALSE);
        CHECK(g_str_has_prefix(t, "Title\n") && !strstr(t, "badge") && !strstr(t, "img.example"),
              "H: heading without '#', image line left out");
        CHECK(strstr(t, "Some bold and italic text with co*de and a link.\n") != NULL,
              "H: inline bold, italic, code and link markers removed");
        CHECK(strstr(t, "\xe2\x80\xa2 item one\n\xe2\x97\xa6 nested\n1. first\nquoted\n") != NULL, "H: lists and quote");
        CHECK(strstr(t, "raw **not bold**\n") != NULL, "H: code block shown as written");
        CHECK(strstr(t, "A   B\nx   y\n") != NULL, "H: small table aligned in columns");
        CHECK(strstr(t, "1.0.0 - 2026-01-01\nShot\n") != NULL && !strstr(t, "https://example.com/compare"),
              "H: reference link in a heading, linked image shows its text, definitions hidden");
        /* tags and URLs */
        GtkTextIter it;
        const char *p = strstr(t, "bold");
        if (!p) p = t;   /* the CHECKs above report it */
        gtk_text_buffer_get_iter_at_offset(b, &it, (gint)g_utf8_pointer_to_offset(t, p));
        CHECK(gtk_text_iter_has_tag(&it, gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(b), "bold")), "H: bold text has the bold tag");
        p = strstr(t, "link.");
        if (!p) p = t;   /* the CHECKs above report it */
        gtk_text_buffer_get_iter_at_offset(b, &it, (gint)g_utf8_pointer_to_offset(t, p));
        const char *u = help_url_at(&it);
        CHECK(u && strcmp(u, "https://example.com") == 0, "H: link carries its URL");
        p = strstr(t, "1.0.0 -");
        if (!p) p = t;   /* the CHECKs above report it */
        gtk_text_buffer_get_iter_at_offset(b, &it, (gint)g_utf8_pointer_to_offset(t, p));
        u = help_url_at(&it);
        CHECK(u && strcmp(u, "https://example.com/compare") == 0, "H: reference link resolved from its definition");
        CHECK(gtk_text_buffer_get_mark(b, "md:title") && gtk_text_buffer_get_mark(b, "md:100---2026-01-01"),
              "H: headings get GitHub-style anchors");
        g_free(t);
        g_object_unref(b);

        int tab; gchar *target;
        CHECK(help_link_target("#quick-start", &tab, &target) == HELP_LINK_ANCHOR && strcmp(target, "quick-start") == 0, "H: #anchor scrolls in the same text");
        g_free(target);
        CHECK(help_link_target("LICENSE", &tab, &target) == HELP_LINK_TAB && tab == HELP_LICENSE && !target, "H: LICENSE opens the License sub-tab");
        g_free(target);
        CHECK(help_link_target("CHANGELOG.md#1330", &tab, &target) == HELP_LINK_TAB && tab == HELP_CHANGELOG && strcmp(target, "1330") == 0, "H: CHANGELOG.md#x opens the Changelog at x");
        g_free(target);
        CHECK(help_link_target("drivers/README.md", &tab, &target) == HELP_LINK_WEB &&
              strcmp(target, DCC_REPO_URL "/blob/main/drivers/README.md") == 0, "H: other files open on GitHub");
        g_free(target);
        gchar *slug = md_slug("Credits & Acknowledgments");
        CHECK(strcmp(slug, "credits--acknowledgments") == 0, "H: anchor of a heading with '&' like on GitHub");
        g_free(slug);

        /* the real files from the repository (DCC_DOC_DIR in the test build) */
        GtkWidget *page = create_help_page();
        g_object_ref_sink(page);
        gtk_widget_show_all(page);   /* the notebook only switches to shown pages */
        help_load();
        const char *expect[HELP_COUNT] = { "Dudes Control Center\n", "Changelog - Dudes Control Center\n", "GNU GENERAL PUBLIC LICENSE" };
        for (int i = 0; i < HELP_COUNT; i++) {
            GtkTextBuffer *hb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(help.views[i]));
            gtk_text_buffer_get_bounds(hb, &s, &e);
            gchar *ht = gtk_text_buffer_get_text(hb, &s, &e, FALSE);
            char msg[128];
            snprintf(msg, sizeof msg, "H: %s loaded and shown", help_files[i]);
            CHECK(strstr(ht, expect[i]) != NULL, msg);
            if (i != HELP_LICENSE) {
                snprintf(msg, sizeof msg, "H: no Markdown link or image syntax left in %s", help_files[i]);
                CHECK(!strstr(ht, "](") && !strstr(ht, "![") && !strstr(ht, "\n#"), msg);
            }
            g_free(ht);
        }
        CHECK(gtk_text_buffer_get_mark(gtk_text_view_get_buffer(GTK_TEXT_VIEW(help.views[HELP_README])), "md:quick-start") != NULL,
              "H: README contents links have their headings");
        gtk_entry_set_text(GTK_ENTRY(help.search), "hard limit");
        gtk_notebook_set_current_page(GTK_NOTEBOOK(help.notebook), HELP_README);
        help_search(0, 1);
        GtkTextBuffer *rb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(help.views[HELP_README]));
        gchar *sel = gtk_text_buffer_get_selection_bounds(rb, &s, &e) ? gtk_text_buffer_get_text(rb, &s, &e, FALSE) : NULL;
        CHECK(sel && g_ascii_strcasecmp(sel, "hard limit") == 0, "H: search selects the match (case ignored)");
        g_free(sel);
        /* Shift+Enter goes back to the previous match, Enter forward (v1.34.0) */
        {
            GtkTextIter m1;
            gtk_text_buffer_get_selection_bounds(rb, &m1, &e);
            int first = gtk_text_iter_get_offset(&m1);
            GdkEventKey kev = { .type = GDK_KEY_PRESS, .keyval = GDK_KEY_Return, .state = 0 };
            gboolean handled = on_help_search_key(help.search, &kev, NULL);
            help_search(0, 0);                              /* what "activate" does for Enter */
            gtk_text_buffer_get_selection_bounds(rb, &s, &e);
            int second = gtk_text_iter_get_offset(&s);
            kev.state = GDK_SHIFT_MASK;
            gboolean handled_shift = on_help_search_key(help.search, &kev, NULL);
            gtk_text_buffer_get_selection_bounds(rb, &s, &e);
            CHECK(!handled && second > first && handled_shift && gtk_text_iter_get_offset(&s) == first,
                  "H: Enter finds the next match, Shift+Enter goes back to the previous one");
        }
        gtk_entry_set_text(GTK_ENTRY(help.search), "zzqx-no-such-text");
        help_search(0, 1);
        CHECK(strcmp(gtk_label_get_text(GTK_LABEL(help.search_status)), _("Not found")) == 0, "H: search says when nothing matches");
        g_object_unref(page);
    }

    /* Q: a daemon restart is not an error in the GUI log (1.44.1) */
    {
        log_init("GUI", LOG_LEVEL_INFO, logdir);
        gui_log_lost("Fan", "Failed to send command", EPIPE);
        gui_log_lost("Q-test", "Connection closed", 0);   /* EOF */
        gui_log_lost("CPU", "poll failed", EBADF);
        gui_event_conn("[GUI] Connected to Q-test daemon successfully!\n");
        gui_event_conn("[GUI] Connected to Q-test daemon successfully!\n");   /* reconnect soon after */
        int qlost = 0;
        gui_log_connected("Qx", &qlost);
        qlost = 1;
        gui_log_connected("Qx", &qlost);
        gui_event("[GUI] Q-test ordinary line\n");
        gui_event("[GUI] Q-test ordinary line\n");
        log_cleanup();
        char lp[1100];
        snprintf(lp, sizeof lp, "%s/GUI.log", logdir);
        gchar *lg = NULL;
        g_file_get_contents(lp, &lg, NULL, NULL);
        CHECK(lg && strstr(lg, "[INFO ] [GUI] Fan daemon closed the connection (restarted or stopped) - reconnecting") &&
              strstr(lg, "[INFO ] [GUI] Q-test daemon closed the connection"),
              "Q: a daemon that closed the connection (restart) is logged at INFO");
        CHECK(lg && strstr(lg, "[ERROR] [GUI] CPU ERROR: poll failed (Bad file descriptor), marking disconnected"),
              "Q: other socket errors stay errors");
        int conn = 0, ord = 0;
        for (char *q = lg; q && (q = strstr(q, "Connected to Q-test daemon")); q++) conn++;
        for (char *q = lg; q && (q = strstr(q, "Q-test ordinary line")); q++) ord++;
        CHECK(conn == 2 && ord == 1, "Q: a reconnect soon after is logged again; other repeats are still dropped");
        CHECK(lg && strstr(lg, "[INFO ] [GUI] Qx daemon connected\n") && strstr(lg, "[INFO ] [GUI] Qx daemon connected again\n") && qlost == 0,
              "Q: the same lines for every daemon: \"connected\", after a lost connection \"connected again\"");
        /* Logs tab: [ERROR] lines in red */
        GtkTextBuffer *lb = gtk_text_buffer_new(NULL);
        log_insert_top(lb, "[t] [INFO ] [GUI] old\n");
        log_insert_top(lb, "[t] [ERROR] [GUI] bad\n[t] [INFO ] [GUI] fine\n");
        GtkTextTag *red = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(lb), "log-error");
        GtkTextIter li;
        int tagged[3];
        for (int i = 0; i < 3; i++) {
            gtk_text_buffer_get_iter_at_line(lb, &li, i);
            gtk_text_iter_forward_chars(&li, 2);
            tagged[i] = red && gtk_text_iter_has_tag(&li, red);
        }
        CHECK(tagged[0] && !tagged[1] && !tagged[2], "Q: Logs tab shows only the [ERROR] line in red");
        g_object_unref(lb);
        g_free(lg);
    }

    printf("\n%d failure(s)\n", fails);
    return fails != 0;
}
