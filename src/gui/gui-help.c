// SPDX-License-Identifier: GPL-2.0-only
/*
 * gui-help.c
 * Help tab (README, changelog, license as formatted text, system report) and About tab.
 * Part of dcc-gui.c, which #includes it: one translation unit, so the
 * static functions and AppData stay shared (split up in 1.48.1).
 */


/* Create Page 5: About */
/* One credit: text with its links, centered */
static GtkWidget *about_credit(const char *markup, const char *link1, const char *link2) {
    GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *text = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(text), markup);
    gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_justify(GTK_LABEL(text), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(text), 42);
    gtk_box_pack_start(GTK_BOX(cell), text, FALSE, FALSE, 0);
    const char *links[2] = { link1, link2 };
    for (int l = 0; l < 2; l++)
        if (links[l]) {
            GtkWidget *link = create_clickable_link(links[l]);
            gtk_widget_set_halign(link, GTK_ALIGN_CENTER);
            gtk_box_pack_start(GTK_BOX(cell), link, FALSE, FALSE, 0);
        }
    return cell;
}

/* ------------------------------------------------------------------------- */
/* Help tab (1.34.0): README, Changelog and License from the package's doc   */
/* folder. README and Changelog are Markdown; md_render() shows the parts    */
/* these files use (headings, lists, quotes, code, tables, links) with text  */
/* tags. Images are left out; a linked image becomes a link with its text.   */
/* ------------------------------------------------------------------------- */
#ifndef DCC_DOC_DIR
#define DCC_DOC_DIR "/usr/share/doc/dudes-control-center"
#endif
#define DCC_REPO_URL "https://github.com/dudewayne/Dudes-Control-Center"

enum { HELP_README = 0, HELP_CHANGELOG = 1, HELP_LICENSE = 2, HELP_COUNT = 3 };
static const char *const help_files[HELP_COUNT] = { "README.md", "CHANGELOG.md", "LICENSE" };

static struct {
    GtkWidget *notebook;               /* the three sub-tabs */
    GtkWidget *views[HELP_COUNT];
    GtkWidget *search;
    GtkWidget *search_status;
    int loaded;
} help;

typedef struct {
    GtkTextBuffer *buf;                /* NULL: only collect the plain text */
    GHashTable *refs;                  /* [label]: url definitions, lower-case label */
} MdCtx;

#define MD_MAX_TAGS 8

/* GitHub's heading anchors: lower case, spaces to '-', other punctuation dropped */
static gchar *md_slug(const char *heading) {
    GString *s = g_string_new(NULL);
    for (const unsigned char *p = (const unsigned char *)heading; *p; p++) {
        if (*p >= 0x80 || g_ascii_isalnum(*p) || *p == '-' || *p == '_') g_string_append_c(s, (char)g_ascii_tolower(*p));
        else if (*p == ' ') g_string_append_c(s, '-');
    }
    return g_string_free(s, FALSE);
}

static void md_tags_create(GtkTextBuffer *b) {
    if (gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(b), "h1")) return;
    gtk_text_buffer_create_tag(b, "h1", "weight", PANGO_WEIGHT_BOLD, "scale", 1.6, "pixels-above-lines", 6, "pixels-below-lines", 8, NULL);
    gtk_text_buffer_create_tag(b, "h2", "weight", PANGO_WEIGHT_BOLD, "scale", 1.3, "pixels-above-lines", 14, "pixels-below-lines", 6, NULL);
    gtk_text_buffer_create_tag(b, "h3", "weight", PANGO_WEIGHT_BOLD, "scale", 1.1, "pixels-above-lines", 10, "pixels-below-lines", 4, NULL);
    gtk_text_buffer_create_tag(b, "bold", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(b, "italic", "style", PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(b, "code", "family", "monospace", NULL);
    gtk_text_buffer_create_tag(b, "pre", "family", "monospace", "left-margin", 30, "wrap-mode", GTK_WRAP_NONE, NULL);
    gtk_text_buffer_create_tag(b, "quote", "style", PANGO_STYLE_ITALIC, "left-margin", 25, "right-margin", 15, NULL);
    gtk_text_buffer_create_tag(b, "li1", "left-margin", 25, "indent", -14, NULL);
    gtk_text_buffer_create_tag(b, "li2", "left-margin", 45, "indent", -14, NULL);
    gtk_text_buffer_create_tag(b, "li3", "left-margin", 65, "indent", -14, NULL);
    gtk_text_buffer_create_tag(b, "row", "left-margin", 25, "indent", -14, NULL);
    gtk_text_buffer_create_tag(b, "table", "family", "monospace", "left-margin", 15, "wrap-mode", GTK_WRAP_NONE, NULL);
    gtk_text_buffer_create_tag(b, "link", "foreground", "#3498db", "underline", PANGO_UNDERLINE_SINGLE, NULL);
    gtk_text_buffer_create_tag(b, "plain", "family", "monospace", NULL);
}

/* Insert text at the end of the buffer with these tags (plain: also collect it) */
static void md_put(MdCtx *c, const char *t, gssize n, GtkTextTag **tags, int ntags, GString *plain) {
    if (n < 0) n = (gssize)strlen(t);
    if (n == 0) return;
    if (plain) g_string_append_len(plain, t, n);
    if (!c->buf) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(c->buf, &end);
    int start_off = gtk_text_iter_get_offset(&end);
    gtk_text_buffer_insert(c->buf, &end, t, (gint)n);
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_offset(c->buf, &s, start_off);
    gtk_text_buffer_get_end_iter(c->buf, &e);
    for (int i = 0; i < ntags; i++) gtk_text_buffer_apply_tag(c->buf, tags[i], &s, &e);
}

static GtkTextTag *md_tag(MdCtx *c, const char *name) {
    return c->buf ? gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(c->buf), name) : NULL;
}

/* Position of the bracket that closes the one at s[open] (nesting counted), or -1 */
static gssize md_match(const char *s, gssize n, gssize open, char o, char cl) {
    int depth = 0;
    for (gssize i = open; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) { i++; continue; }
        if (s[i] == o) depth++;
        else if (s[i] == cl && --depth == 0) return i;
    }
    return -1;
}

static void md_inline(MdCtx *c, const char *s, gssize n, GtkTextTag **tags, int ntags, GString *plain);

/* Link text with the link tag plus a tag that carries the URL */
static void md_link(MdCtx *c, const char *text, gssize tn, const char *url, gssize un,
                    GtkTextTag **tags, int ntags, GString *plain) {
    GtkTextTag *t[MD_MAX_TAGS + 2];
    int k = 0;
    for (int i = 0; i < ntags && k < MD_MAX_TAGS; i++) t[k++] = tags[i];
    if (c->buf) {
        GtkTextTag *u = gtk_text_buffer_create_tag(c->buf, NULL, NULL);
        g_object_set_data_full(G_OBJECT(u), "dcc-url", g_strndup(url, (gsize)un), g_free);
        t[k++] = md_tag(c, "link");
        t[k++] = u;
    }
    md_inline(c, text, tn, t, k, plain);
}

static void md_inline(MdCtx *c, const char *s, gssize n, GtkTextTag **tags, int ntags, GString *plain) {
    if (n < 0) n = (gssize)strlen(s);
    gssize run = 0, i = 0;
#define MD_FLUSH() do { if (i > run) md_put(c, s + run, i - run, tags, ntags, plain); } while (0)
    while (i < n) {
        char ch = s[i];
        if (ch == '\\' && i + 1 < n && g_ascii_ispunct(s[i + 1])) {
            MD_FLUSH(); md_put(c, s + i + 1, 1, tags, ntags, plain); i += 2; run = i; continue;
        }
        if (ch == '`') {
            const char *e = memchr(s + i + 1, '`', (size_t)(n - i - 1));
            if (e) {
                MD_FLUSH();
                GtkTextTag *t[MD_MAX_TAGS + 1]; int k = 0;
                for (int j = 0; j < ntags && k < MD_MAX_TAGS; j++) t[k++] = tags[j];
                if (c->buf) t[k++] = md_tag(c, "code");
                md_put(c, s + i + 1, e - (s + i + 1), t, k, plain);
                i = e - s + 1; run = i; continue;
            }
        }
        if (ch == '*' && i + 1 < n && s[i + 1] == '*') {
            const char *e = g_strstr_len(s + i + 2, n - i - 2, "**");
            if (e && e > s + i + 2) {
                MD_FLUSH();
                GtkTextTag *t[MD_MAX_TAGS + 1]; int k = 0;
                for (int j = 0; j < ntags && k < MD_MAX_TAGS; j++) t[k++] = tags[j];
                if (c->buf) t[k++] = md_tag(c, "bold");
                md_inline(c, s + i + 2, e - (s + i + 2), t, k, plain);
                i = e - s + 2; run = i; continue;
            }
        }
        if (ch == '*' && i + 1 < n && s[i + 1] != ' ' && s[i + 1] != '*') {
            gssize e = -1;
            for (gssize j = i + 1; j < n; j++)
                if (s[j] == '*' && s[j - 1] != ' ' && (j + 1 >= n || s[j + 1] != '*')) { e = j; break; }
            if (e > 0) {
                MD_FLUSH();
                GtkTextTag *t[MD_MAX_TAGS + 1]; int k = 0;
                for (int j = 0; j < ntags && k < MD_MAX_TAGS; j++) t[k++] = tags[j];
                if (c->buf) t[k++] = md_tag(c, "italic");
                md_inline(c, s + i + 1, e - i - 1, t, k, plain);
                i = e + 1; run = i; continue;
            }
        }
        if (ch == '!' && i + 1 < n && s[i + 1] == '[') {      /* image: left out */
            gssize cb = md_match(s, n, i + 1, '[', ']');
            if (cb > 0 && cb + 1 < n && s[cb + 1] == '(') {
                gssize cp = md_match(s, n, cb + 1, '(', ')');
                if (cp > 0) { MD_FLUSH(); i = cp + 1; run = i; continue; }
            }
        }
        if (ch == '[') {
            gssize cb = md_match(s, n, i, '[', ']');
            if (cb > 0) {
                const char *text = s + i + 1; gssize tn = cb - i - 1;
                /* a linked image shows its alt text */
                if (tn > 2 && text[0] == '!' && text[1] == '[') {
                    gssize ab = md_match(text, tn, 1, '[', ']');
                    if (ab > 0) { text += 2; tn = ab - 2; }
                }
                if (cb + 1 < n && s[cb + 1] == '(') {
                    gssize cp = md_match(s, n, cb + 1, '(', ')');
                    if (cp > 0) {
                        MD_FLUSH();
                        md_link(c, text, tn, s + cb + 2, cp - cb - 2, tags, ntags, plain);
                        i = cp + 1; run = i; continue;
                    }
                } else if (c->refs) {
                    gchar *label = g_ascii_strdown(s + i + 1, cb - i - 1);
                    const char *url = g_hash_table_lookup(c->refs, label);
                    g_free(label);
                    if (url) {
                        MD_FLUSH();
                        md_link(c, text, tn, url, (gssize)strlen(url), tags, ntags, plain);
                        i = cb + 1; run = i; continue;
                    }
                }
            }
        }
        if (ch == '<' && (g_str_has_prefix(s + i + 1, "http://") || g_str_has_prefix(s + i + 1, "https://"))) {
            const char *e = memchr(s + i, '>', (size_t)(n - i));
            if (e) {
                MD_FLUSH();
                md_link(c, s + i + 1, e - s - i - 1, s + i + 1, e - s - i - 1, tags, ntags, plain);
                i = e - s + 1; run = i; continue;
            }
        }
        i++;
    }
    MD_FLUSH();
#undef MD_FLUSH
}

/* Visible text of a piece of inline Markdown (for table widths and headings) */
static gchar *md_plain(MdCtx *c, const char *s, gssize n) {
    MdCtx dry = { NULL, c->refs };
    GString *p = g_string_new(NULL);
    md_inline(&dry, s, n, NULL, 0, p);
    return g_string_free(p, FALSE);
}

/* Cells of a table row "| a | b |" (pipes inside `code` do not split) */
static GPtrArray *md_cells(const char *line) {
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    const char *p = line;
    while (*p == ' ') p++;
    if (*p == '|') p++;
    GString *cell = g_string_new(NULL);
    int in_code = 0;
    for (; *p; p++) {
        if (*p == '\\' && p[1] == '|') { g_string_append_c(cell, '|'); p++; continue; }
        if (*p == '`') in_code = !in_code;
        if (*p == '|' && !in_code) { g_ptr_array_add(a, g_strstrip(g_strdup(cell->str))); g_string_truncate(cell, 0); continue; }
        g_string_append_c(cell, *p);
    }
    gchar *rest = g_strstrip(g_strdup(cell->str));
    if (*rest) g_ptr_array_add(a, rest); else g_free(rest);
    g_string_free(cell, TRUE);
    return a;
}

static int md_is_separator_row(const char *line) {
    int dash = 0;
    for (const char *p = line; *p; p++) {
        if (*p == '-') dash = 1;
        else if (*p != '|' && *p != ':' && *p != ' ') return 0;
    }
    return dash;
}

#define MD_TABLE_MAX_WIDTH 100   /* wider tables are shown one row per line */

static void md_table(MdCtx *c, char **lines, int count) {
    GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
    int header = 0;
    for (int i = 0; i < count; i++) {
        if (md_is_separator_row(lines[i])) { if (i == 1) header = 1; continue; }
        g_ptr_array_add(rows, md_cells(lines[i]));
    }
    guint ncols = 0;
    for (guint r = 0; r < rows->len; r++) ncols = MAX(ncols, ((GPtrArray *)rows->pdata[r])->len);
    glong width[16] = { 0 };
    ncols = MIN(ncols, 16);
    glong total = 0;
    /* drop rows that show nothing (only images) */
    for (guint r = 0; r < rows->len; ) {
        GPtrArray *row = rows->pdata[r];
        int visible = 0;
        for (guint k = 0; k < row->len && k < ncols; k++) {
            gchar *pl = md_plain(c, row->pdata[k], -1);
            glong w = g_utf8_strlen(pl, -1);
            if (w > 0) visible = 1;
            width[k] = MAX(width[k], w);
            g_free(pl);
        }
        if (!visible) { g_ptr_array_remove_index(rows, r); if (r == 0) header = 0; continue; }
        r++;
    }
    for (guint k = 0; k < ncols; k++) total += width[k] + 3;
    GtkTextTag *bold = md_tag(c, "bold");
    if (total <= MD_TABLE_MAX_WIDTH) {
        GtkTextTag *tt[2] = { md_tag(c, "table"), bold };
        for (guint r = 0; r < rows->len; r++) {
            GPtrArray *row = rows->pdata[r];
            int nt = (header && r == 0) ? 2 : 1;
            for (guint k = 0; k < ncols; k++) {
                const char *cell = k < row->len ? row->pdata[k] : "";
                gchar *pl = md_plain(c, cell, -1);
                glong pad = width[k] - g_utf8_strlen(pl, -1) + (k + 1 < ncols ? 3 : 0);
                g_free(pl);
                md_inline(c, cell, -1, tt, nt, NULL);
                if (pad > 0) { gchar *sp = g_strnfill((gsize)pad, ' '); md_put(c, sp, -1, tt, 1, NULL); g_free(sp); }
            }
            md_put(c, "\n", 1, tt, 1, NULL);
        }
    } else {
        /* one line per row: first cell bold, the others after it */
        GtkTextTag *rt[2] = { md_tag(c, "row"), bold };
        for (guint r = header ? 1 : 0; r < rows->len; r++) {
            GPtrArray *row = rows->pdata[r];
            md_put(c, "• ", -1, rt, 1, NULL);
            for (guint k = 0; k < row->len; k++) {
                const char *cell = row->pdata[k];
                if (!*cell) continue;
                if (k == 1) md_put(c, " \xe2\x80\x94 ", -1, rt, 1, NULL);          /* " — " */
                else if (k > 1) md_put(c, " \xc2\xb7 ", -1, rt, 1, NULL);         /* " · " */
                md_inline(c, cell, -1, rt, k == 0 ? 2 : 1, NULL);
            }
            md_put(c, "\n", 1, rt, 1, NULL);
        }
    }
    g_ptr_array_unref(rows);
}

/* Show Markdown text in the buffer (replaces its contents) */
static void md_render(GtkTextBuffer *buf, const char *text) {
    md_tags_create(buf);
    gtk_text_buffer_set_text(buf, "", -1);
    MdCtx c = { buf, g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free) };
    gchar **lines = g_strsplit(text, "\n", -1);
    int nlines = (int)g_strv_length(lines);
    /* reference definitions "[label]: url" (the Changelog's version links) */
    GRegex *refdef = g_regex_new("^\\s{0,3}\\[([^\\]]+)\\]:\\s+(\\S+)\\s*$", 0, 0, NULL);
    char *skip = g_malloc0((gsize)nlines + 1);
    for (int i = 0; i < nlines; i++) {
        GMatchInfo *m = NULL;
        g_strchomp(lines[i]);                            /* also drops '\r' */
        if (g_regex_match(refdef, lines[i], 0, &m)) {
            gchar *label = g_match_info_fetch(m, 1), *url = g_match_info_fetch(m, 2);
            gchar *low = g_ascii_strdown(label, -1);
            g_free(label);
            g_hash_table_replace(c.refs, low, url);
            skip[i] = 1;
        }
        g_match_info_free(m);
    }
    g_regex_unref(refdef);

    int in_pre = 0, last_blank = 1, after_heading = 0;
    for (int i = 0; i < nlines; i++) {
        const char *l = lines[i];
        if (skip[i]) continue;
        const char *t = l;
        while (*t == ' ') t++;
        int indent = (int)(t - l);
        if (g_str_has_prefix(t, "```")) { in_pre = !in_pre; continue; }
        if (in_pre) {
            GtkTextTag *pt[1] = { md_tag(&c, "pre") };
            md_put(&c, l, -1, pt, 1, NULL);
            md_put(&c, "\n", 1, pt, 1, NULL);
            last_blank = 0; after_heading = 0;
            continue;
        }
        if (!*t) {
            if (!last_blank && !after_heading) md_put(&c, "\n", 1, NULL, 0, NULL);
            last_blank = 1;
            continue;
        }
        if (*t == '#') {
            int level = 0;
            while (t[level] == '#') level++;
            if (level <= 6 && t[level] == ' ') {
                const char *h = t + level + 1;
                char name[4] = { 'h', (char)('0' + MIN(level, 3)), 0, 0 };
                GtkTextTag *ht[1] = { md_tag(&c, name) };
                gchar *pl = md_plain(&c, h, -1), *slug = md_slug(pl);
                gchar *mark = g_strconcat("md:", slug, NULL);
                GtkTextIter end;
                gtk_text_buffer_get_end_iter(buf, &end);
                if (!gtk_text_buffer_get_mark(buf, mark)) gtk_text_buffer_create_mark(buf, mark, &end, TRUE);
                g_free(mark); g_free(slug); g_free(pl);
                md_inline(&c, h, -1, ht, 1, NULL);
                md_put(&c, "\n", 1, ht, 1, NULL);
                last_blank = 0; after_heading = 1;
                continue;
            }
        }
        after_heading = 0;
        if (*t == '|') {
            int j = i;
            while (j < nlines && !skip[j] && lines[j][strspn(lines[j], " ")] == '|') j++;
            md_table(&c, lines + i, j - i);
            i = j - 1;
            last_blank = 0;
            continue;
        }
        if (g_regex_match_simple("^(-{3,}|\\*{3,}|_{3,})$", t, 0, 0)) {
            md_put(&c, "\xe2\x80\x94\xe2\x80\x94\xe2\x80\x94\n", -1, NULL, 0, NULL);
            last_blank = 0;
            continue;
        }
        if (t[0] == '>') {
            const char *q = t + 1;
            if (*q == ' ') q++;
            GtkTextTag *qt[1] = { md_tag(&c, "quote") };
            md_inline(&c, q, -1, qt, 1, NULL);
            md_put(&c, "\n", 1, qt, 1, NULL);
            last_blank = 0;
            continue;
        }
        int level = MIN(indent / 2 + 1, 3);
        char li[4] = { 'l', 'i', (char)('0' + level), 0 };
        if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
            GtkTextTag *lt[1] = { md_tag(&c, li) };
            md_put(&c, level == 1 ? "\xe2\x80\xa2 " : "\xe2\x97\xa6 ", -1, lt, 1, NULL);   /* • or ◦ */
            md_inline(&c, t + 2, -1, lt, 1, NULL);
            md_put(&c, "\n", 1, lt, 1, NULL);
            last_blank = 0;
            continue;
        }
        size_t digits = strspn(t, "0123456789");
        if (digits > 0 && digits < 4 && t[digits] == '.' && t[digits + 1] == ' ') {
            GtkTextTag *lt[1] = { md_tag(&c, li) };
            md_put(&c, t, (gssize)digits + 2, lt, 1, NULL);
            md_inline(&c, t + digits + 2, -1, lt, 1, NULL);
            md_put(&c, "\n", 1, lt, 1, NULL);
            last_blank = 0;
            continue;
        }
        gchar *pl = md_plain(&c, t, -1);
        int empty = *g_strstrip(pl) == '\0';
        g_free(pl);
        if (empty) continue;                             /* a line of badges / images */
        md_inline(&c, t, -1, NULL, 0, NULL);
        md_put(&c, "\n", 1, NULL, 0, NULL);
        last_blank = 0;
    }
    g_free(skip);
    g_strfreev(lines);
    g_hash_table_unref(c.refs);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buf, &start);
    gtk_text_buffer_place_cursor(buf, &start);
}

/* URL of the link at this position, or NULL */
static const char *help_url_at(GtkTextIter *it) {
    const char *url = NULL;
    GSList *tags = gtk_text_iter_get_tags(it);
    for (GSList *l = tags; l && !url; l = l->next) url = g_object_get_data(G_OBJECT(l->data), "dcc-url");
    g_slist_free(tags);
    return url;
}

/* What a link in the help texts opens: "#anchor" scrolls to the heading,
 * LICENSE / CHANGELOG.md / README.md switch the sub-tab, other relative paths
 * open on GitHub, web links in the browser */
typedef enum { HELP_LINK_ANCHOR, HELP_LINK_TAB, HELP_LINK_WEB } HelpLinkKind;
static HelpLinkKind help_link_target(const char *url, int *tab, gchar **target) {
    *tab = -1; *target = NULL;
    if (url[0] == '#') { *target = g_strdup(url + 1); return HELP_LINK_ANCHOR; }
    if (strstr(url, "://") || g_str_has_prefix(url, "mailto:")) { *target = g_strdup(url); return HELP_LINK_WEB; }
    const char *p = url;
    if (g_str_has_prefix(p, "./")) p += 2;
    for (int i = 0; i < HELP_COUNT; i++) {
        size_t len = strlen(help_files[i]);
        if (strncmp(p, help_files[i], len) == 0 && (p[len] == '\0' || p[len] == '#')) {
            *tab = i;
            if (p[len] == '#') *target = g_strdup(p + len + 1);
            return HELP_LINK_TAB;
        }
    }
    *target = g_strconcat(DCC_REPO_URL "/blob/main/", p, NULL);
    return HELP_LINK_WEB;
}

static void help_scroll_to_anchor(int tab, const char *anchor) {
    if (tab < 0 || tab >= HELP_COUNT || !help.views[tab] || !anchor) return;
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(help.views[tab]));
    gchar *name = g_strconcat("md:", anchor, NULL);
    GtkTextMark *m = gtk_text_buffer_get_mark(b, name);
    g_free(name);
    if (m) gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(help.views[tab]), m, 0.0, TRUE, 0.0, 0.0);
}

static void help_open_url(const char *url, int current_tab) {
    int tab; gchar *target;
    HelpLinkKind kind = help_link_target(url, &tab, &target);
    if (kind == HELP_LINK_ANCHOR) {
        help_scroll_to_anchor(current_tab, target);
    } else if (kind == HELP_LINK_TAB) {
        if (help.notebook) gtk_notebook_set_current_page(GTK_NOTEBOOK(help.notebook), tab);
        help_scroll_to_anchor(tab, target);
    } else {
        GError *error = NULL;
        if (!g_app_info_launch_default_for_uri(target, NULL, &error) && error) {
            LOG_WARN("Could not open %s: %s", target, error->message);
            g_error_free(error);
        }
    }
    g_free(target);
}

static gboolean on_help_view_release(GtkWidget *view, GdkEventButton *ev, gpointer data) {
    if (ev->button != 1) return FALSE;
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter s, e;
    if (gtk_text_buffer_get_selection_bounds(b, &s, &e)) return FALSE;   /* selecting text */
    gint x, y;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(view), GTK_TEXT_WINDOW_WIDGET, (gint)ev->x, (gint)ev->y, &x, &y);
    GtkTextIter it;
    if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(view), &it, x, y)) return FALSE;
    const char *url = help_url_at(&it);
    if (url) {
        gchar *copy = g_strdup(url);   /* switching tabs must not free it under us */
        help_open_url(copy, GPOINTER_TO_INT(data));
        g_free(copy);
    }
    return FALSE;
}

static gboolean on_help_view_motion(GtkWidget *view, GdkEventMotion *ev, gpointer data) {
    (void)data;
    gint x, y;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(view), GTK_TEXT_WINDOW_WIDGET, (gint)ev->x, (gint)ev->y, &x, &y);
    GtkTextIter it;
    int on_link = gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(view), &it, x, y) && help_url_at(&it);
    GdkWindow *w = gtk_text_view_get_window(GTK_TEXT_VIEW(view), GTK_TEXT_WINDOW_TEXT);
    GdkCursor *cur = gdk_cursor_new_from_name(gdk_window_get_display(w), on_link ? "pointer" : "text");
    gdk_window_set_cursor(w, cur);
    if (cur) g_object_unref(cur);
    return FALSE;
}

/* Fill the three views (once, when the Help tab is first shown) */
static void help_load(void) {
    if (help.loaded) return;
    help.loaded = 1;
    for (int i = 0; i < HELP_COUNT; i++) {
        if (!help.views[i]) continue;
        GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(help.views[i]));
        gchar *path = g_build_filename(DCC_DOC_DIR, help_files[i], NULL);
        gchar *text = NULL;
        if (!g_file_get_contents(path, &text, NULL, NULL)) {
            gchar *msg = g_strdup_printf(_("File not found: %s"), path);
            gtk_text_buffer_set_text(b, msg, -1);
            g_free(msg);
        } else {
            if (!g_utf8_validate(text, -1, NULL)) { gchar *v = g_utf8_make_valid(text, -1); g_free(text); text = v; }
            if (i == HELP_LICENSE) {
                for (char *p = text; *p; p++) if (*p == '\f') *p = ' ';
                md_tags_create(b);
                GtkTextIter it;
                gtk_text_buffer_set_text(b, "", -1);
                gtk_text_buffer_get_start_iter(b, &it);
                gtk_text_buffer_insert_with_tags_by_name(b, &it, text, -1, "plain", NULL);
                gtk_text_buffer_get_start_iter(b, &it);
                gtk_text_buffer_place_cursor(b, &it);
            } else {
                md_render(b, text);
            }
        }
        g_free(text);
        g_free(path);
    }
}

static void on_help_page_map(GtkWidget *w, gpointer data) { (void)w; (void)data; help_load(); }

/* Search in the shown sub-tab: forward from the selection (or the start),
 * backward with Shift+Enter; wraps around */
static void help_search(int backward, int from_start) {
    if (!help.notebook || !help.search) return;
    int tab = gtk_notebook_get_current_page(GTK_NOTEBOOK(help.notebook));
    if (tab < 0 || tab >= HELP_COUNT) return;
    GtkTextView *view = GTK_TEXT_VIEW(help.views[tab]);
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    const char *needle = gtk_entry_get_text(GTK_ENTRY(help.search));
    gtk_label_set_text(GTK_LABEL(help.search_status), "");
    if (!*needle) return;
    GtkTextIter s, e, ms, me, from;
    int has_sel = gtk_text_buffer_get_selection_bounds(b, &s, &e);
    if (from_start || !has_sel) gtk_text_buffer_get_start_iter(b, &from);
    else from = backward ? s : e;
    GtkTextSearchFlags fl = GTK_TEXT_SEARCH_CASE_INSENSITIVE;
    gboolean found = backward ? gtk_text_iter_backward_search(&from, needle, fl, &ms, &me, NULL)
                              : gtk_text_iter_forward_search(&from, needle, fl, &ms, &me, NULL);
    if (!found) {                                         /* wrap around */
        if (backward) gtk_text_buffer_get_end_iter(b, &from); else gtk_text_buffer_get_start_iter(b, &from);
        found = backward ? gtk_text_iter_backward_search(&from, needle, fl, &ms, &me, NULL)
                         : gtk_text_iter_forward_search(&from, needle, fl, &ms, &me, NULL);
    }
    if (!found) { gtk_label_set_text(GTK_LABEL(help.search_status), _("Not found")); return; }
    gtk_text_buffer_select_range(b, &ms, &me);
    GtkTextMark *mk = gtk_text_buffer_get_mark(b, "dcc-search");
    if (!mk) mk = gtk_text_buffer_create_mark(b, "dcc-search", &ms, TRUE);
    else gtk_text_buffer_move_mark(b, mk, &ms);
    gtk_text_view_scroll_to_mark(view, mk, 0.0, TRUE, 0.0, 0.3);
}

static void on_help_search_changed(GtkSearchEntry *e, gpointer d) { (void)e; (void)d; help_search(0, 1); }
static void on_help_search_next(GtkWidget *e, gpointer d) { (void)e; (void)d; help_search(0, 0); }
static void on_help_search_prev(GtkWidget *e, gpointer d) { (void)e; (void)d; help_search(1, 0); }
/* GtkEntry takes Shift+Enter as a plain Enter ("activate"); GtkSearchEntry
 * only binds Ctrl+Shift+G to "previous-match". Shift+Enter goes back here. */
static gboolean on_help_search_key(GtkWidget *e, GdkEventKey *ev, gpointer d) {
    (void)e; (void)d;
    if ((ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter || ev->keyval == GDK_KEY_ISO_Enter)
        && (ev->state & GDK_SHIFT_MASK)) {
        help_search(1, 0);
        return TRUE;
    }
    return FALSE;
}
static void on_help_tab_switched(GtkNotebook *nb, GtkWidget *page, guint num, gpointer d) {
    (void)nb; (void)page; (void)num; (void)d;
    if (help.search_status) gtk_label_set_text(GTK_LABEL(help.search_status), "");
}

/* System report (1.45.0): what was detected, shown for checking before it
 * is copied into a GitHub issue. Not modal and without a nested main loop. */
static GtkWidget *report_dialog;

static void on_report_response(GtkDialog *d, gint response, gpointer user_data) {
    GtkWidget *note = GTK_WIDGET(user_data);
    if (response == 1 || response == 2) {
        GtkTextBuffer *tb = g_object_get_data(G_OBJECT(d), "report-buffer");
        GtkTextIter a, e;
        gtk_text_buffer_get_bounds(tb, &a, &e);
        gchar *text = gtk_text_buffer_get_text(tb, &a, &e, FALSE);
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
        g_free(text);
        gtk_label_set_text(GTK_LABEL(note), response == 2
            ? _("Copied. Paste the report into the new issue that opened in your browser.")
            : _("Copied to the clipboard."));
        if (response == 2) {
            GError *error = NULL;
            if (!g_app_info_launch_default_for_uri(DCC_REPO_URL "/issues/new?template=hardware-report.md", NULL, &error) && error) {
                g_printerr("[GUI] Failed to open URI: %s\n", error->message);
                g_error_free(error);
            }
        }
        return;
    }
    gtk_widget_destroy(GTK_WIDGET(d));
    report_dialog = NULL;
}

static void report_dialog_open(GtkWidget *parent) {
    if (report_dialog) {
        gtk_window_present(GTK_WINDOW(report_dialog));
        return;
    }
    char *text = dcc_report_build(APP_VERSION);
    GtkWindow *top = parent ? GTK_WINDOW(gtk_widget_get_toplevel(parent)) : NULL;
    report_dialog = gtk_dialog_new_with_buttons(_("System Report"), top, GTK_DIALOG_DESTROY_WITH_PARENT,
                                                _("Copy"), 1, _("Copy and Open GitHub Issues"), 2,
                                                _("Close"), GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(report_dialog), 760, 560);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(report_dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);
    gtk_box_set_spacing(GTK_BOX(content), 8);
    GtkWidget *intro = gtk_label_new(_("What Dudes Control Center detected on this computer, for a bug report from a "
                                       "laptop it was not tested on. It contains no user names, host names, serial "
                                       "numbers or home folder paths. Please read it before you share it."));
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_xalign(GTK_LABEL(intro), 0.0);
    gtk_box_pack_start(GTK_BOX(content), intro, FALSE, FALSE, 0);
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 380);
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(tb, text ? text : _("The report could not be made (out of memory)."), -1);
    free(text);
    g_object_set_data(G_OBJECT(report_dialog), "report-buffer", tb);
    gtk_container_add(GTK_CONTAINER(scrolled), view);
    gtk_box_pack_start(GTK_BOX(content), scrolled, TRUE, TRUE, 0);
    GtkWidget *note = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(note), 0.0);
    gtk_box_pack_start(GTK_BOX(content), note, FALSE, FALSE, 0);
    g_signal_connect(report_dialog, "response", G_CALLBACK(on_report_response), note);
    gtk_widget_show_all(report_dialog);
}

static void on_report_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    report_dialog_open(GTK_WIDGET(btn));
}

static GtkWidget *create_help_page(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 5);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    help.search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(help.search), _("Search"));
    gtk_widget_set_tooltip_text(help.search, _("Enter: next match, Shift+Enter: previous match"));
    gtk_entry_set_width_chars(GTK_ENTRY(help.search), 28);
    help.search_status = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(bar), help.search, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), help.search_status, FALSE, FALSE, 0);
    GtkWidget *report_btn = gtk_button_new_with_label(_("System Report…"));
    gtk_widget_set_tooltip_text(report_btn, _("What was detected on this computer, to copy into a GitHub issue "
                                              "(no user names, host names, serial numbers or home folder paths)"));
    g_signal_connect(report_btn, "clicked", G_CALLBACK(on_report_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(bar), report_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    g_signal_connect(help.search, "search-changed", G_CALLBACK(on_help_search_changed), NULL);
    g_signal_connect(help.search, "activate", G_CALLBACK(on_help_search_next), NULL);
    g_signal_connect(help.search, "next-match", G_CALLBACK(on_help_search_next), NULL);
    g_signal_connect(help.search, "previous-match", G_CALLBACK(on_help_search_prev), NULL);
    g_signal_connect(help.search, "key-press-event", G_CALLBACK(on_help_search_key), NULL);

    help.notebook = gtk_notebook_new();
    const char *titles[HELP_COUNT] = { N_("README"), N_("Changelog"), N_("License") };
    for (int i = 0; i < HELP_COUNT; i++) {
        GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 150);
        GtkWidget *view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 10);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 10);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 6);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 6);
        gtk_widget_add_events(view, GDK_POINTER_MOTION_MASK | GDK_BUTTON_RELEASE_MASK);
        g_signal_connect(view, "button-release-event", G_CALLBACK(on_help_view_release), GINT_TO_POINTER(i));
        g_signal_connect(view, "motion-notify-event", G_CALLBACK(on_help_view_motion), NULL);
        help.views[i] = view;
        gtk_container_add(GTK_CONTAINER(scrolled), view);
        gtk_notebook_append_page(GTK_NOTEBOOK(help.notebook), scrolled, gtk_label_new(_(titles[i])));
    }
    g_signal_connect(help.notebook, "switch-page", G_CALLBACK(on_help_tab_switched), NULL);
    gtk_box_pack_start(GTK_BOX(box), help.notebook, TRUE, TRUE, 0);
    g_signal_connect(box, "map", G_CALLBACK(on_help_page_map), NULL);
    return box;
}

#ifndef DCC_LOGO_DIR
#define DCC_LOGO_DIR "/usr/share/dudes-control-center"
#endif

/* About logo (1.34.4): logo_dark.png / logo_light.png for the window's
 * appearance, swapped when it changes (appearance_refresh) */
static GtkWidget *about_logo_image;
static int about_logo_dark = -1;

static GdkPixbuf *about_logo_load(int dark) {
    gchar *paths[3] = {
        g_build_filename(DCC_LOGO_DIR, dark ? "logo_dark.png" : "logo_light.png", NULL),
        g_build_filename(DCC_LOGO_DIR, "logo.png", NULL),                /* before 1.34.4 */
        g_strdup("/usr/share/pixmaps/DudesControlCenter.png") };
    GdkPixbuf *pb = NULL;
    for (int i = 0; i < 3 && !pb; i++) pb = gdk_pixbuf_new_from_file_at_scale(paths[i], 230, 230, TRUE, NULL);
    for (int i = 0; i < 3; i++) g_free(paths[i]);
    return pb;
}

static void about_logo_set(int dark) {
    if (dark == about_logo_dark && about_logo_image && gtk_image_get_pixbuf(GTK_IMAGE(about_logo_image))) return;
    about_logo_dark = dark;
    if (!about_logo_image) return;
    GdkPixbuf *pb = about_logo_load(dark);
    gtk_image_set_from_pixbuf(GTK_IMAGE(about_logo_image), pb);
    if (pb) g_object_unref(pb);
}

static GtkWidget *create_about_page(void) {
    /* Layout (1.29.1): name and version on top, the logo below it in the
     * middle with the credits left and right of it (each column centered on
     * the logo); below the logo the AI note, the contact and the license */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    /* Application name and version on top, over the logo */
    GtkWidget *title = gtk_label_new(NULL);
    {
        char t[256];
        snprintf(t, sizeof(t), _("<span size='xx-large' weight='bold'>Dudes Control Center</span>\n"
                               "<span size='large' foreground='#7f8c8d'>Version %s</span>"), APP_VERSION);
        gtk_label_set_markup(GTK_LABEL(title), t);
        gtk_label_set_justify(GTK_LABEL(title), GTK_JUSTIFY_CENTER);   /* version under the name */
    }
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 2);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_box_set_homogeneous(GTK_BOX(top), TRUE);   /* left, logo, right: equal widths */

    /* Left: Claude and the code this project is built on */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_valign(left, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(left), about_credit(
        _("Coded with <b>Claude</b> (Claude Code) by <b>Anthropic</b>"), "https://www.anthropic.com/", NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), about_credit(
        _("Fan control code based on <b>fan-control</b> by steiiin, a fork of <b>clevo-fan-control</b> by agramian (GPL-2.0)"),
        "https://github.com/steiiin/fan-control", "https://github.com/agramian/clevo-fan-control"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), about_credit(
        _("Keyboard backlight driver <b>ite_829x</b> by <b>TUXEDO Computers</b> (tuxedo-drivers, GPL-2.0-or-later)"),
        "https://github.com/tuxedocomputers/tuxedo-drivers", NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), left, TRUE, TRUE, 0);

    /* Middle: the logo */
    GtkWidget *logo = gtk_image_new();
    about_logo_image = logo;
    about_logo_set(about_logo_dark > 0);
    gtk_widget_set_valign(logo, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), logo, TRUE, TRUE, 0);

    /* Right: Bionic AI Agent, Linux, its community and the hardware */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(right), about_credit(
        _("Special thanks to the creators of <b>Bionic AI Agent</b>"), "https://lmstudio.ai/", NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), about_credit(
        _("Honoring <b>Linus Torvalds</b>, the visionary founder of Linux"), "https://torvalds-family.blogspot.com/", NULL),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), about_credit(
        _("Appreciation for the <b>Great Linux Community</b>"), NULL, NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), about_credit(
        _("Thanks to <b>Clevo</b> for producing &amp; selling awesome Laptops"), "https://www.clevo.com.tw/", NULL),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), right, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), top, FALSE, FALSE, 0);

    /* Below the logo: the AI note, contact */

    GtkWidget *ai_text = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(ai_text), _("<span weight='bold'>This application was completely coded by AI</span>"));
    gtk_widget_set_halign(ai_text, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), ai_text, FALSE, FALSE, 0);

    GtkWidget *contact = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(contact, GTK_ALIGN_CENTER);
    GtkWidget *contact_text = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(contact_text), _("<b>Contact:</b> <b>Dudemeister</b>  GitHub:"));
    gtk_box_pack_start(GTK_BOX(contact), contact_text, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(contact), create_clickable_link("https://github.com/dudewayne/Dudes-Control-Center"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), contact, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    GtkWidget *license_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(license_label),
        _("<span size='small'>Licensed under the GNU General Public License version 2. "
        "The keyboard driver is GPL-2.0-or-later (TUXEDO Computers).</span>"));
    gtk_widget_set_halign(license_label, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), license_label, FALSE, FALSE, 0);

    return vbox;
}
