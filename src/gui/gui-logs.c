// SPDX-License-Identifier: GPL-2.0-only
/*
 * gui-logs.c
 * Logs tab: the GUI's and the daemons' logs.
 * Part of dcc-gui.c, which #includes it: one translation unit, so the
 * static functions and AppData stay shared (split up in 1.48.1).
 */


/* Log views (1.32.1): keep only complete lines of a chunk read from a log
 * file and make it valid UTF-8. A chunk can start inside a line (the first
 * load starts 10 KB before the end) and end inside one (still being
 * written); a cut inside a multi-byte character (°, ü, ß...) made GTK reject
 * the text ("g_utf8_validate ... failed"). Returns the number of bytes of
 * the file used (up to the last newline); *buf is replaced by the text. */
static size_t log_complete_lines(char **buf, size_t len, int skip_first) {
    char *b = *buf;
    char *last_nl = NULL;
    for (size_t i = len; i > 0; i--)
        if (b[i - 1] == '\n') { last_nl = b + i - 1; break; }
    if (!last_nl) {
        b[0] = '\0';
        return 0;
    }
    size_t used = (size_t)(last_nl - b) + 1;
    b[used] = '\0';
    if (skip_first) {
        char *nl = strchr(b, '\n');
        memmove(b, nl + 1, strlen(nl + 1) + 1);
    }
    if (!g_utf8_validate(b, -1, NULL)) {
        gchar *valid = g_utf8_make_valid(b, -1);
        char *copy = strdup(valid);
        g_free(valid);
        if (copy) {
            free(b);
            *buf = copy;
        }
    }
    return used;
}

/* Forward declarations for log viewer */
static gboolean refresh_logs_timer(gpointer user_data);
static void load_gui_logs(AppData *app);
static void load_daemon_logs(AppData *app);
static void load_cpu_daemon_logs(AppData *app);
static void load_kbd_daemon_logs(AppData *app);

/* Create Page 4: Log Viewer */
static GtkWidget *create_log_page(AppData *app) {
    /* Create a notebook for tabbed daemon logs */
    GtkWidget *notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(notebook), 5);

    /* Tab 1: GUI Log */
    {
        GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 150);   /* was 300 (1.24.4) */

        app->gui_log_view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(app->gui_log_view), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->gui_log_view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->gui_log_view), 5);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->gui_log_view), 5);
        gtk_style_context_add_class(gtk_widget_get_style_context(app->gui_log_view), "monospace-log-view");
        app->gui_log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->gui_log_view));
        gtk_container_add(GTK_CONTAINER(scrolled), app->gui_log_view);

        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled, gtk_label_new(_("GUI Log")));
    }

    /* Tab 2: CPU Daemon Log */
    {
        GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 150);   /* was 300 (1.24.4) */

        app->cpu_daemon_log_view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(app->cpu_daemon_log_view), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->cpu_daemon_log_view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->cpu_daemon_log_view), 5);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->cpu_daemon_log_view), 5);
        gtk_style_context_add_class(gtk_widget_get_style_context(app->cpu_daemon_log_view), "monospace-log-view");
        app->cpu_daemon_log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->cpu_daemon_log_view));
        gtk_container_add(GTK_CONTAINER(scrolled), app->cpu_daemon_log_view);

        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled, gtk_label_new(_("CPU Daemon Log")));
    }

    /* Tab 3: Fan Daemon Log */
    {
        GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 150);   /* was 300 (1.24.4) */

        app->daemon_log_view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(app->daemon_log_view), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->daemon_log_view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->daemon_log_view), 5);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->daemon_log_view), 5);
        gtk_style_context_add_class(gtk_widget_get_style_context(app->daemon_log_view), "monospace-log-view");
        app->daemon_log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->daemon_log_view));
        gtk_container_add(GTK_CONTAINER(scrolled), app->daemon_log_view);

        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled, gtk_label_new(_("Fan Daemon Log")));
    }

    /* Tab 4: Keyboard Backlight Daemon Log */
    {
        GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 150);   /* was 300 (1.24.4) */

        app->kbd_daemon_log_view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(app->kbd_daemon_log_view), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->kbd_daemon_log_view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->kbd_daemon_log_view), 5);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->kbd_daemon_log_view), 5);
        gtk_style_context_add_class(gtk_widget_get_style_context(app->kbd_daemon_log_view), "monospace-log-view");
        app->kbd_daemon_log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->kbd_daemon_log_view));
        gtk_container_add(GTK_CONTAINER(scrolled), app->kbd_daemon_log_view);

        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled, gtk_label_new(_("Keyboard Backlight Daemon Log")));
    }

    /* Start the log refresh timer (every 500ms) */
    app->log_refresh_timer_id = g_timeout_add(500, refresh_logs_timer, app);

    /* Initial load of logs */
    load_gui_logs(app);
    load_daemon_logs(app);
    load_cpu_daemon_logs(app);
    load_kbd_daemon_logs(app);

    return notebook;
}

/* Timer callback to refresh logs periodically */
/* Insert new log lines at the top of a Logs tab view; lines with [ERROR]
 * are shown in red (1.44.1) */
static void log_insert_top(GtkTextBuffer *b, const char *text) {
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(b);
    GtkTextTag *red = gtk_text_tag_table_lookup(table, "log-error");
    if (!red) red = gtk_text_buffer_create_tag(b, "log-error", "foreground", "#e74c3c", NULL);
    GtkTextIter it;
    gtk_text_buffer_get_start_iter(b, &it);
    gtk_text_buffer_insert(b, &it, text, -1);
    int line = 0;
    for (const char *p = text; *p; line++) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (g_strstr_len(p, (gssize)len, "[ERROR]")) {
            GtkTextIter a, e;
            gtk_text_buffer_get_iter_at_line(b, &a, line);
            e = a;
            if (!gtk_text_iter_ends_line(&e)) gtk_text_iter_forward_to_line_end(&e);
            gtk_text_buffer_apply_tag(b, red, &a, &e);
        }
        if (!nl) break;
        p = nl + 1;
    }
}

static gboolean refresh_logs_timer(gpointer user_data) {
    AppData *app = (AppData *)user_data;

    /* Check if we're on the log page before refreshing to save resources */
    gint current_page = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    if (current_page == PAGE_LOGS) {
        load_gui_logs(app);
        load_daemon_logs(app);
        load_cpu_daemon_logs(app);
        load_kbd_daemon_logs(app);
    }

    return G_SOURCE_CONTINUE;  /* Keep the timer running */
}

/* Load GUI logs from file */
static void load_gui_logs(AppData *app) {
    const char *home = g_get_home_dir();
    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/.cache/dudes-control-center/logs/GUI.log", home);

    static long last_file_size = 0;
    static gboolean initial_load_done = FALSE;

    FILE *f = fopen(log_path, "r");
    if (!f) {
        return;  /* Log file doesn't exist yet */
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);

    if (file_size == last_file_size) {
        fclose(f);
        return;  /* No new content */
    }

    /* Handle file rotation/truncation */
    if (file_size < last_file_size) {
        last_file_size = 0;
        initial_load_done = FALSE;
        gtk_text_buffer_set_text(app->gui_log_buffer, "", 0);
    }

    /* On initial load, only read the tail to avoid blocking GTK */
    if (!initial_load_done) {
        if (file_size > 10000) {
            last_file_size = file_size - 10000;
        }
        initial_load_done = TRUE;
    }

    /* Cap new content per refresh to avoid blocking GTK after standby/large bursts */
    const long MAX_NEW_CONTENT = 65536;  // 64KB max per refresh
    long new_size = file_size - last_file_size;

    if (new_size > MAX_NEW_CONTENT) {
        /* Too much new content - only read the tail */
        new_size = MAX_NEW_CONTENT;
        last_file_size = file_size - new_size;
    }

    char *new_content = malloc(new_size + 1);
    if (!new_content) {
        fclose(f);
        return;
    }

    /* starting inside a line? (the first load starts 10 KB before the end) */
    int mid_line = 0;
    if (last_file_size > 0 && fseek(f, last_file_size - 1, SEEK_SET) == 0) mid_line = fgetc(f) != '\n';
    fseek(f, last_file_size, SEEK_SET);
    size_t bytes_read = fread(new_content, 1, new_size, f);
    new_content[bytes_read] = '\0';
    fclose(f);
    long read_up_to = last_file_size + (long)log_complete_lines(&new_content, bytes_read, mid_line);
    if (!new_content || !new_content[0]) {   /* no complete line yet */
        free(new_content);
        last_file_size = read_up_to;
        return;
    }

    /* Split into lines */
    GSList *lines = NULL;
    char *saveptr = NULL;
    char *line = strtok_r(new_content, "\n", &saveptr);
    while (line != NULL) {
        lines = g_slist_append(lines, g_strdup(line));
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(new_content);

    /* Reverse the list so newest lines come first */
    lines = g_slist_reverse(lines);

    /* Build single string with all lines, trailing newline ensures separation from existing buffer */
    GString *reversed = g_string_new(NULL);
    GSList *first = lines;
    for (GSList *l = first; l != NULL; l = l->next) {
        if (l != first) {
            g_string_append_c(reversed, '\n');
        }
        g_string_append(reversed, (gchar *)l->data);
    }
    g_string_append_c(reversed, '\n');

    /* Insert all at once at the start of the buffer */
    GtkTextIter start_iter;
    gtk_text_buffer_get_start_iter(app->gui_log_buffer, &start_iter);
    log_insert_top(app->gui_log_buffer, reversed->str);

    /* Trim buffer if too large to keep GTK responsive */
    const gsize MAX_BUFFER_CHARS = 10000;
    gsize current_length = gtk_text_buffer_get_char_count(app->gui_log_buffer);
    if (current_length > MAX_BUFFER_CHARS) {
        GtkTextIter end_iter;
        gtk_text_buffer_get_end_iter(app->gui_log_buffer, &end_iter);
        GtkTextIter trim_iter;
        gtk_text_buffer_get_iter_at_offset(app->gui_log_buffer, &trim_iter, MAX_BUFFER_CHARS);
        gtk_text_buffer_delete(app->gui_log_buffer, &trim_iter, &end_iter);
    }

    /* Cleanup */
    g_string_free(reversed, TRUE);
    for (GSList *l = lines; l != NULL; l = l->next) {
        g_free(l->data);
    }
    g_slist_free(lines);

    last_file_size = read_up_to;   /* an unfinished last line is read next time */

    /* Scroll to top */
    gtk_text_buffer_get_start_iter(app->gui_log_buffer, &start_iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->gui_log_view), &start_iter, 0.0, FALSE, 0.0, 0.0);
}

/* Load daemon logs from file */
static void load_daemon_logs(AppData *app) {
    char log_path[] = "/var/log/dudes-control-center/FAN_DAEMON.log";

    static long last_file_size = 0;
    static gboolean initial_load_done = FALSE;

    FILE *f = fopen(log_path, "r");
    if (!f) {
        /* Try to add a placeholder message if we can't read the file */
        GtkTextIter start_iter;
        gtk_text_buffer_get_start_iter(app->daemon_log_buffer, &start_iter);
        gsize current_length = gtk_text_buffer_get_char_count(app->daemon_log_buffer);

        if (current_length == 0) {
            gtk_text_buffer_insert_markup(app->daemon_log_buffer, &start_iter,
                _("<span foreground='#e74c3c'>Unable to read daemon log. Check permissions or ensure the daemon is running.</span>\n"), -1);
        }
        return;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size == last_file_size) {
        fclose(f);
        return;  /* No new content */
    }

    /* Handle file rotation/truncation */
    if (file_size < last_file_size) {
        last_file_size = 0;
        initial_load_done = FALSE;
        gtk_text_buffer_set_text(app->daemon_log_buffer, "", 0);
    }

    /* On initial load, only read the tail to avoid blocking GTK */
    if (!initial_load_done) {
        if (file_size > 10000) {
            last_file_size = file_size - 10000;
        }
        initial_load_done = TRUE;
    }

    /* Cap new content per refresh to avoid blocking GTK after standby/large bursts */
    const long MAX_NEW_CONTENT = 65536;  // 64KB max per refresh
    long new_size = file_size - last_file_size;

    if (new_size > MAX_NEW_CONTENT) {
        /* Too much new content - only read the tail */
        new_size = MAX_NEW_CONTENT;
        last_file_size = file_size - new_size;
    }

    char *new_content = malloc(new_size + 1);
    if (!new_content) {
        fclose(f);
        return;
    }

    /* starting inside a line? (the first load starts 10 KB before the end) */
    int mid_line = 0;
    if (last_file_size > 0 && fseek(f, last_file_size - 1, SEEK_SET) == 0) mid_line = fgetc(f) != '\n';
    fseek(f, last_file_size, SEEK_SET);
    size_t bytes_read = fread(new_content, 1, new_size, f);
    new_content[bytes_read] = '\0';
    fclose(f);
    long read_up_to = last_file_size + (long)log_complete_lines(&new_content, bytes_read, mid_line);
    if (!new_content || !new_content[0]) {   /* no complete line yet */
        free(new_content);
        last_file_size = read_up_to;
        return;
    }

    /* Split into lines */
    GSList *lines = NULL;
    char *saveptr = NULL;
    char *line = strtok_r(new_content, "\n", &saveptr);
    while (line != NULL) {
        lines = g_slist_append(lines, g_strdup(line));
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(new_content);

    /* Reverse the list so newest lines come first */
    lines = g_slist_reverse(lines);

    /* Build single string with all lines, trailing newline ensures separation from existing buffer */
    GString *reversed = g_string_new(NULL);
    GSList *first = lines;
    for (GSList *l = first; l != NULL; l = l->next) {
        if (l != first) {
            g_string_append_c(reversed, '\n');
        }
        g_string_append(reversed, (gchar *)l->data);
    }
    g_string_append_c(reversed, '\n');

    /* Insert all at once at the start of the buffer */
    GtkTextIter start_iter;
    gtk_text_buffer_get_start_iter(app->daemon_log_buffer, &start_iter);
    log_insert_top(app->daemon_log_buffer, reversed->str);

    /* Trim buffer if too large to keep GTK responsive */
    const gsize MAX_BUFFER_CHARS = 10000;
    gsize current_length = gtk_text_buffer_get_char_count(app->daemon_log_buffer);
    if (current_length > MAX_BUFFER_CHARS) {
        GtkTextIter end_iter;
        gtk_text_buffer_get_end_iter(app->daemon_log_buffer, &end_iter);
        GtkTextIter trim_iter;
        gtk_text_buffer_get_iter_at_offset(app->daemon_log_buffer, &trim_iter, MAX_BUFFER_CHARS);
        gtk_text_buffer_delete(app->daemon_log_buffer, &trim_iter, &end_iter);
    }

    /* Cleanup */
    g_string_free(reversed, TRUE);
    for (GSList *l = lines; l != NULL; l = l->next) {
        g_free(l->data);
    }
    g_slist_free(lines);

    last_file_size = read_up_to;   /* an unfinished last line is read next time */

    /* Scroll to top */
    gtk_text_buffer_get_start_iter(app->daemon_log_buffer, &start_iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->daemon_log_view), &start_iter, 0.0, FALSE, 0.0, 0.0);
}

/* Load CPU daemon logs from file */
static void load_cpu_daemon_logs(AppData *app) {
    char log_path[] = "/var/log/dudes-control-center/CPU_DAEMON.log";

    static long last_file_size = 0;
    static gboolean initial_load_done = FALSE;

    FILE *f = fopen(log_path, "r");
    if (!f) {
        GtkTextIter start_iter;
        gtk_text_buffer_get_start_iter(app->cpu_daemon_log_buffer, &start_iter);
        gsize current_length = gtk_text_buffer_get_char_count(app->cpu_daemon_log_buffer);

        if (current_length == 0) {
            gtk_text_buffer_insert_markup(app->cpu_daemon_log_buffer, &start_iter,
                _("<span foreground='#e74c3c'>Unable to read CPU daemon log. Check permissions or ensure the daemon is running.</span>\n"), -1);
        }
        return;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size == last_file_size) {
        fclose(f);
        return;
    }

    if (file_size < last_file_size) {
        last_file_size = 0;
        initial_load_done = FALSE;
        gtk_text_buffer_set_text(app->cpu_daemon_log_buffer, "", 0);
    }

    if (!initial_load_done) {
        if (file_size > 10000) {
            last_file_size = file_size - 10000;
        }
        initial_load_done = TRUE;
    }

    const long MAX_NEW_CONTENT = 65536;
    long new_size = file_size - last_file_size;

    if (new_size > MAX_NEW_CONTENT) {
        new_size = MAX_NEW_CONTENT;
        last_file_size = file_size - new_size;
    }

    char *new_content = malloc(new_size + 1);
    if (!new_content) {
        fclose(f);
        return;
    }

    /* starting inside a line? (the first load starts 10 KB before the end) */
    int mid_line = 0;
    if (last_file_size > 0 && fseek(f, last_file_size - 1, SEEK_SET) == 0) mid_line = fgetc(f) != '\n';
    fseek(f, last_file_size, SEEK_SET);
    size_t bytes_read = fread(new_content, 1, new_size, f);
    new_content[bytes_read] = '\0';
    fclose(f);
    long read_up_to = last_file_size + (long)log_complete_lines(&new_content, bytes_read, mid_line);
    if (!new_content || !new_content[0]) {   /* no complete line yet */
        free(new_content);
        last_file_size = read_up_to;
        return;
    }

    GSList *lines = NULL;
    char *saveptr = NULL;
    char *line = strtok_r(new_content, "\n", &saveptr);
    while (line != NULL) {
        lines = g_slist_append(lines, g_strdup(line));
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(new_content);

    lines = g_slist_reverse(lines);

    GString *reversed = g_string_new(NULL);
    GSList *first = lines;
    for (GSList *l = first; l != NULL; l = l->next) {
        if (l != first) {
            g_string_append_c(reversed, '\n');
        }
        g_string_append(reversed, (gchar *)l->data);
    }
    g_string_append_c(reversed, '\n');

    GtkTextIter start_iter;
    gtk_text_buffer_get_start_iter(app->cpu_daemon_log_buffer, &start_iter);
    log_insert_top(app->cpu_daemon_log_buffer, reversed->str);

    const gsize MAX_BUFFER_CHARS = 10000;
    gsize current_length = gtk_text_buffer_get_char_count(app->cpu_daemon_log_buffer);
    if (current_length > MAX_BUFFER_CHARS) {
        GtkTextIter end_iter;
        gtk_text_buffer_get_end_iter(app->cpu_daemon_log_buffer, &end_iter);
        GtkTextIter trim_iter;
        gtk_text_buffer_get_iter_at_offset(app->cpu_daemon_log_buffer, &trim_iter, MAX_BUFFER_CHARS);
        gtk_text_buffer_delete(app->cpu_daemon_log_buffer, &trim_iter, &end_iter);
    }

    g_string_free(reversed, TRUE);
    for (GSList *l = lines; l != NULL; l = l->next) {
        g_free(l->data);
    }
    g_slist_free(lines);

    last_file_size = read_up_to;   /* an unfinished last line is read next time */

    gtk_text_buffer_get_start_iter(app->cpu_daemon_log_buffer, &start_iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->cpu_daemon_log_view), &start_iter, 0.0, FALSE, 0.0, 0.0);
}

/* Load keyboard backlight daemon logs (same as the CPU daemon log view) */
static void load_kbd_daemon_logs(AppData *app) {
    char log_path[] = "/var/log/dudes-control-center/KBD_BACKLIGHT_DAEMON.log";

    static long last_file_size = 0;
    static gboolean initial_load_done = FALSE;

    FILE *f = fopen(log_path, "r");
    if (!f) {
        GtkTextIter start_iter;
        gtk_text_buffer_get_start_iter(app->kbd_daemon_log_buffer, &start_iter);
        gsize current_length = gtk_text_buffer_get_char_count(app->kbd_daemon_log_buffer);

        if (current_length == 0) {
            gtk_text_buffer_insert_markup(app->kbd_daemon_log_buffer, &start_iter,
                _("<span foreground='#e74c3c'>Unable to read Keyboard daemon log. Check permissions or ensure the daemon is running.</span>\n"), -1);
        }
        return;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size == last_file_size) {
        fclose(f);
        return;
    }

    if (file_size < last_file_size) {
        last_file_size = 0;
        initial_load_done = FALSE;
        gtk_text_buffer_set_text(app->kbd_daemon_log_buffer, "", 0);
    }

    if (!initial_load_done) {
        if (file_size > 10000) {
            last_file_size = file_size - 10000;
        }
        initial_load_done = TRUE;
    }

    const long MAX_NEW_CONTENT = 65536;
    long new_size = file_size - last_file_size;

    if (new_size > MAX_NEW_CONTENT) {
        new_size = MAX_NEW_CONTENT;
        last_file_size = file_size - new_size;
    }

    char *new_content = malloc(new_size + 1);
    if (!new_content) {
        fclose(f);
        return;
    }

    /* starting inside a line? (the first load starts 10 KB before the end) */
    int mid_line = 0;
    if (last_file_size > 0 && fseek(f, last_file_size - 1, SEEK_SET) == 0) mid_line = fgetc(f) != '\n';
    fseek(f, last_file_size, SEEK_SET);
    size_t bytes_read = fread(new_content, 1, new_size, f);
    new_content[bytes_read] = '\0';
    fclose(f);
    long read_up_to = last_file_size + (long)log_complete_lines(&new_content, bytes_read, mid_line);
    if (!new_content || !new_content[0]) {   /* no complete line yet */
        free(new_content);
        last_file_size = read_up_to;
        return;
    }

    GSList *lines = NULL;
    char *saveptr = NULL;
    char *line = strtok_r(new_content, "\n", &saveptr);
    while (line != NULL) {
        lines = g_slist_append(lines, g_strdup(line));
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(new_content);

    lines = g_slist_reverse(lines);

    GString *reversed = g_string_new(NULL);
    GSList *first = lines;
    for (GSList *l = first; l != NULL; l = l->next) {
        if (l != first) {
            g_string_append_c(reversed, '\n');
        }
        g_string_append(reversed, (gchar *)l->data);
    }
    g_string_append_c(reversed, '\n');

    GtkTextIter start_iter;
    gtk_text_buffer_get_start_iter(app->kbd_daemon_log_buffer, &start_iter);
    log_insert_top(app->kbd_daemon_log_buffer, reversed->str);

    const gsize MAX_BUFFER_CHARS = 10000;
    gsize current_length = gtk_text_buffer_get_char_count(app->kbd_daemon_log_buffer);
    if (current_length > MAX_BUFFER_CHARS) {
        GtkTextIter end_iter;
        gtk_text_buffer_get_end_iter(app->kbd_daemon_log_buffer, &end_iter);
        GtkTextIter trim_iter;
        gtk_text_buffer_get_iter_at_offset(app->kbd_daemon_log_buffer, &trim_iter, MAX_BUFFER_CHARS);
        gtk_text_buffer_delete(app->kbd_daemon_log_buffer, &trim_iter, &end_iter);
    }

    g_string_free(reversed, TRUE);
    for (GSList *l = lines; l != NULL; l = l->next) {
        g_free(l->data);
    }
    g_slist_free(lines);

    last_file_size = read_up_to;   /* an unfinished last line is read next time */

    gtk_text_buffer_get_start_iter(app->kbd_daemon_log_buffer, &start_iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->kbd_daemon_log_view), &start_iter, 0.0, FALSE, 0.0, 0.0);
}
