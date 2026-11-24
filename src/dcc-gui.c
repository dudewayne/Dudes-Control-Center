// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dudes Control Center - GUI Application
 * GTK-based fan control interface
 */

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gtk/deprecated/gtkstatusicon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <stdbool.h>
#include <cjson/cJSON.h>
#include <poll.h>
#include <locale.h>
#include <libintl.h>

/* UI languages (1.31.0): gettext catalogs dudes-control-center.mo */
#define DCC_TEXT_DOMAIN "dudes-control-center"
#ifndef DCC_LOCALE_DIR
#define DCC_LOCALE_DIR "/usr/share/locale"
#endif
#define _(s) gettext(s)
#define N_(s) (s)

typedef struct { const char *code, *label; } UiLanguage;
static const UiLanguage ui_languages[] = {
    { "system", N_("System default") },
    { "en",     "English (US)" },
    { "en_GB",  "English (UK)" },
    { "de",     "Deutsch" },
    { "es",     "Espa\u00f1ol" },
    { "ja",     "\u65e5\u672c\u8a9e" },
    { "ko",     "\ud55c\uad6d\uc5b4" },
};
#define UI_NUM_LANGUAGES ((int)(sizeof ui_languages / sizeof ui_languages[0]))
static char ui_language_code[16] = "system";   /* as chosen (and running) */
static char **gui_argv;   /* for "Restart now" in Settings */

/* Define M_PI if not already defined */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 (M_PI / 2.0)
#endif

#include "dcc-log.h"
#include "dcc-common.h"
#include "dcc-cpu-common.h"
#include "dcc-kbd-common.h"
#include <cairo.h>
#include <cairo-gobject.h>

/* --- Keyboard Visual Zone Editor --- */
typedef struct {
    double x, y, w, h;
} KeyPos;

/* Keyboard layouts (1.28.0): key names and positions come from
 * <layout dir>/<code>/keys.csv, the picture from keyboard_dark.svg there */
#define KBD_MAX_KEYS 160
#ifndef DCC_LAYOUT_DIR
#define DCC_LAYOUT_DIR "/usr/share/dudes-control-center/layouts"
#endif

/* Keyboard profile being edited in the zone editor (1.24.0): a base color
 * for keys in no zone plus named zones, each with a color and its LEDs.
 * Same content as a profile in the keyboard daemon. */
#define KBD_GUI_MAX_ZONES 32
typedef struct {
    char name[64];
    unsigned char rgb[3];
    int leds[120];
    int n;
} KbdZoneEdit;
typedef struct {
    int has_base;
    unsigned char base[3];
    int num_zones;
    KbdZoneEdit zones[KBD_GUI_MAX_ZONES];
} KbdProfileEdit;

/* Default LED wizard mapping: per LED an index into kbd_legacy_key_names
 * (-1 = the LED lights no key), used until the LED wizard has saved a
 * mapping for the layout. The key names are turned into keys of the
 * current layout (German: by name; other layouts: by position). Keys with
 * several LEDs (Space, Backspace, Shift...) are covered. Taken from a checked wizard run on this keyboard (1.22.0); the
 * old table (one LED per key) had the bottom row, the Numpad 0/, LEDs and
 * ß/<> wrong. LED IDs use the name numbering (rgb:kbd_backlight_N = N). */
static const int kbd_default_led_position[120] = {
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,
     12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,
     24,  25,  26,  27,  28,  29,  30,  31,  -1,  33,  34,  34,
     36,  37,  38,  39,  40,  40,  42,  43,  44,  45,  46,  47,
     48,  49,  50,  51,  52,  53,  54,  -1,  56,  57,  58,  59,
     61,  61,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,
     73,  -1,  74,  54,  75,  76,  77,  59,  78,  -1,  80,  81,
     82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  91,  93,
     94,  95,  96,  97,  99,  99, 100, 101, 102, 103, 103,  -1,
    103, 103, 104, 105, 106, 106, 107, 108, 109, 110, 111,  97
};

/* Key table of versions before 1.28.0 (German, compiled in; keys with
 * several LEDs had several entries). Only used to read the built-in
 * mapping above and to convert old keyboard_led_mapping.csv files. */
#define KBD_LEGACY_NUM_KEYS 113
static const char *const kbd_legacy_key_names[KBD_LEGACY_NUM_KEYS] = {
    "Esc", "F1", "F2", "F3", "F4", "F5", "F6", "F7",
    "F8", "F9", "F10", "F11", "F12", "Druck S-Abf", "Einfg Rollen↓", "Entf",
    "Pos1", "Ende", "Bild↑ Pause", "Bild↓ Untbr",
    "^", "1", "2", "3", "4", "5", "6", "7",
    "8", "9", "0", "ß", "ß", "´", "Backspace", "Backspace",
    "Numpad Num Lock", "Numpad ÷", "Numpad ×", "Numpad −",
    "Tab", "Tab", "Q", "W", "E", "R", "T", "Z",
    "U", "I", "O", "P", "Ü", "+", "Enter", "Enter",
    "Numpad 7", "Numpad 8", "Numpad 9", "Numpad +", "Numpad +",
    "Caps Lock", "Caps Lock", "A", "S", "D", "F", "G", "H",
    "J", "K", "L", "Ö", "Ä", "#", "Numpad 4", "Numpad 5", "Numpad 6",
    "Shift left", "Shift left", "<", "Y", "X", "C", "V", "B",
    "N", "M", ",", ".", "-", "Shift right", "Shift right",
    "Arrow up", "Numpad 1", "Numpad 2", "Numpad 3", "Numpad Enter", "Numpad Enter",
    "Strg left", "Fn", "Win", "Alt", "Space", "AltGr", "AI (Copilot)", "Strg right",
    "Arrow left", "Arrow down", "Arrow right", "Numpad 0", "Numpad ,", "Numpad Enter"
};

/* Keys of the current layout (loaded from keys.csv) */
static size_t kbd_num_keys;
static char kbd_key_name_buf[KBD_MAX_KEYS][48];
static const char *kbd_key_names[KBD_MAX_KEYS];
static KeyPos kbd_key_positions[KBD_MAX_KEYS];
static char kbd_layout_code[8] = "de";

typedef struct { const char *code, *label, *xkb; } KbdLayoutInfo;
static const KbdLayoutInfo kbd_layouts[] = {
    { "de", N_("German (DE)"),   "de,at,ch" },
    { "uk", N_("English (UK)"),  "gb,uk,ie" },
    { "es", N_("Spanish (ES)"),  "es" },
    { "jp", N_("Japanese (JP)"), "jp" },
    { "kr", N_("Korean (KR)"),   "kr" },
};
#define KBD_NUM_LAYOUTS ((int)(sizeof kbd_layouts / sizeof kbd_layouts[0]))

static int kbd_layout_known(const char *code) {
    for (int i = 0; i < KBD_NUM_LAYOUTS; i++) if (code && !strcmp(kbd_layouts[i].code, code)) return 1;
    return 0;
}

/* One line of keys.csv: ';' separated, "quoted" fields with "" for a quote */
static int kbd_csv_split(const char *line, char fields[][96], int max) {
    int n = 0;
    const char *q = line;
    while (n < max) {
        size_t len = 0;
        if (*q == '"') {
            q++;
            while (*q) {
                if (*q == '"' && q[1] == '"') { if (len < 95) fields[n][len++] = '"'; q += 2; }
                else if (*q == '"') { q++; break; }
                else { if (len < 95) fields[n][len++] = *q; q++; }
            }
            while (*q && *q != ';') q++;
        } else {
            while (*q && *q != ';' && *q != '\r' && *q != '\n') { if (len < 95) fields[n][len++] = *q; q++; }
        }
        fields[n++][len] = '\0';
        if (*q != ';') break;
        q++;
    }
    return n;
}

typedef struct {
    size_t n;
    char names[KBD_MAX_KEYS][48];
    KeyPos pos[KBD_MAX_KEYS];
    int row[KBD_MAX_KEYS];             /* keyboard row (keys.csv "row"), -1 if unknown */
    double right;                      /* right edge of the keys */
} KbdLayoutData;

/* Read <layout dir>/<code>/keys.csv (columns found by their header names) */
static int kbd_layout_parse(const char *code, KbdLayoutData *L) {
    memset(L, 0, sizeof *L);
    if (!kbd_layout_known(code)) return -1;
    char path[512];
    snprintf(path, sizeof path, "%s/%s/keys.csv", DCC_LAYOUT_DIR, code);
    gchar *data = NULL;
    if (!g_file_get_contents(path, &data, NULL, NULL)) {
        g_print("[GUI] Keyboard layout '%s': cannot read %s\n", code, path);
        return -1;
    }
    char *text = data;
    if (!strncmp(text, "\xEF\xBB\xBF", 3)) text += 3;          /* UTF-8 BOM */
    int ck = -1, cx = -1, cy = -1, cw = -1, ch = -1, cr = -1, header = 1;
    static char f[24][96];
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        int nf = kbd_csv_split(line, f, 24);
        if (header) {
            for (int i = 0; i < nf; i++) {
                if (!strcmp(f[i], "key")) ck = i;
                else if (!strcmp(f[i], "x_px")) cx = i;
                else if (!strcmp(f[i], "y_px")) cy = i;
                else if (!strcmp(f[i], "width_px")) cw = i;
                else if (!strcmp(f[i], "height_px")) ch = i;
                else if (!strcmp(f[i], "row")) cr = i;
            }
            header = 0;
            if (ck < 0 || cx < 0 || cy < 0 || cw < 0 || ch < 0) break;
            continue;
        }
        int need = MAX(MAX(ck, cx), MAX(MAX(cy, cw), ch));
        if (nf <= need || !f[ck][0] || L->n >= KBD_MAX_KEYS) continue;
        KeyPos k = { atof(f[cx]), atof(f[cy]), atof(f[cw]), atof(f[ch]) };
        if (k.w <= 0 || k.h <= 0) continue;
        snprintf(L->names[L->n], sizeof L->names[0], "%s", f[ck]);
        L->row[L->n] = (cr >= 0 && cr < nf && f[cr][0]) ? atoi(f[cr]) : -1;
        L->pos[L->n++] = k;
        if (k.x + k.w > L->right) L->right = k.x + k.w;
    }
    g_free(data);
    if (L->n == 0) g_print("[GUI] Keyboard layout '%s': no keys in %s\n", code, path);
    return L->n > 0 ? 0 : -1;
}

static KbdLayoutData kbd_cur_layout;   /* the current layout as read */

/* Make code the current layout; the old one stays if it cannot be read */
static int kbd_layout_load(const char *code) {
    static KbdLayoutData L;
    if (kbd_layout_parse(code, &L) != 0) return -1;
    kbd_cur_layout = L;
    kbd_num_keys = L.n;
    for (size_t i = 0; i < L.n; i++) {
        memcpy(kbd_key_name_buf[i], L.names[i], sizeof kbd_key_name_buf[i]);
        kbd_key_names[i] = kbd_key_name_buf[i];
        kbd_key_positions[i] = L.pos[i];
    }
    snprintf(kbd_layout_code, sizeof kbd_layout_code, "%s", code);
    g_print("[GUI] Keyboard layout '%s': %zu keys\n", code, L.n);
    return 0;
}

static int kbd_layout_index_of(const char *name) {
    for (size_t i = 0; i < kbd_num_keys; i++) if (!strcmp(kbd_key_names[i], name)) return (int)i;
    return -1;
}

/* German keys -> keys of another layout (1.28.0), in two steps:
 * 1. keys with the same name (Strg = Ctrl) in the same or a neighbouring
 *    row at nearly the same place (within 10% of the keyboard width): the
 *    function, navigation and modifier keys, also when a layout moves them
 *    a little (Japanese arrow keys sit one row lower);
 * 2. the rest row by row: the keys are lined up by their position within
 *    their full row and matched in order, each key at most once (an
 *    alignment with a cost for keys left out).
 * Symbol keys are not matched by name: the same legend sits at different
 * places in different layouts, and the LED belongs to the place. Keys that
 * only one layout has stay without LED (e.g. Japanese Henkan, Korean '\'
 * above Enter): they need the LED Wizard. Scaling positions alone gave two
 * German keys the same narrower Japanese key and left '[' or '/' unlit. */
#define KBD_MATCH_SKIP 0.06
#define KBD_MATCH_NAME_DIST 0.10

/* Position of every key within its full row (0 = left end, 1 = right end) */
static void kbd_row_positions(const KbdLayoutData *L, double *u) {
    for (size_t i = 0; i < L->n; i++) {
        double left = 1e9, right = -1e9;
        for (size_t j = 0; j < L->n; j++) {
            if (L->row[j] != L->row[i]) continue;
            left = MIN(left, L->pos[j].x);
            right = MAX(right, L->pos[j].x + L->pos[j].w);
        }
        double span = right - left > 1 ? right - left : 1;
        u[i] = (L->pos[i].x + L->pos[i].w / 2.0 - left) / span;
    }
}

static void kbd_match_row(const int *ai, const double *ua, int m, const int *bi, const double *ub, int n, int *out) {
    static double D[KBD_MAX_KEYS + 1][KBD_MAX_KEYS + 1];
    static char W[KBD_MAX_KEYS + 1][KBD_MAX_KEYS + 1];   /* 0 match, 1 skip A, 2 skip B */
    for (int i = 0; i <= m; i++)
        for (int j = 0; j <= n; j++) {
            if (!i && !j) { D[0][0] = 0; continue; }
            double best = 1e18;
            char w = 0;
            if (i && j) { best = D[i - 1][j - 1] + ABS(ua[ai[i - 1]] - ub[bi[j - 1]]); w = 0; }
            if (i && D[i - 1][j] + KBD_MATCH_SKIP < best) { best = D[i - 1][j] + KBD_MATCH_SKIP; w = 1; }
            if (j && D[i][j - 1] + KBD_MATCH_SKIP < best) { best = D[i][j - 1] + KBD_MATCH_SKIP; w = 2; }
            D[i][j] = best;
            W[i][j] = w;
        }
    for (int i = m, j = n; i > 0 || j > 0;) {
        if (i && j && W[i][j] == 0) { out[ai[i - 1]] = bi[j - 1]; i--; j--; }
        else if (i && (W[i][j] == 1 || !j)) i--;
        else j--;
    }
}

/* Keys of one row that are still free, sorted by x */
static int kbd_row_keys(const KbdLayoutData *L, int row, const char *taken, int *out) {
    int n = 0;
    for (size_t i = 0; i < L->n; i++) if (L->row[i] == row && !taken[i]) out[n++] = (int)i;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && L->pos[out[b]].x < L->pos[out[b - 1]].x; b--) {
            int t = out[b]; out[b] = out[b - 1]; out[b - 1] = t;
        }
    return n;
}

static void kbd_match_layouts(const KbdLayoutData *A, const KbdLayoutData *B, int *out) {
    static double ua[KBD_MAX_KEYS], ub[KBD_MAX_KEYS];
    char used_a[KBD_MAX_KEYS] = { 0 }, used_b[KBD_MAX_KEYS] = { 0 };
    for (int i = 0; i < KBD_MAX_KEYS; i++) out[i] = -1;
    kbd_row_positions(A, ua);
    kbd_row_positions(B, ub);
    /* 1. same name, nearly the same place */
    for (size_t i = 0; i < A->n; i++) {
        const char *nm = A->names[i];
        if (!strcmp(nm, "Strg left")) nm = "Ctrl left";
        else if (!strcmp(nm, "Strg right")) nm = "Ctrl right";
        double xa = (A->pos[i].x + A->pos[i].w / 2.0) / (A->right > 0 ? A->right : 1);
        for (size_t j = 0; j < B->n; j++) {
            if (used_b[j] || strcmp(B->names[j], nm) || ABS(A->row[i] - B->row[j]) > 1) continue;
            double xb = (B->pos[j].x + B->pos[j].w / 2.0) / (B->right > 0 ? B->right : 1);
            if (ABS(xa - xb) >= KBD_MATCH_NAME_DIST) continue;
            out[i] = (int)j;
            used_a[i] = used_b[j] = 1;
            break;
        }
    }
    /* 2. the rest row by row */
    for (int row = 0; row < 16; row++) {
        int ai[KBD_MAX_KEYS], bi[KBD_MAX_KEYS];
        int m = kbd_row_keys(A, row, used_a, ai), n = kbd_row_keys(B, row, used_b, bi);
        if (m && n) kbd_match_row(ai, ua, m, bi, ub, n, out);
    }
}

/* An old (German table) key -> key of the current layout: German by name,
 * the others through kbd_match_layouts. -1 if there is none. */
static int kbd_legacy_to_current(int legacy) {
    if (legacy < 0 || legacy >= KBD_LEGACY_NUM_KEYS) return -1;
    const char *name = kbd_legacy_key_names[legacy];
    if (!strcmp(kbd_layout_code, "de")) return kbd_layout_index_of(name);
    static KbdLayoutData de;
    static int de_loaded = 0;
    static int de_to_cur[KBD_MAX_KEYS];
    static char matched_for[8] = "";
    if (!de_loaded) de_loaded = kbd_layout_parse("de", &de) == 0 ? 1 : -1;
    if (de_loaded != 1) return -1;
    if (strcmp(matched_for, kbd_layout_code) != 0) {
        kbd_match_layouts(&de, &kbd_cur_layout, de_to_cur);
        snprintf(matched_for, sizeof matched_for, "%s", kbd_layout_code);
    }
    for (size_t i = 0; i < de.n; i++)
        if (!strcmp(de.names[i], name)) return de_to_cur[i];
    return -1;
}

/* Socket paths are overridable at build time (used by the test suite) */
/* Components switched off in the Settings tab (1.27.0): flag files
 * <dir>/disabled-fan|cpu|keyboard, written by dcc-component as root */
#ifndef DCC_COMPONENT_DIR
#define DCC_COMPONENT_DIR "/etc/dudes-control-center"
#endif
#define DCC_COMPONENT_HELPER "/usr/local/sbin/dcc-component"
enum { COMP_FAN = 0, COMP_CPU = 1, COMP_KEYBOARD = 2, COMP_COUNT = 3 };
/* Appearance setting (1.29.0) */
enum { APPEARANCE_SYSTEM = 0, APPEARANCE_DARK = 1, APPEARANCE_LIGHT = 2 };
static const char *const appearance_names[3] = { "system", "dark", "light" };
static const char *const dcc_component_names[COMP_COUNT] = { "fan", "cpu", "keyboard" };

static int dcc_component_disabled(int comp) {
    if (comp < 0 || comp >= COMP_COUNT) return 0;
    char path[512];
    snprintf(path, sizeof path, "%s/disabled-%s", DCC_COMPONENT_DIR, dcc_component_names[comp]);
    return access(path, F_OK) == 0;
}

#ifndef SOCKET_PATH
#define SOCKET_PATH "/run/dcc-fan.sock"
#endif
#ifndef CPU_SOCKET_PATH
#define CPU_SOCKET_PATH "/run/dcc-cpu.sock"
#endif
#define BUFFER_SIZE 4096
#define MAX_CURVE_POINTS 50

/* Structure to hold curve point data for sorting */
typedef struct {
    int temp;
    int fan_pct;
} CurvePoint;

/* Comparison function for sorting curve points by temperature (ascending) */
static int compare_curve_points_asc(const void *a, const void *b) {
    const CurvePoint *pa = (const CurvePoint *)a;
    const CurvePoint *pb = (const CurvePoint *)b;
    return pa->temp - pb->temp;  /* Ascending order */
}

/* Helper: close a dialog when Enter is pressed in an entry */
static void on_entry_activate(GtkEntry *entry, GtkDialog *dialog) {
    (void)entry;
    gtk_dialog_response(dialog, GTK_RESPONSE_ACCEPT);
}

/* Helper function to get timestamp string */
static void get_timestamp(char *buf, size_t buf_size) {
    struct timeval tv;
    struct tm *tm_info;
    char time_str[32];
    
    #pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    gettimeofday(&tv, NULL);
    tm_info = localtime(&tv.tv_sec);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
    snprintf(buf, buf_size, "[%s.%03ld] ", time_str, (long)(tv.tv_usec / 1000));
#pragma GCC diagnostic pop
}

/* --- Route all GUI output into GUI.log (Rule R2) ---
 * The GUI historically printed to stdout with g_print(), which is lost when it
 * is started from the desktop menu. These handlers send g_print/g_printerr and
 * GLib/GTK warnings to the log file (and still echo to a terminal if present).
 * Identical messages (e.g. from the periodic status refresh) are written at
 * most once per GUI_LOG_REPEAT_WINDOW_SEC to keep the log readable. */
#define GUI_LOG_REPEAT_WINDOW_SEC 60
#define GUI_LOG_REPEAT_SLOTS      64

static uint64_t gui_log_hash(const char *str) {
    uint64_t h = 1469598103934665603ULL;           /* FNV-1a */
    for (; *str; str++) { h ^= (unsigned char)*str; h *= 1099511628211ULL; }
    return h;
}

/* Returns 1 if this exact message was already logged within the window */
static int gui_log_is_repeat(const char *msg) {
    static uint64_t hashes[GUI_LOG_REPEAT_SLOTS];
    static time_t   stamps[GUI_LOG_REPEAT_SLOTS];
    uint64_t h = gui_log_hash(msg);
    time_t now = time(NULL);
    int oldest = 0;
    for (int i = 0; i < GUI_LOG_REPEAT_SLOTS; i++) {
        if (hashes[i] == h && stamps[i] != 0) {
            if (now - stamps[i] < GUI_LOG_REPEAT_WINDOW_SEC) return 1;
            stamps[i] = now;
            return 0;
        }
        if (stamps[i] < stamps[oldest]) oldest = i;
    }
    hashes[oldest] = h;
    stamps[oldest] = now;
    return 0;
}

static void gui_log_text(LogLevel default_level, const char *text) {
    char *copy = g_strdup(text);
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *msg = line;
        /* Drop the "[HH:MM:SS.mmm] " prefix from get_timestamp(); the log adds its own */
        if (msg[0] == '[' && strlen(msg) > 15 && msg[3] == ':' && msg[6] == ':' && msg[13] == ']') {
            msg += 14;
            while (*msg == ' ') msg++;
        }
        if (strncmp(msg, "[GUI] ", 6) == 0) msg += 6;
        if (*msg == '\0') continue;

        LogLevel level = default_level;
        if (strstr(msg, "ERROR") || strstr(msg, "CRITICAL") || strstr(msg, "Failed")) level = LOG_LEVEL_ERROR;
        else if (strstr(msg, "WARNING") || strstr(msg, "WARN")) level = LOG_LEVEL_WARN;
        else if (strncmp(msg, "DEBUG", 5) == 0 || strstr(msg, " DEBUG:")) level = LOG_LEVEL_DEBUG;

        if (!log_should_output(level)) continue;
        if (level > LOG_LEVEL_WARN && gui_log_is_repeat(msg)) continue;  /* never drop errors */
        log_write(level, "%s", msg);
    }
    g_free(copy);
}

static void gui_print_handler(const gchar *text) {
    gui_log_text(LOG_LEVEL_INFO, text);
    if (isatty(STDOUT_FILENO)) { fputs(text, stdout); fflush(stdout); }
}

static void gui_printerr_handler(const gchar *text) {
    gui_log_text(LOG_LEVEL_WARN, text);
    if (isatty(STDERR_FILENO)) { fputs(text, stderr); fflush(stderr); }
}

static void gui_glib_log_handler(const gchar *domain, GLogLevelFlags flags,
                                 const gchar *message, gpointer user_data) {
    (void)user_data;
    LogLevel level = LOG_LEVEL_INFO;
    if (flags & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL)) level = LOG_LEVEL_ERROR;
    else if (flags & G_LOG_LEVEL_WARNING) level = LOG_LEVEL_WARN;
    else if (flags & G_LOG_LEVEL_DEBUG) level = LOG_LEVEL_DEBUG;
    if (log_should_output(level)) {
        log_write(level, "[%s] %s", domain ? domain : "GLib", message ? message : "");
    }
    g_log_default_handler(domain, flags, message, NULL);  /* keep normal terminal output */
}

static void gui_install_log_handlers(void) {
    g_set_print_handler(gui_print_handler);
    g_set_printerr_handler(gui_printerr_handler);
    g_log_set_default_handler(gui_glib_log_handler, NULL);
}

/* Application data structure */
typedef struct {
    GtkWidget *window;
    GtkWidget *notebook;
    
    /* Page 1: Status & Manual Control widgets */
    GtkWidget *cpu_temp_label;
    GtkWidget *gpu_temp_label;
    GtkWidget *cpu_fan_rpm_label;
    GtkWidget *gpu_fan_rpm_label;
    GtkWidget *fan_pct_label;
    GtkWidget *manual_mode_switch;
    GtkWidget *manual_speed_label; /* Display current manual speed */
    GtkWidget *manual_plus_btn;   /* +10% button */
    GtkWidget *manual_minus_btn;  /* -10% button */
    
    /* Page 2: Curve Editor widgets */
    GtkWidget *profile_combo;
    GtkWidget *fan_activate_btn;   /* Fan Profiles tab: activate the selected profile */
    GtkListStore *curve_store;
    GtkWidget *curve_treeview;
    GtkWidget *curve_graph; /* Drawing area for temperature curve visualization */
    int num_profiles;
    char **profile_names;
    int active_profile_idx;
    
    /* Page 3: Profile List widgets */
    int pending_active_profile_idx; /* Pending selection before Apply */
    
    /* Socket connection */
    int socket_fd;
    int connected;
    
    /* Flag to prevent callback recursion during menu creation */
    int updating_tray_menu;
    
    /* Flag to prevent profile loading during refresh */
    int refreshing_profile_list;
    
    /* Flag to track unsaved curve point changes */
    int curve_points_modified;
    
    /* Flag to prevent re-entry in notebook page switch callback */
    int switching_page;
    
    /* Auto mode state from daemon */
    int auto_mode_enabled;
    
    /* Manual mode current speed (for +/- buttons) */
    int manual_current_speed;
    
    /* Last known fan duty from status refresh */
    int last_known_fan_duty;
    int emergency_auto_seen; /* last EMERGENCY_AUTO count from the fan daemon (-1 = none yet) */
    
    /* Manual mode state tracking */
    int in_manual_mode; /* TRUE if currently in manual mode */
    int profile_before_manual; /* Profile index that was active before entering manual mode */
    
    /* Page 3: Manual mode warning widgets */
    GtkWidget *manual_mode_warning_label;
    GtkWidget *disable_manual_mode_btn;
    
    /* Signal IDs for blocking/unblocking callbacks */
    guint manual_mode_signal_id;
    guint profile_combo_signal_id;
    guint cpu_profile_combo_signal_id;
    guint cpu_custom_profile_combo_signal_id;
    guint cpu_apply_all_cores_signal_id;
    
    /* Statusbar */
    GtkWidget *statusbar;
    guint status_context;
    
    /* Refresh timer */
    guint refresh_timer_id;
    
    /* System tray icon */
    GtkStatusIcon *status_icon;
    GtkWidget *tray_menu;
    
    /* Window position */
    gint window_x, window_y;
    guint window_width, window_height;
    
    /* Handler ID for page-switched signal (for blocking during cancel) */
    guint page_switched_handler_id;
    
    /* Page 4: Log viewer widgets */
    GtkWidget *gui_log_view;
    GtkWidget *daemon_log_view;
    GtkWidget *cpu_daemon_log_view;
    GtkWidget *kbd_daemon_log_view;
    GtkTextBuffer *gui_log_buffer;
    GtkTextBuffer *daemon_log_buffer;
    GtkTextBuffer *cpu_daemon_log_buffer;
    GtkTextBuffer *kbd_daemon_log_buffer;
    guint log_refresh_timer_id;
    
    /* Page 6: CPU Control widgets */
    int cpu_socket_fd;
    int cpu_connected;
    GtkWidget *cpu_page_temp_label;
    GtkWidget *cpu_page_load_label;
    GtkWidget *cpu_page_governor_label;
    GtkWidget *cpu_page_freq_label;
    GtkWidget *cpu_page_num_label;
    GtkWidget *cpu_page_min_max_freq_label;
    GtkWidget *cpu_profile_combo;
    GtkListStore *cpu_profile_store;
    guint cpu_refresh_timer_id;
    
    /* Per-core frequency display */
    GtkWidget *cpu_core_grid;
    GtkWidget *cpu_core_freq_labels[32];
    int cpu_core_count;
    
    /* Per-core frequency profile management UI */
    GtkWidget *cpu_custom_profile_combo;
    GtkListStore *cpu_custom_profile_store;
    GtkWidget *cpu_apply_all_cores_checkbox;
    GtkWidget *cpu_freq_profile_create_btn;
    GtkWidget *cpu_freq_profile_delete_btn;
    GtkWidget *cpu_freq_profile_rename_btn;
    GtkWidget *cpu_freq_profile_save_btn;
    GtkWidget *cpu_freq_profile_activate_btn;
    GtkWidget *cpu_freq_profile_status_label;
    GtkWidget *cpu_core_freq_grid; /* Grid for per-core min/max spinners */
    GtkWidget *cpu_core_max_spinners[32]; /* Spinners for max freq per core */
    GtkWidget *cpu_core_min_spinners[32]; /* Spinners for min freq per core */
    GtkAdjustment *cpu_core_max_adjustments[32];
    GtkAdjustment *cpu_core_min_adjustments[32];
    int cpu_active_custom_profile_idx;   /* profile selected in the CPU tab */
    int cpu_running_profile_idx;         /* profile the daemon marks active ([*]), -1 = none */
    int cpu_freq_profiles_modified; /* Track unsaved changes */
    /* Values of the shown CPU profile as loaded/saved; the unsaved-changes
     * dialog compares the widgets with them (1.23.0) */
    int cpu_snap_valid;
    int cpu_snap_profile;              /* profile index the values belong to */
    int cpu_snap_mode_row;             /* power mode combo row */
    int cpu_snap_all_cores;
    int cpu_snap_cores;
    double cpu_snap_min[MAX_CPUS], cpu_snap_max[MAX_CPUS];

    /* Hard limit configuration UI */
    GtkWidget *cpu_hard_limit_spinner;
    GtkAdjustment *cpu_hard_limit_adjustment;
    GtkWidget *cpu_hard_limit_apply_btn;

    /* Page 7: Keyboard Backlight Control widgets */
    int kbd_socket_fd;
    int kbd_connected;
    GtkWidget *kbd_profile_combo;
    GtkListStore *kbd_profile_store;
    GtkWidget *kbd_profile_create_btn;
    GtkWidget *kbd_profile_delete_btn;
    GtkWidget *kbd_profile_rename_btn;
    GtkWidget *kbd_profile_save_btn;
    GtkWidget *kbd_profile_activate_btn;
    GtkWidget *kbd_profile_status_label;
    GtkWidget *kbd_zone_grid; /* Grid for zone color editors */
    GtkWidget *kbd_keyboard_canvas; /* Drawing area for keyboard schematic */
    cairo_surface_t *kbd_keyboard_image; /* Cached keyboard image */
    int kbd_selected_led_indices[120]; /* LEDs selected for current zone */
    int kbd_num_selected_leds; /* Count of selected LEDs */
    unsigned char kbd_led_rgb[120][3]; /* current color per LED (from GET_ZONES) */
    unsigned char kbd_led_has_color[120]; /* 1 if the LED belongs to a zone */
    GtkWidget *kbd_status_label;
    GtkWidget *fan_daemon_status_label;   /* Fan Profiles tab */
    GtkWidget *cpu_daemon_status_label;   /* CPU Profiles tab */
    guint kbd_refresh_timer_id;
    int kbd_active_profile_idx;
    int kbd_profiles_modified; /* Track unsaved changes */
    /* Status tab: Active Profiles (1.30.0) */
    GtkWidget *active_fan_label, *active_cpu_label, *active_mode_label, *active_kbd_label;
    /* Settings tab (1.27.0) */
    GtkWidget *page_widgets[10];       /* notebook page per PAGE_* */
    GtkWidget *settings_switch[3];
    GtkWidget *settings_state[3];
    gulong settings_switch_sig[3];
    int settings_busy;                 /* a pkexec helper is running */
    GtkWidget *settings_layout_combo;
    /* Appearance (1.29.0) */
    int appearance;                    /* APPEARANCE_SYSTEM / _DARK / _LIGHT */
    int is_dark;                       /* the window is drawn dark right now */
    GtkWidget *settings_appearance_combo;
    GtkWidget *settings_language_combo;   /* UI language (1.31.0) */
    GtkWidget *settings_language_note;
    GtkWidget *settings_restart_btn;
    GtkWidget *settings_appearance_note;
    GtkWidget *settings_layout_note;
    /* Zone editor (1.24.0) */
    KbdProfileEdit kbd_edit;           /* profile being edited (shown as a preview) */
    KbdProfileEdit kbd_saved;          /* the same profile as saved */
    char kbd_edit_name[256];
    int kbd_edit_loaded;
    int kbd_sel_zone;                  /* zone being edited, -1: whole keyboard */
    int kbd_canvas_width_used;         /* canvas width its height was computed for */
    int kbd_drag_active;               /* left button down on the picture (1.25.0) */
    int kbd_drag_moved;                /* moved far enough: a rectangle, not a click */
    double kbd_drag_x0, kbd_drag_y0, kbd_drag_x1, kbd_drag_y1;   /* widget coordinates */
    int kbd_canvas_height_want;
    GtkListStore *kbd_zone_store;
    GtkWidget *kbd_zone_view;
    GtkWidget *kbd_zone_rename_btn;
    GtkWidget *kbd_zone_delete_btn;
    GtkWidget *kbd_mode_label;
    gulong kbd_profile_combo_signal_id;
    gulong kbd_zone_sel_signal_id;

    /* Keyboard zone editor widgets (dynamic zones) */
    GtkWidget *kbd_zone_name_entries[32];
    GtkWidget *kbd_zone_color_buttons[32];
    GtkWidget *kbd_zone_delete_buttons[32];
    GtkWidget *kbd_off_switch; /* Toggle to disable keyboard backlight */
    GtkWidget *kbd_brightness_scale; /* keyboard hardware brightness 0..max */
    guint kbd_brightness_timer;      /* debounce while the slider is dragged */
    int kbd_brightness_max;
    int kbd_off_active; /* 1 when off switch is active */
    int kbd_num_zones;
    
    /* LED Wizard state */
    int kbd_wizard_active; /* 1 when wizard is running */
    int kbd_wizard_current_led; /* Current LED being assigned (0-119) */
    int kbd_wizard_mapping[120]; /* LED ID -> key position index */
    int kbd_wizard_backup[120];  /* mapping before the wizard started (Cancel restores it) */
    GtkWidget *kbd_led_wizard_btn; /* LED Wizard button */
    GtkWidget *kbd_led_wizard_label; /* Label showing current LED */
    GtkWidget *kbd_led_wizard_skip_btn; /* Skip button */
    GtkWidget *kbd_led_wizard_nokey_btn; /* No Key button: the LED lights no key */
    GtkWidget *kbd_led_wizard_save_btn; /* Save & Finish button */
    GtkWidget *kbd_led_wizard_cancel_btn; /* Cancel button */
} AppData;

/* Forward declarations for callbacks */
static void on_apply_clicked(GtkWidget *widget, gpointer user_data);
static void on_create_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_delete_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_rename_profile_clicked(GtkWidget *widget, gpointer user_data);

/* CPU tab note on frequency maximums the CPU does not go below (1.34.2) */
#define CPU_MAX_FLOOR_NOTE N_("A maximum frequency is a ceiling, not a target: the CPU picks its own speed below it. " \
    "Many CPUs do not go below a certain speed once they have work to do, however low the maximum is " \
    "(an Intel Core Ultra 9 275HX keeps about 2.2 GHz on its P-cores and 1.5 GHz on its E-cores). " \
    "Maximums below that change little; higher ones are kept exactly.")

/* Forward declarations for page creation */
/* Notebook pages (keep in sync with create_main_window) */
enum { PAGE_STATUS = 0, PAGE_FAN = 1, PAGE_CPU = 2, PAGE_KEYBOARD = 3, PAGE_SETTINGS = 4, PAGE_LOGS = 5, PAGE_HELP = 6,
       PAGE_ABOUT = 7, PAGE_COUNT = 8 };

static GtkWidget *create_status_page(AppData *app);
static GtkWidget *create_curve_editor_page(AppData *app);
static GtkWidget *create_log_page(AppData *app);
static GtkWidget *create_about_page(void);
static GtkWidget *create_help_page(void);
static GtkWidget *create_cpu_page(AppData *app);
static GtkWidget *create_keyboard_page(AppData *app);

/* Helper function to create clickable link label */
static GtkWidget *create_clickable_link(const char *url);

/* Forward declarations for curve graph */
static gboolean draw_curve_graph(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static void update_curve_graph(AppData *app);
static void on_add_curve_point_clicked(GtkWidget *widget, gpointer user_data);
static void on_delete_curve_point_clicked(GtkWidget *widget, gpointer user_data);
static gboolean on_curve_point_dialog_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static void on_profile_combo_changed(GtkWidget *widget, gpointer user_data);
static void on_window_destroy(GtkWidget *widget, gpointer user_data);

/* Forward declarations for daemon connection */
static void connect_to_daemon(AppData *app);
static void disconnect_from_daemon(AppData *app);

/* Forward declarations for CPU daemon connection */
static void connect_to_cpu_daemon(AppData *app);
static void update_daemon_status_labels(AppData *app);
static void disconnect_from_cpu_daemon(AppData *app);
static const char *send_cpu_command(AppData *app, const char *cmd);
static void parse_cpu_status(AppData *app, const char *response);

/* Forward declarations for keyboard daemon connection */
static void connect_to_keyboard_daemon(AppData *app);
static int kbd_ensure_connected(AppData *app);
static const char *send_keyboard_command(AppData *app, const char *cmd);
static void on_kbd_create_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_kbd_delete_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_kbd_rename_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_kbd_save_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_kbd_activate_profile_clicked(GtkWidget *widget, gpointer user_data);
static gboolean on_kbd_off_switch_toggled(GtkSwitch *sw, gboolean new_state, gpointer user_data);
static void on_preset_color_clicked(GtkWidget *widget, AppData *app);
static gboolean kbd_canvas_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static gboolean kbd_canvas_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static void kbd_redraw_canvas(AppData *app);
static void on_led_wizard_start_clicked(GtkWidget *widget, gpointer user_data);
static void on_led_wizard_skip_clicked(GtkWidget *widget, gpointer user_data);
static void on_led_wizard_nokey_clicked(GtkWidget *widget, gpointer user_data);
static void on_led_wizard_save_finish_clicked(GtkWidget *widget, gpointer user_data);
static void on_led_wizard_cancel_clicked(GtkWidget *widget, gpointer user_data);
static void kbd_wizard_step(AppData *app, const char *prev_key_name);
static void kbd_wizard_finish(AppData *app);
static void kbd_save_led_mapping_csv(AppData *app);
static void kbd_load_led_mapping_csv(AppData *app);
static void kbd_mapping_set_defaults(AppData *app);
static void kbd_refresh_key_colors(AppData *app);
static int kbd_check_unsaved_changes(AppData *app);   /* 1 saved/none, 0 discarded, -1 cancelled */
static void kbd_enter_tab(AppData *app);
static int kbd_leave_tab(AppData *app);                /* -1: user cancelled */
static void load_keyboard_profiles_select(AppData *app, const char *select_name);
static void kbd_sync_power_switch(AppData *app);
static gboolean cpu_refresh_timer_callback(gpointer user_data);
static void cpu_load_initial_data(AppData *app);
static void load_cpu_governors(AppData *app);
static void on_cpu_governor_changed(GtkWidget *widget, gpointer user_data);

/* Forward declarations for CPU custom frequency profile management */
static gboolean populate_cpu_core_grid_idle(gpointer data);
static void load_cpu_custom_profiles(AppData *app);
static void load_cpu_custom_profiles_select(AppData *app, int select_idx);
static void on_cpu_custom_profile_changed(GtkWidget *widget, gpointer user_data);
static void on_cpu_create_freq_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_cpu_delete_freq_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_cpu_save_freq_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_cpu_activate_freq_profile_clicked(GtkWidget *widget, gpointer user_data);
static void sync_core0_to_all_cores(AppData *app);
static void on_cpu_apply_all_cores_toggled(GtkCheckButton *checkbox, gpointer user_data);
static void build_cpu_core_freq_grid(AppData *app, int num_cores);
static void update_cpu_core_freq_spinners(AppData *app, int profile_idx);
static void mark_cpu_freq_profiles_modified(AppData *app);
static void cpu_take_snapshot(AppData *app);
static int cpu_profile_has_unsaved_changes(const AppData *app);
static int cpu_check_unsaved_changes(AppData *app); /* 1 saved/none, 0 discarded, -1 cancelled */
static void on_cpu_hard_limit_apply_clicked(GtkWidget *widget, gpointer user_data);

/* Forward declarations for profile and curve management */
static void refresh_profile_list(AppData *app);
static void load_curve_points_for_profile(AppData *app, int profile_idx);
static void gui_save_profiles_to_disk(AppData *app);
static void on_apply_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_notebook_page_switched(GtkNotebook *notebook, GtkWidget *page, gint page_num, gpointer user_data);
static void update_profile_tab_for_manual_mode(AppData *app, int manual_mode_enabled);
static void on_disable_manual_mode_clicked(GtkWidget *widget, gpointer user_data);
static int check_unsaved_changes(AppData *app); /* Returns 1 if changes applied, 0 if discarded, -1 if cancelled */

/* System tray functions */
static GtkWidget* create_tray_menu(AppData *app);
static GtkWidget *create_settings_page(AppData *app);
static void on_settings_layout_changed(GtkComboBox *combo, gpointer user_data);
static void on_settings_appearance_changed(GtkComboBox *combo, gpointer user_data);
static void appearance_refresh(AppData *app);
static void appearance_apply_theme(int mode);
static void appearance_save_choice(int mode);
static void on_gtk_theme_changed(GObject *obj, GParamSpec *pspec, gpointer user_data);
static void settings_update_layout_note(AppData *app);
static void kbd_mapping_path(char *buf, size_t size, const char *code);
static void kbd_load_layout_picture(AppData *app);
static void kbd_highlight_zone(AppData *app);
static cJSON *config_read(void);
static void config_write(cJSON *json);
static void kbd_layout_save_choice(void);
static void settings_apply_visibility(AppData *app);
static void on_tray_select_kbd_profile(GtkMenuItem *item, gpointer user_data);
static const char *send_keyboard_command_once(AppData *app, const char *cmd);
static int kbd_load_edit_profile(AppData *app, const char *name);
static void kbd_edit_changed(AppData *app, int send_preview, int light_on);
static void update_tray_menu(AppData *app);
static gboolean on_tray_popup_menu(GtkStatusIcon *status_icon, guint button, guint activate_time, gpointer user_data);
static void on_tray_activate_left_click(GtkStatusIcon *status_icon, gpointer user_data);
static void on_tray_show_app(GtkMenuItem *menuitem, gpointer user_data);
static void on_tray_exit(GtkMenuItem *menuitem, gpointer user_data);
static void on_tray_select_profile(GtkRadioMenuItem *radiomenuitem, gpointer user_data);
static void on_tray_select_cpu_profile(GtkRadioMenuItem *radiomenuitem, gpointer user_data);
static gboolean on_window_delete_event(GtkWidget *widget, GdkEvent *event, gpointer user_data);
static void create_system_tray(AppData *app);
static void save_window_position(AppData *app);
static void load_window_position(AppData *app);

/* Combined idle callback: refresh profile list + update manual mode UI (defer until GTK realizes widgets) */
static gboolean refresh_profile_tab_idle(gpointer data) {
    AppData *app = (AppData *)data;
    refresh_profile_list(app);
    update_profile_tab_for_manual_mode(app, app->in_manual_mode);
    return FALSE; /* Don't repeat */
}

/* Idle callback: populate CPU core frequency grid (defer until GTK realizes widgets on the page) */
static gboolean populate_cpu_core_grid_idle(gpointer data) {
    AppData *app = (AppData *)data;
    g_print("[GUI] populate_cpu_core_grid_idle: cpu_core_count=%d, active_idx=%d\n",
            app->cpu_core_count, app->cpu_active_custom_profile_idx);

    if (app->cpu_core_count > 0 && app->cpu_active_custom_profile_idx >= 0) {
        build_cpu_core_freq_grid(app, app->cpu_core_count);
        update_cpu_core_freq_spinners(app, app->cpu_active_custom_profile_idx);

        /* Restore checkbox state and enforce spinner disabled/enabled state */
        int idx = app->cpu_active_custom_profile_idx;
        char info_cmd[64];
        snprintf(info_cmd, sizeof(info_cmd), "GET_CUSTOM_PROFILE_INFO|%d", idx);
        const char *info_response = send_cpu_command(app, info_cmd);
        int all_cores = 1; /* Default: apply to all cores */
        if (info_response) {
            char *aac_ptr = strstr(info_response, "APPLY_ALL:");
            if (aac_ptr) {
                all_cores = atoi(aac_ptr + 10);
            }
        }

        g_signal_handler_block(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox), all_cores != 0);
        g_signal_handler_unblock(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);

        if (all_cores) {
            sync_core0_to_all_cores(app);
            for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], FALSE);
                gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], FALSE);
            }
        } else {
            for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], TRUE);
                gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], TRUE);
            }
        }
    }
    return FALSE; /* Don't repeat */
}

/* Helper function to show status messages */
static void show_status(AppData *app, const char *message) {
    if (app->statusbar) {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
        gtk_statusbar_pop(app->statusbar, app->status_context);
        gtk_statusbar_push(app->statusbar, app->status_context, message);
        #pragma GCC diagnostic pop
    }
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

/* Send command to daemon and receive response */
static const char *send_command(AppData *app, const char *cmd) {
    static char response[BUFFER_SIZE];
    
    if (!app->connected || app->socket_fd < 0) {
        strcpy(response, "ERROR Not connected to daemon");
        return response;
    }
    
    /* Add newline for line-buffered protocol */
    char cmd_with_newline[BUFFER_SIZE];
    snprintf(cmd_with_newline, sizeof(cmd_with_newline), "%s\n", cmd);
    
    /* Only print non-STATUS commands to avoid spam from periodic refreshes */
    if (strcmp(cmd, "STATUS") != 0) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Sending: %s\n", ts, cmd);
    }
    
    /* Send command */
    ssize_t sent = send(app->socket_fd, cmd_with_newline, strlen(cmd_with_newline), MSG_NOSIGNAL);  /* EPIPE instead of SIGPIPE */
    if (sent < 0) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] ERROR: Failed to send command (%s), marking disconnected\n", ts, strerror(errno));
        app->connected = 0;
        if (app->socket_fd >= 0) {
            close(app->socket_fd);
            app->socket_fd = -1;
        }
        strcpy(response, "ERROR Failed to send command");
        return response;
    }
    
    /* Receive response */
    memset(response, 0, sizeof(response));
    ssize_t received = read(app->socket_fd, response, sizeof(response) - 1);
    if (received < 0) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] ERROR: Failed to receive response (%s), marking disconnected\n", ts, strerror(errno));
        app->connected = 0;
        if (app->socket_fd >= 0) {
            close(app->socket_fd);
            app->socket_fd = -1;
        }
        strcpy(response, "ERROR Failed to receive response");
        return response;
    }
    if (received == 0) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] ERROR: Connection closed by daemon (EOF)\n", ts);
        strcpy(response, "ERROR Connection closed by daemon");
        app->connected = 0;
        return response;
    }
    
    /* Only print non-STATUS responses to avoid spam from periodic refreshes */
    if (strcmp(cmd, "STATUS") != 0) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Received (%zd bytes): %s...\n", ts, received, response);
    }
    
    return response;
}

/* Send command to CPU daemon via its own socket */
static const char *send_cpu_command(AppData *app, const char *cmd) {
    static char response[65536];   /* large enough for LIST_CUSTOM_PROFILES_JSON */

    if (!app->cpu_connected || app->cpu_socket_fd < 0) {
        strcpy(response, "ERROR Not connected to CPU daemon");
        return response;
    }

    /* No fixed size: SAVE_CUSTOM_PROFILE carries two values per core
     * (a 256-byte buffer silently dropped cores beyond ~10) */
    gchar *cmd_with_newline = g_strdup_printf("%s\n", cmd);

    if (strcmp(cmd, "STATUS") != 0) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] CPU Sending: %s\n", ts, cmd);
    }

    ssize_t sent = send(app->cpu_socket_fd, cmd_with_newline, strlen(cmd_with_newline), MSG_NOSIGNAL);  /* EPIPE instead of SIGPIPE */
    g_free(cmd_with_newline);
    if (sent < 0) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] CPU ERROR: Failed to send command (%s), marking disconnected\n", ts, strerror(errno));
        app->cpu_connected = 0;
        if (app->cpu_socket_fd >= 0) {
            close(app->cpu_socket_fd);
            app->cpu_socket_fd = -1;
        }
        strcpy(response, "ERROR Failed to send CPU command");
        return response;
    }

    memset(response, 0, sizeof(response));
    size_t total_received = 0;
    while (total_received < sizeof(response) - 1) {
        struct pollfd pfd;
        pfd.fd = app->cpu_socket_fd;
        pfd.events = POLLIN;
        /* Replies end with a NUL byte (daemons since 1.20.1): stop there. Wait
         * up to 3 s for a slow command; once data arrived, an older daemon
         * without the marker is detected by 200 ms of silence. */
        int poll_ret = poll(&pfd, 1, total_received == 0 ? 3000 : 200);
        if (poll_ret < 0) {
            char ts[32];
            get_timestamp(ts, sizeof(ts));
            g_print("%s [GUI] CPU ERROR: poll failed (%s), marking disconnected\n", ts, strerror(errno));
            app->cpu_connected = 0;
            if (app->cpu_socket_fd >= 0) {
                close(app->cpu_socket_fd);
                app->cpu_socket_fd = -1;
            }
            strcpy(response, "ERROR Failed to receive CPU response");
            return response;
        }
        if (poll_ret == 0) break; /* No more data within timeout */

        ssize_t received = read(app->cpu_socket_fd, response + total_received, sizeof(response) - 1 - total_received);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            char ts[32];
            get_timestamp(ts, sizeof(ts));
            g_print("%s [GUI] CPU ERROR: Failed to receive response (%s), marking disconnected\n", ts, strerror(errno));
            app->cpu_connected = 0;
            if (app->cpu_socket_fd >= 0) {
                close(app->cpu_socket_fd);
                app->cpu_socket_fd = -1;
            }
            strcpy(response, "ERROR Failed to receive CPU response");
            return response;
        }
        if (received == 0) {
            char ts[32];
            get_timestamp(ts, sizeof(ts));
            g_print("%s [GUI] CPU ERROR: Connection closed by daemon (EOF)\n", ts);
            strcpy(response, "ERROR CPU connection closed");
            app->cpu_connected = 0;
            return response;
        }
        int done = memchr(response + total_received, '\0', (size_t)received) != NULL;
        total_received += received;
        if (done) break;   /* end marker */
    }

    if (strcmp(cmd, "STATUS") != 0) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] CPU Received (%zd bytes): %s...\n", ts, total_received, response);
    }

    return response;
}

/* Parse CPU STATUS response and update UI */
static void parse_cpu_status(AppData *app, const char *response) {
    char buffer[8192];
    strncpy(buffer, response, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    


    int cpu_temp = 0;
    float cpu_load = 0.0f;
    int num_cpus = 0;
    char active_governor[64] = "N/A";
    char cur_gov[64] = "", cur_epp[64] = "";   /* raw values (daemons since 1.20.3) */
    char freq_values[256] = "N/A";
    char min_max_freq[128] = "N/A";
    
    /* Per-core frequency storage */
    char core_freqs[32][64];
    int core_count = 0;
    for (int i = 0; i < 32; i++) {
        strncpy(core_freqs[i], "N/A", sizeof(core_freqs[i]) - 1);
        core_freqs[i][sizeof(core_freqs[i]) - 1] = '\0';
    }

    char *saveptr = NULL;
    char *line = strtok_r(buffer, "\n", &saveptr);
    while (line != NULL) {
        if (strncmp(line, "CPU_TEMP=", 9) == 0) {
            cpu_temp = atoi(line + 9);
        } else if (strncmp(line, "CPU_LOAD=", 9) == 0) {
            cpu_load = atof(line + 9);
        } else if (strncmp(line, "NUM_CPUS=", 9) == 0) {
            num_cpus = atoi(line + 9);
        } else if (strncmp(line, "ACTIVE_GOVERNOR=", 16) == 0) {
            strncpy(active_governor, line + 16, sizeof(active_governor) - 1);
        } else if (strncmp(line, "GOVERNOR=", 9) == 0) {
            snprintf(cur_gov, sizeof cur_gov, "%s", line + 9);
        } else if (strncmp(line, "EPP=", 4) == 0) {
            snprintf(cur_epp, sizeof cur_epp, "%s", line + 4);
        } else if (strncmp(line, "MIN_FREQ=", 9) == 0) {
            strncpy(min_max_freq, line + 9, sizeof(min_max_freq) - 1);
        } else if (strncmp(line, "MAX_FREQ=", 9) == 0) {
            strncat(min_max_freq, " / ", sizeof(min_max_freq) - strlen(min_max_freq) - 1);
            strncat(min_max_freq, line + 9, sizeof(min_max_freq) - strlen(min_max_freq) - 1);
        } else {
            /* Parse per-core frequencies: CPU_N_FREQ=value (value may contain spaces like "2.40 GHz") */
            int core_idx = -1;
            char freq_tmp[64] = {0};
            char *freq_marker = strstr(line, "_FREQ=");
            if (freq_marker != NULL && strncmp(line, "CPU_", 4) == 0) {
                core_idx = atoi(line + 4);
                if (core_idx >= 0 && core_idx < 32) {
                    snprintf(freq_tmp, sizeof(freq_tmp), "%s", freq_marker + 6);
                    snprintf(core_freqs[core_idx], sizeof(core_freqs[core_idx]), "%s", freq_tmp);
                    if (core_idx >= core_count) {
                        core_count = core_idx + 1;
                    }
                }
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* Set current frequency from first core if available */
    if (core_count > 0) {
        strncpy(freq_values, core_freqs[0], sizeof(freq_values) - 1);
    }

    /* Update labels */
    char temp_text[32], load_text[32], num_text[32];
    snprintf(temp_text, sizeof(temp_text), "%d°C", cpu_temp);
    snprintf(load_text, sizeof(load_text), "%.1f%%", cpu_load);
    snprintf(num_text, sizeof(num_text), "%d", num_cpus);

    gtk_label_set_text(GTK_LABEL(app->cpu_page_temp_label), temp_text);
    gtk_label_set_text(GTK_LABEL(app->cpu_page_load_label), load_text);
    gtk_label_set_text(GTK_LABEL(app->cpu_page_num_label), num_text);
    /* "Balanced (powersave, balance_performance)" */
    gchar *mode_text = cur_gov[0]
        ? (cur_epp[0] && strcmp(cur_gov, "performance") != 0
               ? g_strdup_printf("%s (%s, %s)", active_governor, cur_gov, cur_epp)
               : g_strdup_printf("%s (%s)", active_governor, cur_gov))
        : g_strdup(active_governor);
    gtk_label_set_text(GTK_LABEL(app->cpu_page_governor_label), mode_text);
    g_free(mode_text);
    gtk_label_set_text(GTK_LABEL(app->cpu_page_freq_label), freq_values);
    gtk_label_set_text(GTK_LABEL(app->cpu_page_min_max_freq_label), min_max_freq);

    /* Populate per-core frequency labels on first call, update on subsequent calls */

    if (app->cpu_core_count == 0 && core_count > 0) {
        /* First time: create labels in a 4-column grid */
        app->cpu_core_count = core_count;

        for (int i = 0; i < core_count && i < 32; i++) {
            gchar *label_text = g_strdup_printf(_("Core %d: %s"), i, core_freqs[i]);
            app->cpu_core_freq_labels[i] = gtk_label_new(label_text);
            g_free(label_text);
            gtk_label_set_xalign(GTK_LABEL(app->cpu_core_freq_labels[i]), 0.0);
            
            /* 4 columns layout: row = i / 4, col = i % 4 */
            int row = i / 4;
            int col = i % 4;
            gtk_grid_attach(GTK_GRID(app->cpu_core_grid), app->cpu_core_freq_labels[i], col, row, 1, 1);
            gtk_widget_show(app->cpu_core_freq_labels[i]);
        }
        gtk_widget_show_all(GTK_WIDGET(app->cpu_core_grid));

    } else if (app->cpu_core_count > 0) {
        /* Subsequent calls: just update label text */
        for (int i = 0; i < app->cpu_core_count && i < 32; i++) {
            if (app->cpu_core_freq_labels[i] != NULL) {
                char label_text[128];
                snprintf(label_text, sizeof(label_text), _("Core %d: %s"), i, core_freqs[i]);
                gtk_label_set_text(GTK_LABEL(app->cpu_core_freq_labels[i]), label_text);
            }
        }
    }
}

/* CPU refresh timer callback - fetch CPU STATUS and update UI every 5 seconds */
/* Load what the CPU tab needs after connecting to the CPU daemon: power
 * modes, core count (STATUS) and the hard limit. Used at startup and when
 * the CPU refresh timer reconnects. */
static void cpu_load_initial_data(AppData *app) {
    /* Load governors into combo box */
    load_cpu_governors(app);

    /* Send an immediate STATUS query so cpu_core_count is available before the user switches tabs */
    {
        const char *immediate_status = send_cpu_command(app, "STATUS");
        if (immediate_status != NULL && strncmp(immediate_status, "ERROR", 5) != 0) {
            parse_cpu_status(app, immediate_status);
        }
    }

    /* Load current hard limit from daemon */
    {
        const char *hard_resp = send_cpu_command(app, "GET_HARD_LIMIT");
        if (hard_resp && strncmp(hard_resp, "OK", 2) == 0) {
            const char *eq = strstr(hard_resp, "HARD_LIMIT=");
            if (eq) {
                guint64 hz = g_ascii_strtoull(eq + 11, NULL, 10);
                double ghz = (double)hz / 1000000000.0;
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->cpu_hard_limit_spinner), ghz);
                g_print("[GUI] Loaded hard limit from daemon: %.2f GHz (%" G_GUINT64_FORMAT " Hz)\n", ghz, hz);
                /* Spinner upper bounds are updated in build_cpu_core_freq_grid() */
            }
        }
    }
}

static gboolean cpu_refresh_timer_callback(gpointer user_data) {
    AppData *app = (AppData *)user_data;

    if (!app->cpu_connected) {
        /* Reconnect after the CPU daemon was restarted or updated (the GUI
         * used to stay disconnected until it was reopened) */
        if (app->cpu_socket_fd >= 0) { close(app->cpu_socket_fd); app->cpu_socket_fd = -1; }
        connect_to_cpu_daemon(app);
        update_daemon_status_labels(app);
        if (!app->cpu_connected) return G_SOURCE_CONTINUE;
        g_print("[GUI] Reconnected to the CPU daemon\n");
        cpu_load_initial_data(app);
        load_cpu_custom_profiles_select(app,
            gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo)));
        update_tray_menu(app);
    }

    const char *response = send_cpu_command(app, "STATUS");
    if (response != NULL && strncmp(response, "ERROR", 5) != 0) {
        parse_cpu_status(app, response);
    }

    return G_SOURCE_CONTINUE;
}

/* Load governor profiles into the CPU combo box */
/* Power-mode list rows: column 0 = name (Performance/Balanced/Power Save),
 * column 1 = key sent to the daemon, "governor" or "governor:epp".
 * Returns the row for a profile's governor + EPP, or -1. Exact key first,
 * then by meaning, so profiles saved before EPP support (e.g. "schedutil"
 * on a system without it) still show the mode they were meant to use. */
static int gui_find_power_mode_row(GtkListStore *store, const char *gov, const char *epp) {
    if (!store || !gov || !*gov) return -1;
    char key[96];
    if (epp && *epp) snprintf(key, sizeof key, "%s:%s", gov, epp);
    else snprintf(key, sizeof key, "%s", gov);

    const char *meaning = NULL;
    if (strcmp(gov, "performance") == 0) meaning = "Performance";
    else if (strcmp(gov, "schedutil") == 0 || strcmp(gov, "ondemand") == 0 || strcmp(gov, "conservative") == 0)
        meaning = "Balanced";
    else if (strcmp(gov, "powersave") == 0)
        meaning = (epp && strncmp(epp, "balance_performance", 19) == 0) ? "Balanced" : "Power Save";

    int by_meaning = -1, idx = 0;
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it);
    while (ok) {
        gchar *name = NULL, *row_key = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &it, 0, &name, 1, &row_key, -1);
        int exact = row_key && strcmp(row_key, key) == 0;
        if (by_meaning < 0 && meaning && name && strcmp(name, meaning) == 0) by_meaning = idx;
        g_free(name);
        g_free(row_key);
        if (exact) return idx;
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &it);
        idx++;
    }
    return by_meaning;
}

static void load_cpu_governors(AppData *app) {
    if (!app->cpu_connected) {
        g_print("[GUI] CPU not connected, skipping governor load\n");
        return;
    }

    const char *response = send_cpu_command(app, "LIST_GOVERNORS");
    if (response == NULL || strncmp(response, "ERROR", 5) == 0) {
        g_print("[GUI] Failed to load governors: %s\n", response);
        return;
    }

    /* Clear existing list */
    gtk_list_store_clear(app->cpu_profile_store);

    /* Parse response: "GOVERNORS\n    Name (gov=..., min=..., max=...)\n[*] Name (gov=..., min=..., max=...)\n" */
    char buffer[8192];
    strncpy(buffer, response, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char *saveptr = NULL;
    char *line = strtok_r(buffer, "\n", &saveptr);
    int active_idx = 0;
    int count = 0;

    while (line != NULL) {
        /* Skip header line */
        if (strncmp(line, "GOVERNORS", 9) == 0) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* Skip empty lines */
        if (line[0] == '\0' || strlen(line) == 0) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* Extract profile name: skip leading markers ([*] or spaces), stop at '(' */
        char *name_start = line;
        while (*name_start == ' ' || *name_start == '[' || *name_start == ']' || *name_start == '*') {
            name_start++;
        }

        /* Extract governor string from "gov=xxx" BEFORE truncating the line */
        char gov_str[80] = "schedutil";
        char *gov_ptr = strstr(line, "gov=");
        if (gov_ptr) {
            char *gov_start = gov_ptr + 4;
            char *gov_end = strchr(gov_start, ',');
            if (gov_end) {
                int gov_len = gov_end - gov_start;
                if (gov_len > 0 && gov_len < (int)sizeof(gov_str)) {
                    memcpy(gov_str, gov_start, gov_len);
                    gov_str[gov_len] = '\0';
                }
            }
        }

        /* EPP of the policy ("epp=balance_performance," or "epp=-,") */
        char epp_str[32] = "";
        char *epp_ptr = strstr(line, "epp=");
        if (epp_ptr) {
            char *epp_start = epp_ptr + 4;
            size_t epp_len = strcspn(epp_start, ",)");
            if (epp_len > 0 && epp_len < sizeof(epp_str) && strncmp(epp_start, "-", epp_len) != 0) {
                memcpy(epp_str, epp_start, epp_len);
                epp_str[epp_len] = '\0';
            }
        }
        if (epp_str[0]) {
            /* key sent to the daemon when a profile is saved: governor:epp */
            size_t gl = strlen(gov_str);
            if (gl + 1 + strlen(epp_str) < sizeof(gov_str)) {
                gov_str[gl] = ':';
                memcpy(gov_str + gl + 1, epp_str, strlen(epp_str) + 1);
            }
        }

        char *paren = strchr(name_start, '(');
        if (paren) {
            *paren = '\0';
        }

        /* Trim trailing spaces */
        int len = strlen(name_start);
        while (len > 0 && name_start[len - 1] == ' ') {
            name_start[len - 1] = '\0';
            len--;
        }

        if (strlen(name_start) > 0) {
            /* Check if this is the active profile */
            if (strstr(line, "[*]") != NULL) {
                active_idx = count;
            }

            GtkTreeIter iter;
            gtk_list_store_append(app->cpu_profile_store, &iter);
            gtk_list_store_set(app->cpu_profile_store, &iter, 0, name_start, 1, gov_str, -1);
            g_print("[GUI]   Added governor row: name='%s' gov='%s'\n", name_start, gov_str);
            count++;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* Select the active profile (block signal to prevent callback during init) */
    if (active_idx < count) {
        g_signal_handler_block(app->cpu_profile_combo, app->cpu_profile_combo_signal_id);
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->cpu_profile_combo), active_idx);
        g_signal_handler_unblock(app->cpu_profile_combo, app->cpu_profile_combo_signal_id);
    }

    g_print("[GUI] Loaded %d governor profiles into combo box (active idx=%d)\n", count, active_idx);
}

/* Callback: Apply governor from combo box selection */
static void on_cpu_governor_changed(GtkWidget *widget, gpointer user_data) {
    AppData *app = (AppData *)user_data;

    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &iter)) {
        return;
    }

    gchar *governor_name = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_profile_store), &iter, 0, &governor_name, -1);
    if (!governor_name) {
        return;
    }

    /* With a per-core profile loaded, the power mode belongs to that profile
     * and is stored on Save. If that profile is the active one, the mode is
     * also applied right away (governor + EPP only, its frequencies stay);
     * for another profile it takes effect when that profile is activated. */
    if (app->cpu_active_custom_profile_idx >= 0) {
        mark_cpu_freq_profiles_modified(app);
        if (app->cpu_active_custom_profile_idx == app->cpu_running_profile_idx) {
            char command[256];
            snprintf(command, sizeof(command), "SET_POWER_MODE|%s", governor_name);
            const char *response = send_cpu_command(app, command);
            int ok = response && strncmp(response, "OK", 2) == 0;
            g_print("[GUI] Power mode '%s' for the active profile: %s\n", governor_name, response ? response : "NULL");
            show_status(app, ok ? _("Power mode applied - click Save to keep it in the profile")
                                : _("Failed to apply the power mode"));
        } else {
            g_print("[GUI] Power mode '%s' selected for an inactive profile (saved with Save, applied with Activate)\n",
                    governor_name);
            show_status(app, _("Power mode changed - click Save to store it in the profile, Activate to apply it"));
        }
        g_free(governor_name);
        return;
    }

    g_print("[GUI] Applying governor: %s\n", governor_name);

    /* Build command: APPLY_GOVERNOR <name> */
    char command[256];
    snprintf(command, sizeof(command), "APPLY_GOVERNOR|%s", governor_name);

    const char *response = send_cpu_command(app, command);
    if (response && strncmp(response, "OK", 2) == 0) {
        show_status(app, _("Governor applied successfully"));
        g_print("[GUI] Governor '%s' applied successfully\n", governor_name);
    } else {
        show_status(app, _("Failed to apply governor"));
        g_print("[GUI] Failed to apply governor: %s\n", response);
    }

    g_free(governor_name);
}

/* ============================================================
 * CPU Custom Frequency Profile Management callbacks
 * ============================================================ */

/* Load custom frequency profiles from daemon into combo box */
/* Reload the profile list and select the active profile */
static void load_cpu_custom_profiles(AppData *app) {
    load_cpu_custom_profiles_select(app, -1);
}

/* Reload the profile list and select select_idx (e.g. the profile just
 * saved or renamed); -1 or an invalid index selects the active profile.
 * Before 1.20.2 a reload always jumped to the active profile. */
static void load_cpu_custom_profiles_select(AppData *app, int select_idx) {
    if (!app->cpu_connected) {
        g_print("[GUI] CPU not connected, skipping custom profile load\n");
        return;
    }

    const char *response = send_cpu_command(app, "LIST_CUSTOM_PROFILES");
    if (response == NULL || strncmp(response, "ERROR", 5) == 0) {
        g_print("[GUI] Failed to load custom profiles: %s\n", response ? response : "(null)");
        return;
    }

    gtk_list_store_clear(app->cpu_custom_profile_store);

    /* Parse response: lines like " [*] ProfileName (gov=schedutil, cores=24, all_cores=0)"
     * or "     ProfileName (gov=performance, cores=24, all_cores=1)"
     * Strip the metadata in parentheses, keep marker + profile name as display name. */
    char *resp_copy = g_strdup(response);
    char *line = strtok(resp_copy, "\n");
    int profile_count = 0;
    int active_idx = -1;
    int active_all_cores = 1;  /* Default: apply to all cores */
    int sel_all_cores = 1;
    while (line != NULL) {
        /* Skip empty lines */
        if (strlen(line) == 0) {
            line = strtok(NULL, "\n");
            continue;
        }

        /* Check for active marker "[*]" */
        int is_active = (strstr(line, "[*]") != NULL);

        /* Extract all_cores value from metadata before stripping */
        int line_all_cores = 0;
        char *ac_ptr = strstr(line, "all_cores=");
        if (ac_ptr) {
            line_all_cores = atoi(ac_ptr + 10);
        }

        /* Strip metadata: truncate at '(' to remove "(gov=..., cores=..., all_cores=...)" */
        char *paren = strchr(line, '(');
        if (paren) *paren = '\0';
        g_strchomp(line);  /* Remove trailing whitespace */

        GtkTreeIter iter;
        gtk_list_store_append(app->cpu_custom_profile_store, &iter);
        gtk_list_store_set(app->cpu_custom_profile_store, &iter, 0, line, -1);
        if (is_active) {
            active_idx = profile_count;
            active_all_cores = line_all_cores;
        }
        if (profile_count == select_idx) sel_all_cores = line_all_cores;
        profile_count++;

        line = strtok(NULL, "\n");
    }
    g_free(resp_copy);
    app->cpu_running_profile_idx = active_idx;   /* [*] */

    /* Select the requested profile, otherwise the active one */
    if (select_idx >= 0 && select_idx < profile_count) {
        active_idx = select_idx;          /* below: "the profile to show" */
        active_all_cores = sel_all_cores;
    }
    if (active_idx >= 0) {
        g_signal_handler_block(app->cpu_custom_profile_combo, app->cpu_custom_profile_combo_signal_id);
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo), active_idx);
        g_signal_handler_unblock(app->cpu_custom_profile_combo, app->cpu_custom_profile_combo_signal_id);
        app->cpu_active_custom_profile_idx = active_idx;

        /* Build the grid immediately so the user can edit on first visit */
        if (app->cpu_core_count > 0) {
            build_cpu_core_freq_grid(app, app->cpu_core_count);
            update_cpu_core_freq_spinners(app, active_idx);
        }

        /* Enable the action buttons so the user can edit immediately */
        gtk_widget_set_sensitive(app->cpu_freq_profile_delete_btn, TRUE);
        gtk_widget_set_sensitive(app->cpu_freq_profile_rename_btn, TRUE);
        gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, TRUE);
        gtk_widget_set_sensitive(app->cpu_freq_profile_activate_btn, TRUE);
        gtk_widget_set_sensitive(app->cpu_apply_all_cores_checkbox, TRUE);

        /* Restore the Apply to All Cores checkbox state from the active profile.
         * Block the toggled callback and manually enforce spinner state, since
         * gtk_toggle_button_set_active() doesn't fire the callback if value is unchanged. */
        g_signal_handler_block(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox), active_all_cores != 0);
        g_signal_handler_unblock(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);

        /* Manually enforce spinner disabled/enabled state */
        if (active_all_cores) {
            sync_core0_to_all_cores(app);
            for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], FALSE);
                gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], FALSE);
            }
        } else {
            for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], TRUE);
                gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], TRUE);
            }
        }
    }

    if (active_idx >= 0) cpu_take_snapshot(app);   /* the values as stored */
    g_print("[GUI] Loaded %d custom frequency profiles (active idx=%d)\n", profile_count, active_idx);

}

/* Callback: Custom profile combo box changed */
static void on_cpu_custom_profile_changed(GtkWidget *widget, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int active = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
    if (active < 0) return;

    /* Unsaved changes in the profile shown so far (the widgets still show it) */
    if (app->cpu_snap_valid && app->cpu_snap_profile != active && cpu_profile_has_unsaved_changes(app)) {
        int left = app->cpu_snap_profile;
        int r = cpu_check_unsaved_changes(app);
        g_signal_handler_block(widget, app->cpu_custom_profile_combo_signal_id);
        if (r < 0) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(widget), left);   /* stay */
            g_signal_handler_unblock(widget, app->cpu_custom_profile_combo_signal_id);
            return;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(widget), active);     /* Save reselected the old one */
        g_signal_handler_unblock(widget, app->cpu_custom_profile_combo_signal_id);
    }

    app->cpu_active_custom_profile_idx = active;
    g_print("[GUI] Selected custom profile index %d\n", active);

    /* Enable delete, rename, save, and activate buttons */
    gtk_widget_set_sensitive(app->cpu_freq_profile_delete_btn, TRUE);
    gtk_widget_set_sensitive(app->cpu_freq_profile_rename_btn, TRUE);
    gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, TRUE);
    gtk_widget_set_sensitive(app->cpu_freq_profile_activate_btn, TRUE);
    gtk_widget_set_sensitive(app->cpu_apply_all_cores_checkbox, TRUE);

    /* Build the per-core freq grid if we have core count info */
    if (app->cpu_core_count > 0) {
        build_cpu_core_freq_grid(app, app->cpu_core_count);
        update_cpu_core_freq_spinners(app, active);

        /* Query the new profile's all_cores value and restore checkbox state */
        char info_cmd[64];
        snprintf(info_cmd, sizeof(info_cmd), "GET_CUSTOM_PROFILE_INFO|%d", active);
        const char *info_response = send_cpu_command(app, info_cmd);
        int all_cores = 1; /* Default: apply to all cores */
        if (info_response) {
            char *aac_ptr = strstr(info_response, "APPLY_ALL:");
            if (aac_ptr) {
                all_cores = atoi(aac_ptr + 10);
            }
        }
        g_print("[GUI] Profile %d all_cores=%d, setting checkbox\n", active, all_cores);

        /* Block the toggled callback to avoid double-firing during manual enforcement */
        g_signal_handler_block(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox), all_cores != 0);
        g_signal_handler_unblock(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);

        /* Manually enforce spinner disabled/enabled state */
        if (all_cores) {
            sync_core0_to_all_cores(app);
            for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], FALSE);
                gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], FALSE);
            }
        } else {
            for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], TRUE);
                gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], TRUE);
            }
        }
    } else {
        g_print("[GUI] Core count not yet available, skipping grid build\n");
    }

    cpu_take_snapshot(app);   /* the values as stored */
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Profile selected - click Activate to use it"));
}

/* Extract clean profile name from stored display string.
 * Stored format: " [*] ProfileName" or "     ProfileName"
 * Returns: pointer into display_name (not a new allocation). */
static const char *cpu_profile_name_from_display(const char *display_name) {
    const char *p = display_name;
    /* Skip leading whitespace */
    while (*p == ' ' || *p == '\t') p++;
    /* Skip "[*] " marker if present */
    if (strncmp(p, "[*] ", 4) == 0) p += 4;
    return p;
}

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

/* Callback: Create new frequency profile */
/* "Balanced" for a new CPU profile, as "governor[:epp]" for the daemon:
 * schedutil where the cpufreq driver offers it, otherwise (intel_pstate in
 * active mode: only performance and powersave) powersave with the EPP
 * balance_performance, like the built-in Balanced mode. reply is the
 * answer to GET_AVAILABLE_GOVERNORS ("GOVERNORS OK\nCOUNT=n\nname\n...").
 * Asking for schedutil where it does not exist made the daemon log a
 * warning for every new profile (1.22.2). */
static const char *cpu_balanced_mode(const char *reply) {
    if (reply && strncmp(reply, "GOVERNORS OK", 12) == 0) {
        for (const char *l = strchr(reply, '\n'); l; l = strchr(l, '\n')) {
            l++;
            if (strncmp(l, "schedutil", 9) == 0 && (l[9] == '\n' || l[9] == '\0'))
                return "schedutil";
        }
        return "powersave:balance_performance";
    }
    return "schedutil";   /* unknown: the daemon resolves it to an available governor */
}

static void on_cpu_create_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    if (cpu_check_unsaved_changes(app) < 0) return;
    g_print("[GUI] Create frequency profile clicked\n");

    /* Show dialog to enter profile name */
    GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Create CPU Frequency Profile"),
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                    _("_Create"), GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);

    GtkWidget *label = gtk_label_new(_("Enter profile name:"));
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
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
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter)) {
                do {
                    gchar *existing_name = NULL;
                    gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter, 0, &existing_name, -1);
                    if (existing_name && strcmp(cpu_profile_name_from_display(existing_name), name) == 0) {
                        duplicate = 1;
                        g_free(existing_name);
                        break;
                    }
                    g_free(existing_name);
                } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter));
            }

            if (duplicate) {
                GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                                 GTK_MESSAGE_ERROR,
                                                                 GTK_BUTTONS_OK,
                                                                 _("A CPU frequency profile with the name '%s' already exists."),
                                                                 name);
                gtk_dialog_run(GTK_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
                gtk_widget_destroy(dialog);
                return;
            }

            /* Send CREATE_CUSTOM_PROFILE command to daemon; a new profile
             * starts in the Balanced mode of this system */
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "CREATE_CUSTOM_PROFILE|%s|%s|%u|0", name,
                     cpu_balanced_mode(send_cpu_command(app, "GET_AVAILABLE_GOVERNORS")),
                     HARD_MAX_FREQ_HZ_DEFAULT);
            const char *response = send_cpu_command(app, cmd);
            g_print("[GUI] CREATE_CUSTOM_PROFILE response: %s\n", response ? response : "NULL");

            /* Reload the profile list */
            load_cpu_custom_profiles(app);

            /* Save profiles to disk (GUI writes directly) */
            gui_save_profiles_to_disk(app);

            /* Find and select the newly created profile */
            int new_idx = -1;
            if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter)) {
                int i = 0;
                do {
                    gchar *existing_name = NULL;
                    gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter, 0, &existing_name, -1);
                    if (existing_name && strcmp(cpu_profile_name_from_display(existing_name), name) == 0) {
                        new_idx = i;
                        g_free(existing_name);
                        break;
                    }
                    g_free(existing_name);
                    i++;
                } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter));
            }

            if (new_idx >= 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo), new_idx);
                app->cpu_active_custom_profile_idx = new_idx;

                /* Default: set APPLY_ALL_CORES to 1 for the new profile */
                {
                    char apply_cmd[64];
                    snprintf(apply_cmd, sizeof(apply_cmd), "SET_APPLY_ALL_CORES|%d|1", new_idx);
                    send_cpu_command(app, apply_cmd);
                }

                /* Load the new profile data into the editor grid */
                if (app->cpu_core_count > 0) {
                    build_cpu_core_freq_grid(app, app->cpu_core_count);
                    update_cpu_core_freq_spinners(app, new_idx);

                    /* Restore checkbox state and enforce spinner disabled/enabled state */
                    char info_cmd[64];
                    snprintf(info_cmd, sizeof(info_cmd), "GET_CUSTOM_PROFILE_INFO|%d", new_idx);
                    const char *info_response = send_cpu_command(app, info_cmd);
                    int all_cores = 1; /* Default: apply to all cores */
                    if (info_response) {
                        char *aac_ptr = strstr(info_response, "APPLY_ALL:");
                        if (aac_ptr) {
                            all_cores = atoi(aac_ptr + 10);
                        }
                    }

                    g_signal_handler_block(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox), all_cores != 0);
                    g_signal_handler_unblock(G_OBJECT(app->cpu_apply_all_cores_checkbox), app->cpu_apply_all_cores_signal_id);

                    if (all_cores) {
                        sync_core0_to_all_cores(app);
                        for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                            gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], FALSE);
                            gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], FALSE);
                        }
                    } else {
                        for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
                            gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], TRUE);
                            gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], TRUE);
                        }
                    }
                }
            }

            gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label),
                              g_strconcat(_("Created profile: "), name, NULL));
        }
    }

    gtk_widget_destroy(dialog);
}

/* Callback: Delete selected frequency profile */
static void on_cpu_delete_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Delete frequency profile clicked\n");

    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo));
    if (idx < 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("No profile selected"));
        return;
    }

    /* Get profile name for confirmation */
    gchar *profile_name = NULL;
    GtkTreeIter iter;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->cpu_custom_profile_combo), &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter, 0, &profile_name, -1);
    }

    if (profile_name == NULL) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Could not read profile name"));
        return;
    }

    /* Show confirmation dialog */
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_QUESTION,
                                               GTK_BUTTONS_YES_NO,
                                               _("Are you sure you want to delete the CPU frequency profile '%s'?"),
                                               profile_name);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES) {
        /* Send DELETE_CUSTOM_PROFILE command */
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "DELETE_CUSTOM_PROFILE|%d", idx);
        const char *response = send_cpu_command(app, cmd);
        g_print("[GUI] DELETE_CUSTOM_PROFILE response: %s\n", response ? response : "NULL");

        /* Reload the profile list */
        load_cpu_custom_profiles(app);

        /* Save profiles to disk (GUI writes directly) */
        gui_save_profiles_to_disk(app);

        /* Defer grid rebuild until the next main loop iteration so the widget
         * hierarchy is stable after the profile list reload. */
        if (app->cpu_core_count > 0 && app->cpu_active_custom_profile_idx >= 0) {
            g_idle_add(populate_cpu_core_grid_idle, app);
        } else {
            /* No profiles left, clear the grid */
            GList *children = gtk_container_get_children(GTK_CONTAINER(app->cpu_core_freq_grid));
            GList *child = children;
            while (child != NULL) {
                gtk_widget_destroy(GTK_WIDGET(child->data));
                child = child->next;
            }
            g_list_free(children);
        }

        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label),
                          g_strconcat(_("Deleted profile: "), profile_name, NULL));
    }

    g_free(profile_name);
    gtk_widget_destroy(dialog);
}

/* Callback: Rename selected frequency profile */
static void on_cpu_rename_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Rename frequency profile clicked\n");

    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo));
    if (idx < 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("No profile selected"));
        return;
    }

    /* Get current profile name */
    gchar *current_name = NULL;
    GtkTreeIter iter;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->cpu_custom_profile_combo), &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter, 0, &current_name, -1);
    }

    if (current_name == NULL) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Could not read profile name"));
        return;
    }

    /* Extract clean profile name from display string */
    const char *clean_name = cpu_profile_name_from_display(current_name);

    /* Show dialog to enter new name */
    GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Rename CPU Frequency Profile"),
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                    _("_Rename"), GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);

    GtkWidget *label = gtk_label_new(_("Enter new profile name:"));
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), clean_name);
    gtk_box_pack_start(GTK_BOX(content), entry, TRUE, TRUE, 0);

    gtk_widget_show_all(content);

    /* Set Accept as default so Enter key triggers rename */
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const gchar *new_name = gtk_entry_get_text(GTK_ENTRY(entry));

        if (strlen(new_name) > 0) {
            /* Send RENAME_CUSTOM_PROFILE command */
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "RENAME_CUSTOM_PROFILE|%d|%s", idx, new_name);
            const char *response = send_cpu_command(app, cmd);
            g_print("[GUI] RENAME_CUSTOM_PROFILE response: %s\n", response ? response : "NULL");

            /* Reload the profile list, staying on the renamed profile */
            load_cpu_custom_profiles_select(app, idx);

            /* Keep selection on the renamed profile */
            g_signal_handler_block(app->cpu_custom_profile_combo, app->cpu_custom_profile_combo_signal_id);
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo), idx);
            g_signal_handler_unblock(app->cpu_custom_profile_combo, app->cpu_custom_profile_combo_signal_id);

            /* Save profiles to disk (GUI writes directly) */
            gui_save_profiles_to_disk(app);

            gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label),
                              g_strconcat(_("Renamed profile to: "), new_name, NULL));
        } else {
            gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Name cannot be empty"));
        }
    }

    g_free(current_name);
    gtk_widget_destroy(dialog);
}

/* Callback: Save current profile values to disk */
/* Context struct for deferred save work */
typedef struct {
    AppData *app;
    int profile_idx;
    int num_cores;
    int apply_all_cores;
    char governor[64];
    double core_min_mhz[MAX_CPUS];
    double core_max_mhz[MAX_CPUS];
} CpuSaveContext;

/* Idle callback that performs the actual save work.
 * Decoupled from the button clicked signal so GTK can release
 * the button's pressed visual state before we destroy/recreate widgets.
 * Uses a single batch command (SAVE_CUSTOM_PROFILE) instead of ~50 round-trips. */
static gboolean cpu_save_freq_profile_idle(gpointer user_data) {
    CpuSaveContext *ctx = (CpuSaveContext *)user_data;
    AppData *app = ctx->app;
    int idx = ctx->profile_idx;
    int num_cores = ctx->num_cores;

    /* Build core data: comma-separated min0,max0,min1,max1,... */
    char core_data[MAX_CPUS * 48] = {0};   /* "min,max," per core, 64-bit Hz */
    size_t pos = 0;
    for (int i = 0; i < num_cores && i < MAX_CPUS; i++) {
        gulong min_hz = (gulong)(ctx->core_min_mhz[i] * 1000000);
        gulong max_hz = (gulong)(ctx->core_max_mhz[i] * 1000000);
        if (i > 0) {
            pos += snprintf(core_data + pos, sizeof(core_data) - pos, ",");
        }
        pos += snprintf(core_data + pos, sizeof(core_data) - pos, "%lu,%lu", min_hz, max_hz);
    }

    /* Send single batch command */
    char batch_cmd[MAX_CPUS * 48 + 256];
    snprintf(batch_cmd, sizeof(batch_cmd),
        "SAVE_CUSTOM_PROFILE|%d|%s|%d|%s",
        idx, ctx->governor, ctx->apply_all_cores, core_data);

    LOG_INFO("Sending batch save for profile %d", idx);
    const char *resp = send_cpu_command(app, batch_cmd);
    LOG_INFO("SAVE_CUSTOM_PROFILE response: %s", resp ? resp : "(NULL)");

    /* Mark profiles modified and save to disk */
    mark_cpu_freq_profiles_modified(app);
    gui_save_profiles_to_disk(app);
    LOG_INFO("Profiles saved to disk");

    /* Reload profile list, staying on the profile just saved */
    load_cpu_custom_profiles_select(app, idx);
    LOG_INFO("Profile list reloaded");

    char status_msg[128];
    snprintf(status_msg, sizeof(status_msg), _("Saved profile"));
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), status_msg);

    /* Re-enable the save button */
    gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, TRUE);
    LOG_INFO("<<< Save frequency profile completed");

    g_free(ctx);
    return G_SOURCE_REMOVE;
}

/* Capture the shown profile values (widgets) for SAVE_CUSTOM_PROFILE */
static CpuSaveContext *cpu_save_ctx_from_ui(AppData *app, int idx) {
    CpuSaveContext *ctx = g_new0(CpuSaveContext, 1);
    ctx->app = app;
    ctx->profile_idx = idx;
    ctx->num_cores = app->cpu_core_count;
    ctx->apply_all_cores = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)) ? 1 : 0;

    GtkTreeIter gov_iter;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->cpu_profile_combo), &gov_iter)) {
        gchar *gov_name = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_profile_store), &gov_iter, 1, &gov_name, -1);
        snprintf(ctx->governor, sizeof(ctx->governor), "%s", gov_name ? gov_name : "");
        g_free(gov_name);
    }
    for (int i = 0; i < ctx->num_cores && i < MAX_CPUS; i++) {
        ctx->core_min_mhz[i] = gtk_adjustment_get_value(app->cpu_core_min_adjustments[i]);
        ctx->core_max_mhz[i] = gtk_adjustment_get_value(app->cpu_core_max_adjustments[i]);
    }
    return ctx;
}

/* --- Unsaved CPU profile changes (1.23.0) ---
 * A snapshot of the shown profile is taken whenever it is loaded or saved;
 * "unsaved" means the widgets differ from it. Values the GUI sets itself
 * (loading, syncing core 0 to all cores) are part of the snapshot, so only
 * the user's edits count. */
static void cpu_take_snapshot(AppData *app) {
    app->cpu_snap_valid = 0;
    int idx = app->cpu_custom_profile_combo
            ? gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo)) : -1;
    int n = app->cpu_core_count < MAX_CPUS ? app->cpu_core_count : MAX_CPUS;
    if (idx < 0 || n <= 0) return;
    for (int i = 0; i < n; i++)
        if (!app->cpu_core_min_adjustments[i] || !app->cpu_core_max_adjustments[i]) return;
    app->cpu_snap_profile = idx;
    app->cpu_snap_mode_row = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_profile_combo));
    app->cpu_snap_all_cores = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)) ? 1 : 0;
    app->cpu_snap_cores = n;
    for (int i = 0; i < n; i++) {
        app->cpu_snap_min[i] = gtk_adjustment_get_value(app->cpu_core_min_adjustments[i]);
        app->cpu_snap_max[i] = gtk_adjustment_get_value(app->cpu_core_max_adjustments[i]);
    }
    app->cpu_snap_valid = 1;
}

static int cpu_profile_has_unsaved_changes(const AppData *app) {
    if (!app->cpu_snap_valid || app->cpu_core_count != app->cpu_snap_cores) return 0;
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_profile_combo)) != app->cpu_snap_mode_row) return 1;
    if ((gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)) ? 1 : 0)
        != app->cpu_snap_all_cores) return 1;
    for (int i = 0; i < app->cpu_snap_cores; i++) {
        if (!app->cpu_core_min_adjustments[i] || !app->cpu_core_max_adjustments[i]) return 0;
        if (gtk_adjustment_get_value(app->cpu_core_min_adjustments[i]) != app->cpu_snap_min[i] ||
            gtk_adjustment_get_value(app->cpu_core_max_adjustments[i]) != app->cpu_snap_max[i]) return 1;
    }
    return 0;
}

/* Like the fan curve editor: Cancel / Discard Changes / Save Changes.
 * Returns 1 when there was nothing to save or it was saved, 0 when the
 * changes were discarded (widgets back to the stored values), -1 when the
 * user cancelled (the caller stops). */
static int cpu_check_unsaved_changes(AppData *app) {
    if (!cpu_profile_has_unsaved_changes(app)) return 1;

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, NULL);
    gtk_message_dialog_set_markup(GTK_MESSAGE_DIALOG(dialog),
        _("<b>Warning:</b> You have unsaved changes to the CPU profile.\n\nWhat would you like to do?"));
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Cancel"), -102);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Discard Changes"), -100);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("_Save Changes"), -101);
    gtk_widget_show_all(dialog);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    int idx = app->cpu_snap_profile;
    if (response == -101) {
        g_print("[GUI] Unsaved CPU profile changes: saving profile %d\n", idx);
        cpu_save_freq_profile_idle(cpu_save_ctx_from_ui(app, idx));   /* now, not deferred */
        return 1;
    }
    if (response == -100) {
        g_print("[GUI] Unsaved CPU profile changes: discarded (profile %d)\n", idx);
        /* A power mode change on the running profile was applied right
         * away: put the stored mode back */
        int row = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_profile_combo));
        if (row != app->cpu_snap_mode_row && idx == app->cpu_running_profile_idx) {
            GtkTreeIter it;
            if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(app->cpu_profile_store), &it, NULL,
                                              app->cpu_snap_mode_row)) {
                gchar *mode = NULL;
                gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_profile_store), &it, 0, &mode, -1);
                if (mode) {
                    char cmd[256];
                    snprintf(cmd, sizeof(cmd), "SET_POWER_MODE|%s", mode);
                    send_cpu_command(app, cmd);
                    g_free(mode);
                }
            }
        }
        load_cpu_custom_profiles_select(app, idx);   /* widgets back to the stored values */
        return 0;
    }
    return -1;
}

static void on_cpu_save_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    LOG_INFO(">>> Save frequency profile clicked");

    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo));
    if (idx < 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("No profile selected"));
        LOG_INFO("<<< Save aborted: no profile selected");
        return;
    }

    int num_cores = app->cpu_core_count;
    if (num_cores <= 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Core count not available"));
        LOG_INFO("<<< Save aborted: core count not available");
        return;
    }

    CpuSaveContext *ctx = cpu_save_ctx_from_ui(app, idx);

    /* Immediately disable the save button to clear its pressed visual state,
     * then re-enable it after the save completes in the idle callback. */
    gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Saving..."));

    /* Defer the heavy save work so GTK can process the button state change */
    g_idle_add((GSourceFunc)cpu_save_freq_profile_idle, ctx);
    LOG_INFO("Save deferred to idle callback, button disabled");
}

/* Idle callback struct for activate profile */
typedef struct {
    AppData *app;
    int profile_idx;
} ActivateIdleData;

static gboolean activate_profile_idle_callback(gpointer user_data) {
    ActivateIdleData *data = (ActivateIdleData *)user_data;
    AppData *app = data->app;
    int idx = data->profile_idx;
    g_free(data);

    /* Apply the profile */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "APPLY_CUSTOM_PROFILE|%d", idx);
    const char *response = send_cpu_command(app, cmd);
    g_print("[GUI] APPLY_CUSTOM_PROFILE response: %s\n", response ? response : "NULL");

    /* Reload profile list so the [*] marker updates to the newly active profile */
    load_cpu_custom_profiles(app);

    /* Persist the active_profile_index to profiles.json */
    gui_save_profiles_to_disk(app);

    char status_msg[128];
    snprintf(status_msg, sizeof(status_msg), _("Activated profile"));
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), status_msg);

    return FALSE; /* One-shot */
}

/* Callback: Activate (apply) the selected frequency profile to the CPU */
static void on_cpu_activate_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Activate frequency profile clicked\n");
    if (cpu_check_unsaved_changes(app) < 0) return;

    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo));
    if (idx < 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("No profile selected"));
        return;
    }

    /* Defer the apply + reload + save to an idle callback so GTK can finish
     * processing the button click before we destroy/rebuild the grid widgets.
     * This prevents race conditions with pending signal handlers. */
    ActivateIdleData *data = g_new0(ActivateIdleData, 1);
    data->app = app;
    data->profile_idx = idx;
    g_idle_add(activate_profile_idle_callback, data);
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), _("Activating..."));
}

/* Callback: Apply hard frequency limit */
static void on_cpu_hard_limit_apply_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;

    double ghz = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->cpu_hard_limit_spinner));
    guint64 hz = (guint64)(ghz * 1000000000.0 + 0.5);

    g_print("[GUI] Applying hard limit: %.2f GHz (%" G_GUINT64_FORMAT " Hz)\n", ghz, hz);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "SET_HARD_LIMIT|%" G_GUINT64_FORMAT, hz);
    const char *response = send_cpu_command(app, cmd);
    g_print("[GUI] SET_HARD_LIMIT response: %s\n", response ? response : "NULL");

    if (response && strncmp(response, "OK", 2) == 0) {
        char status_msg[256];
        snprintf(status_msg, sizeof(status_msg), _("Hard limit applied: %.2f GHz"), ghz);
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), status_msg);

        /* Update core freq spinner upper bounds to match new hard limit */
        int max_mhz = (int)(hz / 1000000);
        for (int i = 0; i < app->cpu_core_count && i < 32; i++) {
            if (app->cpu_core_min_adjustments[i]) {
                gtk_adjustment_set_upper(GTK_ADJUSTMENT(app->cpu_core_min_adjustments[i]), max_mhz);
            }
            if (app->cpu_core_max_adjustments[i]) {
                gtk_adjustment_set_upper(GTK_ADJUSTMENT(app->cpu_core_max_adjustments[i]), max_mhz);
            }
        }
    } else {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label),
            response ? response : _("Failed to apply hard limit"));
    }
}

/* Helper: copy core 0 min/max to all other cores when checkbox is active */
static void sync_core0_to_all_cores(AppData *app) {
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)))
        return;
    if (app->cpu_core_count <= 0) return;

    double first_min = gtk_adjustment_get_value(app->cpu_core_min_adjustments[0]);
    double first_max = gtk_adjustment_get_value(app->cpu_core_max_adjustments[0]);

    for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
        gtk_adjustment_set_value(app->cpu_core_min_adjustments[i], first_min);
        gtk_adjustment_set_value(app->cpu_core_max_adjustments[i], first_max);
    }
}

/* Sync core 0 values to all cores by reading Entry text directly (for live sync during typing)
 * Called from idle callback, so GTK has processed the text */
static void sync_core0_live_to_all_cores(AppData *app) {
    /* Guard: check if widgets still exist (grid may have been rebuilt) */
    if (!GTK_IS_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox))
        return;
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)))
        return;
    if (app->cpu_core_count <= 0) return;
    if (!GTK_IS_SPIN_BUTTON(app->cpu_core_min_spinners[0]))
        return;
    if (!GTK_IS_SPIN_BUTTON(app->cpu_core_max_spinners[0]))
        return;

    /* Read text directly from core 0 spinners (GtkSpinButton IS a GtkEntry in GTK3) */
    const gchar *min_text = gtk_entry_get_text(GTK_ENTRY(app->cpu_core_min_spinners[0]));
    const gchar *max_text = gtk_entry_get_text(GTK_ENTRY(app->cpu_core_max_spinners[0]));
    if (!min_text || !max_text)
        return;

    /* Skip empty text */
    if (min_text[0] == '\0' || max_text[0] == '\0')
        return;

    /* Parse - skip non-numeric */
    char *end1 = NULL, *end2 = NULL;
    double first_min = strtod(min_text, &end1);
    double first_max = strtod(max_text, &end2);
    if (end1 == min_text || end2 == max_text)
        return;

    for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
        if (!GTK_IS_SPIN_BUTTON(app->cpu_core_min_spinners[i]))
            return;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->cpu_core_min_spinners[i]), first_min);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->cpu_core_max_spinners[i]), first_max);
    }
}

/* Idle callback: defer sync until GTK has finished processing the text */
typedef struct {
    AppData *app;
} SyncIdleData;

static gboolean sync_core0_idle_callback(gpointer user_data) {
    SyncIdleData *data = (SyncIdleData *)user_data;
    sync_core0_live_to_all_cores(data->app);
    g_free(data);
    return FALSE; /* One-shot */
}

/* Callback: core 0 Entry changed signal - fires on every keystroke */
static void on_core0_entry_changed(GtkEntry *entry, gpointer user_data) {
    (void)entry;
    AppData *app = (AppData *)user_data;

    /* Defer the sync so GTK finishes processing first */
    SyncIdleData *data = g_new0(SyncIdleData, 1);
    data->app = app;
    g_idle_add(sync_core0_idle_callback, data);
}

/* Callback: Apply to All Cores checkbox toggled */
static void on_cpu_apply_all_cores_toggled(GtkCheckButton *checkbox, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checkbox));
    g_print("[GUI] Apply to All Cores: %s\n", active ? "ON" : "OFF");

    if (app->cpu_core_count <= 0) return;

    if (active) {
        /* Copy first core's min/max values to all cores and disable other spinners */
        sync_core0_to_all_cores(app);

        for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
            gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], FALSE);
            gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], FALSE);
        }

        g_print("[GUI] Copied core 0 values (min=%.0f MHz, max=%.0f MHz) to all %d cores\n",
                gtk_adjustment_get_value(app->cpu_core_min_adjustments[0]),
                gtk_adjustment_get_value(app->cpu_core_max_adjustments[0]),
                app->cpu_core_count);
    } else {
        /* Re-enable all spinners */
        for (int i = 1; i < app->cpu_core_count && i < 32; i++) {
            gtk_widget_set_sensitive(app->cpu_core_min_spinners[i], TRUE);
            gtk_widget_set_sensitive(app->cpu_core_max_spinners[i], TRUE);
        }
    }
}

/* Build the per-core frequency grid with spinners */
static void build_cpu_core_freq_grid(AppData *app, int num_cores) {
    g_print("[GUI] build_cpu_core_freq_grid called with %d cores\n", num_cores);

    /* Clear existing grid contents */
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->cpu_core_freq_grid));
    GList *child = children;
    while (child != NULL) {
        gtk_widget_destroy(GTK_WIDGET(child->data));
        child = child->next;
    }
    g_list_free(children);

    /* Grid columns: Core Label | Min Freq (MHz) | Max Freq (MHz) */
    int col_label = 0;
    int col_min = 1;
    int col_max = 2;

    for (int i = 0; i < num_cores && i < MAX_CPUS; i++) {
        int row = i;

        /* Core label */
        char core_name[32];
        snprintf(core_name, sizeof(core_name), _("Core %d"), i);
        GtkWidget *core_label = gtk_label_new(core_name);
        gtk_widget_set_halign(core_label, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(app->cpu_core_freq_grid), core_label, col_label, row, 1, 1);
        gtk_widget_set_margin_end(core_label, 5);

        /* Min freq spinner (800-3600 MHz, step 100) */
        app->cpu_core_min_adjustments[i] = gtk_adjustment_new(800, 800, 3600, 100, 100, 0);
        app->cpu_core_min_spinners[i] = gtk_spin_button_new(GTK_ADJUSTMENT(app->cpu_core_min_adjustments[i]), 100, 0);
        gtk_widget_set_hexpand(app->cpu_core_min_spinners[i], TRUE);
        gtk_grid_attach(GTK_GRID(app->cpu_core_freq_grid), app->cpu_core_min_spinners[i], col_min, row, 1, 1);
        gtk_widget_set_margin_end(app->cpu_core_min_spinners[i], 5);

        /* Max freq spinner (800-3600 MHz, step 100) */
        app->cpu_core_max_adjustments[i] = gtk_adjustment_new(3600, 800, 3600, 100, 100, 0);
        app->cpu_core_max_spinners[i] = gtk_spin_button_new(GTK_ADJUSTMENT(app->cpu_core_max_adjustments[i]), 100, 0);
        gtk_widget_set_hexpand(app->cpu_core_max_spinners[i], TRUE);
        gtk_grid_attach(GTK_GRID(app->cpu_core_freq_grid), app->cpu_core_max_spinners[i], col_max, row, 1, 1);
    }

    /* Update spinner upper bounds to match the current hard limit */
    {
        const char *hard_resp = send_cpu_command(app, "GET_HARD_LIMIT");
        if (hard_resp && strncmp(hard_resp, "OK", 2) == 0) {
            const char *eq = strstr(hard_resp, "HARD_LIMIT=");
            if (eq) {
                guint64 hz = g_ascii_strtoull(eq + 11, NULL, 10);
                int max_mhz = (int)(hz / 1000000);
                for (int i = 0; i < num_cores; i++) {
                    gtk_adjustment_set_upper(GTK_ADJUSTMENT(app->cpu_core_min_adjustments[i]), max_mhz);
                    gtk_adjustment_set_upper(GTK_ADJUSTMENT(app->cpu_core_max_adjustments[i]), max_mhz);
                }
                g_print("[GUI] Updated core freq spinner upper bounds to %d MHz\n", max_mhz);
            }
        }
    }

    /* Connect core 0 spinners' changed signal to sync all cores when Apply to All is active
     * The changed signal fires on every keystroke for real-time sync */
    {
        g_signal_connect(app->cpu_core_min_spinners[0], "changed", G_CALLBACK(on_core0_entry_changed), app);
        g_signal_connect(app->cpu_core_max_spinners[0], "changed", G_CALLBACK(on_core0_entry_changed), app);
    }

    /* Add header labels */
    GtkWidget *min_header = gtk_label_new(_("Min (MHz)"));
    gtk_widget_set_halign(min_header, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(min_header, 2);
    gtk_grid_attach(GTK_GRID(app->cpu_core_freq_grid), min_header, col_min, num_cores, 1, 1);

    GtkWidget *max_header = gtk_label_new(_("Max (MHz)"));
    gtk_widget_set_halign(max_header, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(max_header, 2);
    gtk_grid_attach(GTK_GRID(app->cpu_core_freq_grid), max_header, col_max, num_cores, 1, 1);

    gtk_widget_show_all(app->cpu_core_freq_grid);
}

/* Update spinner values from profile data */
static void update_cpu_core_freq_spinners(AppData *app, int profile_idx) {
    g_print("[GUI] update_cpu_core_freq_spinners called for profile %d\n", profile_idx);

    if (!app->cpu_connected || app->cpu_socket_fd < 0) {
        g_print("[GUI] CPU daemon not connected, cannot update spinners\n");
        return;
    }

    /* Send GET_CUSTOM_PROFILE_INFO command */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "GET_CUSTOM_PROFILE_INFO|%d", profile_idx);
    const char *response = send_cpu_command(app, cmd);

    if (response == NULL || strstr(response, "ERROR")) {
        g_print("[GUI] Failed to get custom profile info: %s\n", response ? response : "NULL");
        return;
    }

    /* Parse response: lines like CORE_0_MAX:3600000000 and CORE_0_MIN:800000000 */
    char profile_gov[32] = "", profile_epp[32] = "";
    char response_copy[BUFFER_SIZE];
    strncpy(response_copy, response, sizeof(response_copy) - 1);
    response_copy[sizeof(response_copy) - 1] = '\0';

    char *line = strtok(response_copy, "\n");
    while (line != NULL) {
        /* Trim leading/trailing whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        int core_id = -1;
        unsigned long long freq_hz = 0;

        if (strncmp(p, "GOVERNOR:", 9) == 0) {
            snprintf(profile_gov, sizeof profile_gov, "%s", p + 9);
        } else if (strncmp(p, "EPP:", 4) == 0) {
            snprintf(profile_epp, sizeof profile_epp, "%s", p + 4);
        } else if (sscanf(p, "CORE_%d_MAX:%llu", &core_id, &freq_hz) == 2) {
            if (core_id >= 0 && core_id < MAX_CPUS && app->cpu_core_max_adjustments[core_id] != NULL) {
                /* Convert Hz to MHz for spinner */
                double freq_mhz = (double)freq_hz / 1000000.0;
                gtk_adjustment_set_value(GTK_ADJUSTMENT(app->cpu_core_max_adjustments[core_id]), freq_mhz);
            }
        } else if (sscanf(p, "CORE_%d_MIN:%llu", &core_id, &freq_hz) == 2) {
            if (core_id >= 0 && core_id < MAX_CPUS && app->cpu_core_min_adjustments[core_id] != NULL) {
                double freq_mhz = (double)freq_hz / 1000000.0;
                gtk_adjustment_set_value(GTK_ADJUSTMENT(app->cpu_core_min_adjustments[core_id]), freq_mhz);
            }
        }

        line = strtok(NULL, "\n");
    }

    /* Show the profile's power mode (without triggering the change handler) */
    int mode_row = gui_find_power_mode_row(app->cpu_profile_store, profile_gov, profile_epp);
    if (mode_row >= 0 && app->cpu_profile_combo) {
        g_signal_handler_block(app->cpu_profile_combo, app->cpu_profile_combo_signal_id);
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->cpu_profile_combo), mode_row);
        g_signal_handler_unblock(app->cpu_profile_combo, app->cpu_profile_combo_signal_id);
    }
    g_print("[GUI] Profile %d power mode: governor=%s epp=%s -> row %d\n",
            profile_idx, profile_gov, profile_epp[0] ? profile_epp : "-", mode_row);

    g_print("[GUI] Spinner values updated from profile %d\n", profile_idx);
}

/* Mark that frequency profiles have been modified */
static void mark_cpu_freq_profiles_modified(AppData *app) {
    app->cpu_freq_profiles_modified = 1;
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
        g_print("%s [GUI] Manual mode ENABLED, app->in_manual_mode=%d, app->auto_mode_enabled=%d\n", ts, app->in_manual_mode, app->auto_mode_enabled);
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
        g_print("%s [GUI] Manual mode DISABLED (auto mode enabled)\n", ts);
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
    
    /* User clicked Add/Update - proceed with the operation */
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
    
    /* Collect all existing points plus the new/edited point */
    CurvePoint all_points[MAX_CURVE_POINTS];
    int point_count = 0;
    
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->curve_store), &iter)) {
        do {
            gchar *temp_str;
            int fpct;
            gtk_tree_model_get(GTK_TREE_MODEL(app->curve_store), &iter,
                              0, &temp_str, 1, &fpct, -1);
            if (point_count < MAX_CURVE_POINTS) {
                all_points[point_count].temp = atoi(temp_str);
                all_points[point_count].fan_pct = fpct;
                point_count++;
            }
            g_free(temp_str);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->curve_store), &iter));
    }
    
    /* If editing, update the existing point; otherwise add new point */
    if (edit_iter) {
        /* Find and update the edited point in our array */
        for (int i = 0; i < point_count; i++) {
            if (all_points[i].temp == edit_temp) {
                all_points[i].fan_pct = fan_pct;
                break;
            }
        }
    } else {
        /* Add new point */
        if (point_count < MAX_CURVE_POINTS) {
            all_points[point_count].temp = temp;
            all_points[point_count].fan_pct = fan_pct;
            point_count++;
        }
    }
    
    /* Sort by temperature ascending */
    if (point_count > 1) {
        qsort(all_points, point_count, sizeof(CurvePoint), compare_curve_points_asc);
    }
    
    /* Clear and repopulate the tree view with sorted points */
    gtk_list_store_clear(GTK_LIST_STORE(app->curve_store));
    for (int i = 0; i < point_count; i++) {
        char temp_str[16];
        snprintf(temp_str, sizeof(temp_str), "%d°C", all_points[i].temp);
        gtk_list_store_append(GTK_LIST_STORE(app->curve_store), &iter);
        gtk_list_store_set(GTK_LIST_STORE(app->curve_store), &iter,
                          0, temp_str,
                          1, all_points[i].fan_pct,
                          -1);
    }
    
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

/* Callback: Window destroy */
static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    
    /* Stop refresh timer */
    if (app->refresh_timer_id > 0) {
        g_source_remove(app->refresh_timer_id);
    }
    
    /* Stop log refresh timer */
    if (app->log_refresh_timer_id > 0) {
        g_source_remove(app->log_refresh_timer_id);
    }

    /* Stop CPU refresh timer */
    if (app->cpu_refresh_timer_id > 0) {
        g_source_remove(app->cpu_refresh_timer_id);
    }

    /* Free profile names */
    if (app->profile_names) {
        for (int i = 0; i < app->num_profiles; i++) {
            g_free(app->profile_names[i]);
        }
        g_free(app->profile_names);
    }
    
    /* Free profile radios list */
    
    /* Disconnect from daemon */
    disconnect_from_daemon(app);

    /* Disconnect from CPU daemon */
    disconnect_from_cpu_daemon(app);

    gtk_main_quit();
}

/* Connect to daemon */
/* "<name> daemon connected", or in red when it is not */
static void set_daemon_status(GtkWidget *label, const char *name, int connected) {
    if (!label || !GTK_IS_LABEL(label)) return;
    gchar *m = connected
        ? g_markup_printf_escaped(_("%s daemon connected"), name)
        : g_markup_printf_escaped(_("<span foreground='#d9534f' weight='bold'>%s daemon not connected</span>"), name);
    gtk_label_set_markup(GTK_LABEL(label), m);
    g_free(m);
}

/* Status lines of the Fan, CPU and Keyboard tabs */
static void update_daemon_status_labels(AppData *app) {
    set_daemon_status(app->fan_daemon_status_label, _("Fan"), app->connected && app->socket_fd >= 0);
    set_daemon_status(app->cpu_daemon_status_label, _("CPU"), app->cpu_connected && app->cpu_socket_fd >= 0);
    set_daemon_status(app->kbd_status_label, _("Keyboard"), app->kbd_connected && app->kbd_socket_fd >= 0);
    /* switched off in Settings: grey, not a red "not connected" (1.27.0) */
    GtkWidget *labels[COMP_COUNT] = { app->fan_daemon_status_label, app->cpu_daemon_status_label, app->kbd_status_label };
    const char *names[COMP_COUNT] = { _("Fan"), _("CPU"), _("Keyboard") };
    for (int c = 0; c < COMP_COUNT; c++)
        if (labels[c] && GTK_IS_LABEL(labels[c]) && dcc_component_disabled(c)) {
            gchar *m = g_markup_printf_escaped(_("<span foreground='#888888'>%s control is switched off (Settings tab)</span>"), names[c]);
            gtk_label_set_markup(GTK_LABEL(labels[c]), m);
            g_free(m);
        }
    if (app->settings_state[0]) {
        int up[COMP_COUNT] = { app->connected && app->socket_fd >= 0, app->cpu_connected && app->cpu_socket_fd >= 0,
                               app->kbd_connected && app->kbd_socket_fd >= 0 };
        for (int c = 0; c < COMP_COUNT; c++) {
            const char *t = dcc_component_disabled(c) ? _("<span foreground='#888888'>Switched off</span>")
                          : up[c] ? _("Running (daemon connected)")
                                  : _("<span foreground='#d9534f'>On, daemon not connected</span>");
            gtk_label_set_markup(GTK_LABEL(app->settings_state[c]), t);
        }
    }
}

static void connect_to_daemon(AppData *app) {
    if (dcc_component_disabled(COMP_FAN)) {          /* switched off in Settings */
        app->socket_fd = -1;
        app->connected = 0;
        return;
    }
    
    /* Check if socket file exists first */
    struct stat st;
    if (stat(SOCKET_PATH, &st) != 0) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    GTK_MESSAGE_ERROR,
                                                    GTK_BUTTONS_OK,
                                                    _("Daemon is not running!\n\n"
                                                    "The socket file does not exist at: %s\n\n"
                                                    "To start the daemon, run in a terminal:\n"
                                                    "  sudo dcc-fan-daemon --foreground\n\n"
                                                    "Or enable the systemd service:\n"
                                                    "  sudo systemctl enable --now dcc-fan-daemon"),
                                                    SOCKET_PATH);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        app->socket_fd = -1;
        return;
    }
    
    /* Check socket permissions */
    mode_t perms = st.st_mode & 0777;
    if (perms != 0666) {
        LOG_WARN("Socket has unexpected permissions %o (expected 666)", perms);
    }
    
    app->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (app->socket_fd < 0) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    GTK_MESSAGE_ERROR,
                                                    GTK_BUTTONS_OK,
                                                    _("Failed to create socket: %s"),
                                                    strerror(errno));
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(app->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    GTK_MESSAGE_ERROR,
                                                    GTK_BUTTONS_OK,
                                                    _("Failed to connect to daemon: %s"),
                                                    strerror(errno));
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        close(app->socket_fd);
        app->socket_fd = -1;
        return;
    }
    
    app->connected = 1;
    g_print("[GUI] Connected to daemon successfully!\n");
}

/* Disconnect from daemon */
static void disconnect_from_daemon(AppData *app) {
    if (app->socket_fd >= 0) {
        close(app->socket_fd);
        app->socket_fd = -1;
        app->connected = 0;
    }
}

/* Connect to CPU daemon */
static void connect_to_cpu_daemon(AppData *app) {
    if (dcc_component_disabled(COMP_CPU)) {          /* switched off in Settings */
        app->cpu_socket_fd = -1;
        app->cpu_connected = 0;
        return;
    }
    /* Check if socket file exists first */
    struct stat st;
    if (stat(CPU_SOCKET_PATH, &st) != 0) {
        g_print("[GUI] CPU daemon socket not found at %s. CPU tab will be unavailable.\n", CPU_SOCKET_PATH);
        app->cpu_socket_fd = -1;
        app->cpu_connected = 0;
        return;
    }

    app->cpu_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (app->cpu_socket_fd < 0) {
        g_print("[GUI] Failed to create CPU socket: %s\n", strerror(errno));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CPU_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(app->cpu_socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        g_print("[GUI] Failed to connect to CPU daemon: %s\n", strerror(errno));
        close(app->cpu_socket_fd);
        app->cpu_socket_fd = -1;
        return;
    }

    app->cpu_connected = 1;
    update_daemon_status_labels(app);
    g_print("[GUI] Connected to CPU daemon successfully!\n");
}

/* Disconnect from CPU daemon */
static void disconnect_from_cpu_daemon(AppData *app) {
    if (app->cpu_socket_fd >= 0) {
        close(app->cpu_socket_fd);
        app->cpu_socket_fd = -1;
        app->cpu_connected = 0;
    }
}

/* Connect to keyboard daemon */
static void connect_to_keyboard_daemon(AppData *app) {
    if (dcc_component_disabled(COMP_KEYBOARD)) {     /* switched off in Settings */
        app->kbd_socket_fd = -1;
        app->kbd_connected = 0;
        update_daemon_status_labels(app);
        return;
    }
    struct stat st;
    if (stat(KBD_SOCKET_PATH, &st) != 0) {
        g_print("[GUI] Keyboard daemon socket not found at %s. Keyboard tab will be unavailable.\n", KBD_SOCKET_PATH);
        app->kbd_socket_fd = -1;
        app->kbd_connected = 0;
        update_daemon_status_labels(app);
        return;
    }

    app->kbd_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (app->kbd_socket_fd < 0) {
        g_print("[GUI] Failed to create keyboard socket: %s\n", strerror(errno));
        update_daemon_status_labels(app);
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, KBD_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(app->kbd_socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        g_print("[GUI] Failed to connect to keyboard daemon: %s\n", strerror(errno));
        close(app->kbd_socket_fd);
        app->kbd_socket_fd = -1;
        update_daemon_status_labels(app);
        return;
    }

    app->kbd_connected = 1;
    update_daemon_status_labels(app);
    g_print("[GUI] Connected to keyboard daemon successfully!\n");

    /* The daemon restores the backlight itself (off, profile colors or
     * white); the GUI only shows that state instead of switching it on */
    kbd_sync_power_switch(app);
    
    /* Load LED mapping from CSV */
    kbd_load_led_mapping_csv(app);
    kbd_refresh_key_colors(app);
    if (app->tray_menu) update_tray_menu(app);   /* Keyboard Profiles submenu */
}


/* Send command to keyboard daemon (one attempt, see send_keyboard_command) */
static const char *send_keyboard_command_once(AppData *app, const char *cmd) {
    static char response[8192];

    if (!app->kbd_connected || app->kbd_socket_fd < 0) {
        strcpy(response, "ERROR Not connected to keyboard daemon");
        return response;
    }

    gchar *cmd_with_newline = g_strdup_printf("%s\n", cmd);
    ssize_t sent = send(app->kbd_socket_fd, cmd_with_newline, strlen(cmd_with_newline), MSG_NOSIGNAL);  /* EPIPE instead of SIGPIPE */
    g_free(cmd_with_newline);
    if (sent < 0) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] KBD ERROR: Failed to send command (%s), marking disconnected\n", ts, strerror(errno));
        app->kbd_connected = 0;
        if (app->kbd_socket_fd >= 0) {
            close(app->kbd_socket_fd);
            app->kbd_socket_fd = -1;
        }
        strcpy(response, "ERROR Failed to send keyboard command");
        return response;
    }

    memset(response, 0, sizeof(response));
    size_t total_received = 0;
    while (total_received < sizeof(response) - 1) {
        struct pollfd pfd;
        pfd.fd = app->kbd_socket_fd;
        pfd.events = POLLIN;
        /* stop at the reply's NUL end marker (see send_cpu_command) */
        int poll_ret = poll(&pfd, 1, total_received == 0 ? 3000 : 200);
        if (poll_ret < 0) {
            char ts[32];
            get_timestamp(ts, sizeof(ts));
            g_print("%s [GUI] KBD ERROR: poll failed (%s), marking disconnected\n", ts, strerror(errno));
            app->kbd_connected = 0;
            if (app->kbd_socket_fd >= 0) {
                close(app->kbd_socket_fd);
                app->kbd_socket_fd = -1;
            }
            strcpy(response, "ERROR Failed to receive keyboard response");
            return response;
        }
        if (poll_ret == 0) break;

        ssize_t received = read(app->kbd_socket_fd, response + total_received, sizeof(response) - 1 - total_received);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            char ts[32];
            get_timestamp(ts, sizeof(ts));
            g_print("%s [GUI] KBD ERROR: Failed to receive response (%s), marking disconnected\n", ts, strerror(errno));
            app->kbd_connected = 0;
            if (app->kbd_socket_fd >= 0) {
                close(app->kbd_socket_fd);
                app->kbd_socket_fd = -1;
            }
            strcpy(response, "ERROR Failed to receive keyboard response");
            return response;
        }
        if (received == 0) {
            char ts[32];
            get_timestamp(ts, sizeof(ts));
            g_print("%s [GUI] KBD ERROR: Connection closed by daemon (EOF)\n", ts);
            strcpy(response, "ERROR Keyboard connection closed");
            app->kbd_connected = 0;
            return response;
        }
        int done = memchr(response + total_received, '\0', (size_t)received) != NULL;
        total_received += received;
        if (done) break;   /* end marker */
    }

    return response;
}

/* Send a command to the keyboard daemon. If the connection is gone (e.g. the
 * daemon was restarted or updated), reconnect and retry once - before 1.18.6
 * the GUI stayed disconnected and silently skipped keyboard actions. */
/* (Re)connect to the keyboard daemon if needed. Returns 1 if connected. */
static int kbd_ensure_connected(AppData *app) {
    if (app->kbd_connected && app->kbd_socket_fd >= 0) return 1;
    if (app->kbd_socket_fd >= 0) {      /* stale socket left by a lost connection */
        close(app->kbd_socket_fd);
        app->kbd_socket_fd = -1;
    }
    app->kbd_connected = 0;
    connect_to_keyboard_daemon(app);
    return app->kbd_connected;
}

static const char *send_keyboard_command(AppData *app, const char *cmd) {
    if (!kbd_ensure_connected(app)) return send_keyboard_command_once(app, cmd);
    const char *r = send_keyboard_command_once(app, cmd);
    if (!app->kbd_connected) {          /* the send/receive found a dead connection */
        g_print("[GUI] Keyboard daemon connection lost - reconnecting\n");
        if (kbd_ensure_connected(app)) r = send_keyboard_command_once(app, cmd);
    }
    return r;
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
        if (sscanf(buf, "BASE color=%u,%u,%u", &r, &g, &b) == 3) {
            e->has_base = 1;
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
    if (a->has_base && memcmp(a->base, b->base, 3) != 0) return 0;
    for (int z = 0; z < a->num_zones; z++) {
        const KbdZoneEdit *x = &a->zones[z], *y = &b->zones[z];
        if (strcmp(x->name, y->name) != 0 || memcmp(x->rgb, y->rgb, 3) != 0 || x->n != y->n ||
            memcmp(x->leds, y->leds, sizeof(int) * (size_t)x->n) != 0)
            return 0;
    }
    return 1;
}

/* "base::zones" for PREVIEW (base "r,g,b" or "-", zones "name|r,g,b|l,l;...") */
static void kbd_edit_encode(const KbdProfileEdit *e, GString *out) {
    if (e->has_base) g_string_append_printf(out, "%u,%u,%u", e->base[0], e->base[1], e->base[2]);
    else g_string_append(out, "-");
    g_string_append(out, "::");
    for (int z = 0; z < e->num_zones; z++) {
        const KbdZoneEdit *zone = &e->zones[z];
        g_string_append_printf(out, "%s%s|%u,%u,%u|", z ? ";" : "", zone->name,
                               zone->rgb[0], zone->rgb[1], zone->rgb[2]);
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
    g_print("[GUI] Keyboard: %s\n", msg);
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
        g_print("[GUI] Could not read keyboard profile '%s': %s\n", name, r ? r : "NULL");
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
        g_print("[GUI] Keyboard backlight turned OFF (override active)\n");
    } else {
        /* Turn the backlight back on (the daemon restores the current colors) */
        app->kbd_off_active = 0;
        const char *response = send_keyboard_command(app, "TURN_ON");
        g_print("[GUI] TURN_ON response: %s\n", response ? response : "NULL");
        gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label), _("Backlight turned ON"));
        g_print("[GUI] Keyboard backlight turned ON\n");
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
    g_print("[GUI] LED mapping saved for layout '%s': %s\n", kbd_layout_code, csv_path);
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
        g_print("[GUI] LED mapping loaded from %s\n", csv_path);
        return;
    }
    if (!strcmp(kbd_layout_code, "de")) {
        char old_path[512];
        const char *home = getenv("HOME");
        snprintf(old_path, sizeof old_path, "%s/.config/dudes-control-center/keyboard_led_mapping.csv", home ? home : "/tmp");
        if (kbd_read_mapping_file(app, old_path, 1) == 0) {
            g_print("[GUI] LED mapping converted from %s to the German layout file\n", old_path);
            kbd_save_led_mapping_csv(app);       /* the old file stays for older versions */
            return;
        }
    }
    g_print("[GUI] No LED mapping for layout '%s' (%s), using the built-in mapping\n", kbd_layout_code, csv_path);
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

/* Show the daemon's real power state on the "Backlight Off" switch
 * (without sending anything back) */
static void kbd_sync_power_switch(AppData *app) {
    if (!app->kbd_off_switch || !kbd_ensure_connected(app)) return;
    const char *status = send_keyboard_command(app, "STATUS");
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

/* Parse STATUS response and update UI */
static void parse_status(AppData *app, const char *response) {
    char buffer[BUFFER_SIZE];
    strncpy(buffer, response, BUFFER_SIZE - 1);
    buffer[BUFFER_SIZE - 1] = '\0';
    
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    
    /* Parse response and update labels */
    int cpu_temp = 0, gpu_temp = 0, cpu_fan_rpm = 0, gpu_fan_rpm = 0;
    int auto_mode = 1, fan_duty = 0, emergency_auto = -1;
    int cpu_fan_duty = -1, gpu_fan_duty = -1;   /* EC duty per fan (daemons before 1.19.2: missing) */
    
    char *token = strtok(buffer, "\n");
    while (token != NULL) {
        if (strncmp(token, "CPU_TEMP=", 9) == 0) {
            cpu_temp = atoi(token + 9);
        } else if (strncmp(token, "GPU_TEMP=", 9) == 0) {
            gpu_temp = atoi(token + 9);
        } else if (strncmp(token, "CPU_FAN_RPM=", 12) == 0) {
            cpu_fan_rpm = atoi(token + 12);
        } else if (strncmp(token, "GPU_FAN_RPM=", 12) == 0) {
            gpu_fan_rpm = atoi(token + 12);
        } else if (strncmp(token, "AUTO_MODE=", 10) == 0) {
            auto_mode = atoi(token + 10);
        } else if (strncmp(token, "FAN_DUTY=", 9) == 0) {
            fan_duty = atoi(token + 9);
            app->last_known_fan_duty = fan_duty; /* Cache for manual mode */
        } else if (strncmp(token, "CPU_FAN_DUTY=", 13) == 0) {
            cpu_fan_duty = atoi(token + 13);
        } else if (strncmp(token, "GPU_FAN_DUTY=", 13) == 0) {
            gpu_fan_duty = atoi(token + 13);
        } else if (strncmp(token, "EMERGENCY_AUTO=", 15) == 0) {
            emergency_auto = atoi(token + 15);
        }
        token = strtok(NULL, "\n");
    }
    
    /* Update labels */
    char cpu_text[32], gpu_text[32], cpu_rpm_text[64], gpu_rpm_text[64];
    snprintf(cpu_text, sizeof(cpu_text), _("CPU: %d°C"), cpu_temp);
    snprintf(gpu_text, sizeof(gpu_text), _("GPU: %d°C"), gpu_temp);
    /* Percent = duty set in the EC for each fan; RPM = measured speed */
    format_fan_text(cpu_rpm_text, sizeof(cpu_rpm_text), _("CPU Fan"), cpu_fan_rpm, cpu_fan_duty);
    format_fan_text(gpu_rpm_text, sizeof(gpu_rpm_text), _("GPU Fan"), gpu_fan_rpm, gpu_fan_duty);
    
    gtk_label_set_text(GTK_LABEL(app->cpu_temp_label), cpu_text);
    gtk_label_set_text(GTK_LABEL(app->gpu_temp_label), gpu_text);
    gtk_label_set_text(GTK_LABEL(app->cpu_fan_rpm_label), cpu_rpm_text);
    gtk_label_set_text(GTK_LABEL(app->gpu_fan_rpm_label), gpu_rpm_text);
    
    /* Sync manual mode switch state with daemon */
    if (app->manual_mode_switch != NULL && gtk_widget_get_realized(app->manual_mode_switch)) {
        gboolean current_switch_state = gtk_switch_get_active(GTK_SWITCH(app->manual_mode_switch));
        gboolean should_be_manual = (auto_mode == 0);
        
        g_print("%s [GUI] parse_status: AUTO_MODE=%d, current_switch_state=%d, should_be_manual=%d, app->in_manual_mode=%d, app->auto_mode_enabled=%d\n", 
                ts, auto_mode, current_switch_state, should_be_manual, app->in_manual_mode, app->auto_mode_enabled);
        
        if (current_switch_state != should_be_manual) {
            g_print("%s [GUI] parse_status: SYNCING switch from %d to %d\n", ts, current_switch_state, should_be_manual);
            g_signal_handler_block(app->manual_mode_switch, app->manual_mode_signal_id);
            gtk_switch_set_state(GTK_SWITCH(app->manual_mode_switch), should_be_manual);
            g_signal_handler_unblock(app->manual_mode_switch, app->manual_mode_signal_id);
        }
    }
    
    /* Update +/- buttons and label based on mode */
    if (app->auto_mode_enabled == 0) { /* Manual mode */
        /* In manual mode, do NOT update from daemon - preserve user's manual setting */
        /* The local app->manual_current_speed is already correct from button clicks */
    } else { /* Auto mode */
        /* In auto mode, show "--" and buttons are disabled (handled in switch callback) */
    }
    
    /* The daemon ended manual mode after an emergency: follow it and say why */
    int ended_by_emergency = emergency_auto >= 0 && app->emergency_auto_seen >= 0 &&
                             emergency_auto > app->emergency_auto_seen;
    if (emergency_auto >= 0) app->emergency_auto_seen = emergency_auto;
    if (auto_mode && !app->auto_mode_enabled) {
        app->auto_mode_enabled = 1;
        if (app->manual_speed_label) gtk_label_set_text(GTK_LABEL(app->manual_speed_label), "--");
        if (ended_by_emergency) {
            g_print("[GUI] Fan daemon switched to auto mode after an emergency temperature\n");
            /* xgettext:no-c-format */
            show_status(app, _("Manual fan mode ended: the CPU/GPU reached 95\302\260C - fans at 100% until it cools down, then the fan curve (auto mode)"));
        }
    }

    /* Update internal manual mode tracking */
    int was_in_manual_mode = app->in_manual_mode;
    app->in_manual_mode = (auto_mode == 0);
    
    /* If manual mode state changed, update the profile tab UI */
    if (was_in_manual_mode != app->in_manual_mode) {
        g_print("[GUI] Manual mode state changed: %s -> %s\n", 
                was_in_manual_mode ? "ENABLED" : "DISABLED",
                app->in_manual_mode ? "ENABLED" : "DISABLED");
        update_profile_tab_for_manual_mode(app, app->in_manual_mode);
    }
    
    /* Update button sensitivity based on mode */
    if (app->manual_minus_btn != NULL && app->manual_plus_btn != NULL) {
        gboolean should_be_manual = (auto_mode == 0);
        gtk_widget_set_sensitive(app->manual_minus_btn, should_be_manual);
        gtk_widget_set_sensitive(app->manual_plus_btn, should_be_manual);
    }
}

/* Create Page 1: Status & Manual Control */
static GtkWidget *create_status_page(AppData *app) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 20);
    
    /* Temperature display */
    GtkWidget *temp_frame = gtk_frame_new(_("Temperatures"));
    GtkWidget *temp_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(temp_vbox), 15);
    
    app->cpu_temp_label = gtk_label_new(_("CPU: --°C"));
    gtk_widget_set_halign(app->cpu_temp_label, GTK_ALIGN_START);
    GtkWidget *gpu_temp_label_holder = gtk_label_new(_("GPU: --°C"));
    gtk_widget_set_halign(gpu_temp_label_holder, GTK_ALIGN_START);
    
    app->cpu_temp_label = gtk_label_new(_("CPU: --°C"));
    gtk_widget_set_halign(app->cpu_temp_label, GTK_ALIGN_START);
    app->gpu_temp_label = gtk_label_new(_("GPU: --°C"));
    gtk_widget_set_halign(app->gpu_temp_label, GTK_ALIGN_START);
    
    gtk_box_pack_start(GTK_BOX(temp_vbox), app->cpu_temp_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(temp_vbox), app->gpu_temp_label, FALSE, FALSE, 0);
    
    gtk_container_add(GTK_CONTAINER(temp_frame), temp_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), temp_frame, FALSE, FALSE, 0);
    
    /* Fan speed display */
    GtkWidget *fan_frame = gtk_frame_new(_("Fan Speed"));
    GtkWidget *fan_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(fan_vbox), 15);
    
    app->cpu_fan_rpm_label = gtk_label_new(_("CPU Fan: -- RPM (--%)"));
    gtk_widget_set_halign(app->cpu_fan_rpm_label, GTK_ALIGN_START);
    app->gpu_fan_rpm_label = gtk_label_new(_("GPU Fan: -- RPM (--%)"));
    gtk_widget_set_halign(app->gpu_fan_rpm_label, GTK_ALIGN_START);
    
    gtk_box_pack_start(GTK_BOX(fan_vbox), app->cpu_fan_rpm_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fan_vbox), app->gpu_fan_rpm_label, FALSE, FALSE, 0);
    
    gtk_container_add(GTK_CONTAINER(fan_frame), fan_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), fan_frame, FALSE, FALSE, 0);
    
    /* Manual control */
    GtkWidget *manual_frame = gtk_frame_new(_("Manual Fan Control"));
    GtkWidget *manual_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_container_set_border_width(GTK_CONTAINER(manual_vbox), 15);
    
    /* Manual mode switch */
    GtkWidget *switch_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    app->manual_mode_switch = GTK_WIDGET(gtk_switch_new());
    GtkWidget *switch_label = gtk_label_new(_("Manual Mode:"));
    gtk_box_pack_start(GTK_BOX(switch_hbox), switch_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(switch_hbox), app->manual_mode_switch, FALSE, FALSE, 0);
    
    /* Connect the toggle signal and store the handler ID */
    app->manual_mode_signal_id = g_signal_connect(app->manual_mode_switch, "state-set", G_CALLBACK(on_manual_mode_switched), app);
    
    gtk_box_pack_start(GTK_BOX(manual_vbox), switch_hbox, FALSE, FALSE, 0);
    
    /* Speed control with +/- buttons (disabled by default) */
    GtkWidget *speed_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    
    /* Minus button */
    app->manual_minus_btn = gtk_button_new_with_label("-");
    gtk_widget_set_size_request(app->manual_minus_btn, 50, -1);
    gtk_widget_set_sensitive(app->manual_minus_btn, FALSE); /* Disabled until manual mode is enabled */
    g_signal_connect(app->manual_minus_btn, "clicked", G_CALLBACK(on_manual_speed_minus_clicked), app);
    
    /* Speed display label */
    app->manual_speed_label = gtk_label_new("--");
    GtkWidget *speed_display_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_size_request(app->manual_speed_label, 60, -1);
    gtk_label_set_justify(GTK_LABEL(app->manual_speed_label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(app->manual_speed_label, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(speed_display_box), app->manual_speed_label, FALSE, FALSE, 0);
    GtkWidget *speed_pct_label = gtk_label_new("%");
    gtk_box_pack_start(GTK_BOX(speed_display_box), speed_pct_label, FALSE, FALSE, 0);
    
    /* Plus button */
    app->manual_plus_btn = gtk_button_new_with_label("+");
    gtk_widget_set_size_request(app->manual_plus_btn, 50, -1);
    gtk_widget_set_sensitive(app->manual_plus_btn, FALSE); /* Disabled until manual mode is enabled */
    g_signal_connect(app->manual_plus_btn, "clicked", G_CALLBACK(on_manual_speed_plus_clicked), app);
    
    gtk_box_pack_start(GTK_BOX(speed_hbox), app->manual_minus_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(speed_hbox), speed_display_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(speed_hbox), app->manual_plus_btn, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(manual_vbox), speed_hbox, FALSE, FALSE, 0);
    
    /* Info label */
    GtkWidget *info_label = gtk_label_new(_("Note: Enable Manual Mode to override automatic fan control. Changes take effect immediately."));
    gtk_label_set_line_wrap(GTK_LABEL(info_label), TRUE);
    gtk_box_pack_start(GTK_BOX(manual_vbox), info_label, FALSE, FALSE, 0);
    
    gtk_container_add(GTK_CONTAINER(manual_frame), manual_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), manual_frame, FALSE, FALSE, 0);
    
    /* Per-Core CPU Frequencies */
    {
        GtkWidget *core_frame = gtk_frame_new(_("Per-Core CPU Frequencies"));
        GtkWidget *core_scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(core_scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(core_scrolled), 400);
        gtk_container_set_border_width(GTK_CONTAINER(core_scrolled), 5);
        gtk_container_add(GTK_CONTAINER(core_frame), core_scrolled);
        /* Per-core frequencies and Active Profiles side by side, same width (1.30.0) */
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
        gtk_box_pack_start(GTK_BOX(row), core_frame, TRUE, TRUE, 0);
        GtkWidget *prof_frame = gtk_frame_new(_("Active Profiles"));
        GtkWidget *grid = gtk_grid_new();
        gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        const char *names[4] = { _("Fan profile:"), _("CPU profile:"), _("Power mode:"), _("Keyboard profile:") };
        GtkWidget **vals[4] = { &app->active_fan_label, &app->active_cpu_label, &app->active_mode_label, &app->active_kbd_label };
        for (int r = 0; r < 4; r++) {
            GtkWidget *n = gtk_label_new(names[r]);
            gtk_label_set_xalign(GTK_LABEL(n), 0.0);
            gtk_grid_attach(GTK_GRID(grid), n, 0, r, 1, 1);
            *vals[r] = gtk_label_new("--");
            gtk_label_set_xalign(GTK_LABEL(*vals[r]), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(*vals[r]), PANGO_ELLIPSIZE_END);
            gtk_widget_set_hexpand(*vals[r], TRUE);
            gtk_grid_attach(GTK_GRID(grid), *vals[r], 1, r, 1, 1);
        }
        gtk_container_add(GTK_CONTAINER(prof_frame), grid);
        gtk_box_pack_start(GTK_BOX(row), prof_frame, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), row, TRUE, TRUE, 0);
        
        /* 4-column grid for compact display */
        app->cpu_core_grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(app->cpu_core_grid), 2);
        gtk_grid_set_column_spacing(GTK_GRID(app->cpu_core_grid), 10);
        gtk_container_add(GTK_CONTAINER(core_scrolled), app->cpu_core_grid);
        
        /* Initialize labels to NULL */
        app->cpu_core_count = 0;
        for (int i = 0; i < 32; i++) {
            app->cpu_core_freq_labels[i] = NULL;
        }
    }
    
    return vbox;
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
            g_print("%s [GUI] Re-activating profile %d\n", ts, app->profile_before_manual);
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

/* Callback when notebook page is switched */
static void on_notebook_page_switched(GtkNotebook *notebook, GtkWidget *page, gint page_num, gpointer user_data) {
    (void)notebook;
    (void)page;
    AppData *app = (AppData *)user_data;
    
    /* Prevent re-entry */
    if (app->switching_page) {
        return;
    }
    
    /* Check if we're leaving the Fan Profiles tab with unsaved curve changes */
    gint current_page = gtk_notebook_get_current_page(notebook);
    if (current_page == PAGE_FAN && page_num != PAGE_FAN && app->curve_points_modified) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Leaving Curve Editor tab with unsaved changes\n", ts);
        
        int result = check_unsaved_changes(app);
        if (result == 1) {
            /* User chose to apply changes - call on_apply_clicked logic */
            on_apply_clicked(NULL, app);
        } else if (result == 0) {
            /* User chose to discard changes - reload curve points and clear flag */
            load_curve_points_for_profile(app, app->active_profile_idx);
            app->curve_points_modified = 0;
        } else {
            /* User clicked Cancel - abort tab switch, keep curve points as-is */
            g_print("%s [GUI] -> Tab switch cancelled by user\n", ts);
            
            /* Stop the page switch immediately by forcing back to current page */
            gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), current_page);
            
            /* Stop further emission of this signal to prevent any side effects */
            g_signal_stop_emission_by_name(G_OBJECT(notebook), "switch-page");
            
            g_print("%s [GUI] -> Page switch cancelled, staying on page %d\n", ts, current_page);
            
            /* Return early to prevent further execution of this callback */
            return;
        }
    }
    
    /* Leaving the Keyboard tab: unsaved changes, then the active profile again (1.24.0) */
    if (current_page == PAGE_KEYBOARD && page_num != PAGE_KEYBOARD) {
        if (kbd_leave_tab(app) < 0) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), current_page);
            g_signal_stop_emission_by_name(G_OBJECT(notebook), "switch-page");
            return;
        }
    }

    /* Leaving the CPU tab with unsaved profile changes (1.23.0) */
    if (current_page == PAGE_CPU && page_num != PAGE_CPU && cpu_profile_has_unsaved_changes(app)) {
        if (cpu_check_unsaved_changes(app) < 0) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), current_page);
            g_signal_stop_emission_by_name(G_OBJECT(notebook), "switch-page");
            return;
        }
    }

    /* Clear status messages when switching tabs */
    if (current_page != page_num) {
        show_status(app, "");
        
        /* Clear CPU profile status label when leaving the CPU Profiles tab */
        if (current_page == PAGE_CPU && app->cpu_freq_profile_status_label) {
            gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "");
        }
    }

    /* Fan Profiles tab: refresh the list (keeps the selected profile) and
     * show the selected profile's curve */
    if (page_num == PAGE_FAN) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Switching to Fan Profiles tab\n", ts);
        app->pending_active_profile_idx = app->active_profile_idx;
        refresh_profile_list(app);
        int sel = gtk_combo_box_get_active(GTK_COMBO_BOX(app->profile_combo));
        load_curve_points_for_profile(app, sel >= 0 ? sel : app->active_profile_idx);
        update_profile_tab_for_manual_mode(app, app->in_manual_mode);
    }

    /* Page 3 is the CPU Profiles tab (0-indexed: Status=0, Curve Editor=1, Profiles=2, CPU Profiles=3) */
    if (page_num == PAGE_CPU) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Switching to CPU Profiles tab\n", ts);


        /* Reload CPU custom profiles; keep the profile the user had selected
         * (the active one on the first visit) */
        load_cpu_custom_profiles_select(app,
            gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo)));

        /* Update tray menu so CPU profiles appear in the context menu.
         * The tray menu is created at startup when cpu_custom_profile_store is empty.
         * Rebuilding it after profiles load ensures the submenu is populated. */
        update_tray_menu(app);

        /* Defer grid population until GTK has realized the widgets on this page.
         * GTK3 lazy-realizes notebook pages: the page is only realized after
         * the first switch-page signal fires. By using g_idle_add, we ensure
         * the widgets are fully realized before building the grid.
         * This fixes the bug where the core frequency grid is empty on first visit. */
        g_idle_add(populate_cpu_core_grid_idle, app);
    }

    /* Page 4 is the Keyboard Backlight tab (0-indexed: Status=0, Curve Editor=1, Profiles=2, CPU Profiles=3, Keyboard=4) */
    if (page_num == PAGE_KEYBOARD) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Switching to Keyboard Backlight tab\n", ts);

        /* Edit the active profile */
        kbd_enter_tab(app);
        
        /* Redraw canvas to show loaded LED mapping */
        if (app->kbd_keyboard_canvas) {
            gtk_widget_queue_draw(app->kbd_keyboard_canvas);
        }
    }
}


/* Callback for clickable links - destroys the duplicated URL string */
/* The URL belongs to the link widget and is freed when the widget is
 * destroyed (g_signal_connect_data below). It used to be freed here on every
 * left click, so a second click used freed memory and crashed the GUI
 * (double free). Only a single left press opens the link (no second browser
 * tab from the double-click event GTK also sends). */
static gboolean on_link_clicked(GtkWidget *widget __attribute__((unused)), GdkEventButton *event, gpointer user_data) {
    if (event->button != 1 || event->type != GDK_BUTTON_PRESS) return FALSE;
    const gchar *url = (const gchar *)user_data;
    GError *error = NULL;
    if (!g_app_info_launch_default_for_uri(url, NULL, &error) && error) {
        g_printerr("[GUI] Failed to open URI: %s\n", error->message);
        g_error_free(error);
    }
    return TRUE; /* Event handled */
}

/* Helper function to create clickable link label */
static GtkWidget *create_clickable_link(const char *url) {
    /* Create the label with the URL text */
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_strdup_printf("<span size='small' foreground='#3498db'><u>%s</u></span>", url);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    
    /* Wrap label in an EventBox to reliably capture mouse events */
    GtkWidget *event_box = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(event_box), label);
    gtk_container_set_border_width(GTK_CONTAINER(event_box), 2);
    gtk_widget_set_halign(event_box, GTK_ALIGN_CENTER);
    
    /* Connect the button-press-event to the event box */
    g_signal_connect_data(event_box, "button-press-event", G_CALLBACK(on_link_clicked),
                          g_strdup(url), (GClosureNotify)(void (*)(void))g_free, 0);
    
    return event_box;
}

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
    
    /* Tab 4: Keyboard Backlight Log */
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
        
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled, gtk_label_new(_("Keyboard Backlight Log")));
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
    gtk_text_buffer_insert(app->gui_log_buffer, &start_iter, reversed->str, -1);
    
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
    gtk_text_buffer_insert(app->daemon_log_buffer, &start_iter, reversed->str, -1);
    
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
    gtk_text_buffer_insert(app->cpu_daemon_log_buffer, &start_iter, reversed->str, -1);
    
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
    gtk_text_buffer_insert(app->kbd_daemon_log_buffer, &start_iter, reversed->str, -1);
    
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

#ifndef DCC_LOGO_PATH
#define DCC_LOGO_PATH "/usr/share/dudes-control-center/logo.png"
#endif

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
    GtkWidget *logo = NULL;
    const char *logo_paths[] = { DCC_LOGO_PATH, "/usr/share/pixmaps/DudesControlCenter.png" };
    for (int i = 0; i < 2 && !logo; i++) {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(logo_paths[i], 230, 230, TRUE, NULL);
        if (pb) {
            logo = gtk_image_new_from_pixbuf(pb);
            g_object_unref(pb);
        }
    }
    if (!logo) logo = gtk_label_new("");
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

/* Create Page 6: CPU Control */
static GtkWidget *create_cpu_page(AppData *app) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    /* 12 px at the top like the Keyboard tab, so the daemon status lines of
     * all tabs are at the same height; 20 px elsewhere as before */
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 20);
    gtk_widget_set_margin_start(vbox, 20);
    gtk_widget_set_margin_end(vbox, 20);

    /* Daemon status (like the Keyboard tab) */
    {
        GtkWidget *frame = gtk_frame_new(_("CPU Daemon Status"));
        gtk_container_set_border_width(GTK_CONTAINER(frame), 6);
        app->cpu_daemon_status_label = gtk_label_new(_("CPU daemon not connected"));
        gtk_label_set_xalign(GTK_LABEL(app->cpu_daemon_status_label), 0.0);
        gtk_widget_set_margin_start(app->cpu_daemon_status_label, 6);
        gtk_widget_set_margin_end(app->cpu_daemon_status_label, 6);
        gtk_container_add(GTK_CONTAINER(frame), app->cpu_daemon_status_label);
        gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    }

    /* Section: CPU Info */
    GtkWidget *info_frame = gtk_frame_new(_("CPU Information"));
    GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(info_box), 10);
    gtk_container_add(GTK_CONTAINER(info_frame), info_box);
    gtk_box_pack_start(GTK_BOX(vbox), info_frame, FALSE, FALSE, 0);
    /* Two columns (1.24.4): temperature, load, CPUs left; frequencies and
     * power mode right - half the height of one long list */
    GtkWidget *info_cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_box_set_homogeneous(GTK_BOX(info_cols), TRUE);
    GtkWidget *info_left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *info_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_pack_start(GTK_BOX(info_cols), info_left, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(info_cols), info_right, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(info_box), info_cols, FALSE, FALSE, 0);

    /* CPU Temperature */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("CPU Temperature:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_temp_label = gtk_label_new(_("N/A"));
        gtk_widget_set_halign(app->cpu_page_temp_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_temp_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_left), row, FALSE, FALSE, 0);
    }

    /* CPU Load */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("CPU Load:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_load_label = gtk_label_new(_("N/A"));
        gtk_widget_set_halign(app->cpu_page_load_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_load_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_left), row, FALSE, FALSE, 0);
    }

    /* Number of CPUs */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("Number of CPUs:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_num_label = gtk_label_new(_("N/A"));
        gtk_widget_set_halign(app->cpu_page_num_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_num_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_left), row, FALSE, FALSE, 0);
    }

    /* Current Frequency */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("Current Frequency:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_freq_label = gtk_label_new(_("N/A"));
        gtk_widget_set_halign(app->cpu_page_freq_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_freq_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_right), row, FALSE, FALSE, 0);
    }

    /* Min/Max Frequency */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("Min / Max Frequency:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_min_max_freq_label = gtk_label_new(_("N/A"));
        gtk_widget_set_halign(app->cpu_page_min_max_freq_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_min_max_freq_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_right), row, FALSE, FALSE, 0);
    }

    /* Active Governor */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("Active Power Mode:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_governor_label = gtk_label_new(_("N/A"));
        gtk_widget_set_halign(app->cpu_page_governor_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_governor_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_right), row, FALSE, FALSE, 0);
    }

    /* Section: Governor Profiles */
    GtkWidget *profile_frame = gtk_frame_new(_("Governor Profiles"));
    GtkWidget *profile_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(profile_box), 10);
    gtk_container_add(GTK_CONTAINER(profile_frame), profile_box);
    gtk_box_pack_start(GTK_BOX(vbox), profile_frame, FALSE, FALSE, 0);

    /* Profile combo box */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(_("Power mode:"));
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);

        app->cpu_profile_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
        app->cpu_profile_combo = gtk_combo_box_new();
        gtk_combo_box_set_model(GTK_COMBO_BOX(app->cpu_profile_combo), GTK_TREE_MODEL(app->cpu_profile_store));
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(app->cpu_profile_combo), renderer, TRUE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(app->cpu_profile_combo), renderer, "text", 0, NULL);
        gtk_widget_set_hexpand(app->cpu_profile_combo, TRUE);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_profile_combo, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(profile_box), row, FALSE, FALSE, 0);

        /* Connect combo box changed signal and store handler ID */
        app->cpu_profile_combo_signal_id = g_signal_connect(app->cpu_profile_combo, "changed", G_CALLBACK(on_cpu_governor_changed), app);
    }

    /* Section: Per-Core Frequency Profiles */
    {
        GtkWidget *freq_frame = gtk_frame_new(_("Per-Core Frequency Profiles"));
        GtkWidget *freq_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_container_set_border_width(GTK_CONTAINER(freq_box), 10);
        gtk_container_add(GTK_CONTAINER(freq_frame), freq_box);
        gtk_box_pack_start(GTK_BOX(vbox), freq_frame, FALSE, FALSE, 0);

        /* Profile combo box */
        {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            GtkWidget *label = gtk_label_new(_("Frequency Profile:"));
            gtk_widget_set_halign(label, GTK_ALIGN_END);
            gtk_label_set_width_chars(GTK_LABEL(label), 20);
            gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);

            app->cpu_custom_profile_store = gtk_list_store_new(1, G_TYPE_STRING);
            app->cpu_custom_profile_combo = gtk_combo_box_new();
            gtk_combo_box_set_model(GTK_COMBO_BOX(app->cpu_custom_profile_combo), GTK_TREE_MODEL(app->cpu_custom_profile_store));
            GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
            gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(app->cpu_custom_profile_combo), renderer, TRUE);
            gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(app->cpu_custom_profile_combo), renderer, "text", 0, NULL);
            gtk_widget_set_hexpand(app->cpu_custom_profile_combo, TRUE);
            gtk_box_pack_start(GTK_BOX(row), app->cpu_custom_profile_combo, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(freq_box), row, FALSE, FALSE, 0);

            app->cpu_custom_profile_combo_signal_id = g_signal_connect(app->cpu_custom_profile_combo, "changed", G_CALLBACK(on_cpu_custom_profile_changed), app);
        }

        /* Apply to All Cores checkbox */
        {
            app->cpu_apply_all_cores_checkbox = gtk_check_button_new_with_label(_("Apply values to all cores"));
            gtk_box_pack_start(GTK_BOX(freq_box), app->cpu_apply_all_cores_checkbox, FALSE, FALSE, 0);
            gtk_widget_set_sensitive(app->cpu_apply_all_cores_checkbox, FALSE); /* Disabled until profile selected */
            app->cpu_apply_all_cores_signal_id = g_signal_connect(app->cpu_apply_all_cores_checkbox, "toggled", G_CALLBACK(on_cpu_apply_all_cores_toggled), app);
        }

        /* Note on low maximums (1.34.2): measured on a Core Ultra 9 275HX, the
         * CPU kept 2.2 GHz (P-cores) / 1.5 GHz (E-cores) with work to do
         * however low scaling_max_freq was; higher maximums held exactly. */
        {
            GtkWidget *note = gtk_label_new(NULL);
            gchar *m = g_markup_printf_escaped("<small>%s</small>", _(CPU_MAX_FLOOR_NOTE));
            gtk_label_set_markup(GTK_LABEL(note), m);
            g_free(m);
            gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
            gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
            gtk_label_set_max_width_chars(GTK_LABEL(note), 110);
            gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
            gtk_box_pack_start(GTK_BOX(freq_box), note, FALSE, FALSE, 0);
        }

        /* Scrollable area for per-core min/max spinners */
        {
            GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
            gtk_container_set_border_width(GTK_CONTAINER(scroll), 5);
            gtk_widget_set_size_request(scroll, -1, 200); /* Fixed height for the scroll area */
            gtk_box_pack_start(GTK_BOX(freq_box), scroll, FALSE, FALSE, 0);

            app->cpu_core_freq_grid = gtk_grid_new();
            gtk_container_add(GTK_CONTAINER(scroll), app->cpu_core_freq_grid);
            /* Grid will be populated when a profile is selected */
        }

        /* Create, Rename, Save, Delete, Activate (same order in every tab) */
        {
            GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_box_pack_start(GTK_BOX(freq_box), btn_box, FALSE, FALSE, 0);

            app->cpu_freq_profile_create_btn = gtk_button_new_with_label(_("Create"));
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_create_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_create_btn, "clicked", G_CALLBACK(on_cpu_create_freq_profile_clicked), app);

            app->cpu_freq_profile_rename_btn = gtk_button_new_with_label(_("Rename"));
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_rename_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_rename_btn, "clicked", G_CALLBACK(on_cpu_rename_freq_profile_clicked), app);
            gtk_widget_set_sensitive(app->cpu_freq_profile_rename_btn, FALSE); /* Disabled until profile selected */

            app->cpu_freq_profile_save_btn = gtk_button_new_with_label(_("Save"));
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_save_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_save_btn, "clicked", G_CALLBACK(on_cpu_save_freq_profile_clicked), app);
            gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, FALSE); /* Disabled until profile selected */

            app->cpu_freq_profile_delete_btn = gtk_button_new_with_label(_("Delete"));
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_delete_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_delete_btn, "clicked", G_CALLBACK(on_cpu_delete_freq_profile_clicked), app);
            gtk_widget_set_sensitive(app->cpu_freq_profile_delete_btn, FALSE); /* Disabled until profile selected */

            app->cpu_freq_profile_activate_btn = gtk_button_new_with_label(_("Activate"));
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_activate_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_activate_btn, "clicked", G_CALLBACK(on_cpu_activate_freq_profile_clicked), app);
            gtk_widget_set_sensitive(app->cpu_freq_profile_activate_btn, FALSE); /* Disabled until profile selected */
        }

        /* Status label */
        {
            app->cpu_freq_profile_status_label = gtk_label_new("");
            gtk_widget_set_halign(app->cpu_freq_profile_status_label, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(freq_box), app->cpu_freq_profile_status_label, FALSE, FALSE, 0);
        }
    }

    /* Section: Hard Frequency Limit */
    {
        GtkWidget *limit_frame = gtk_frame_new(_("Hard Frequency Limit"));
        GtkWidget *limit_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_container_set_border_width(GTK_CONTAINER(limit_box), 10);
        gtk_container_add(GTK_CONTAINER(limit_frame), limit_box);
        gtk_box_pack_start(GTK_BOX(vbox), limit_frame, FALSE, FALSE, 0);

        /* Warning label */
        {
            GtkWidget *warning_label = gtk_label_new("");
            gtk_label_set_markup(GTK_LABEL(warning_label),
                _("<span foreground='#e74c3c' weight='bold'>WARNING:</span> "
                "This is a safety limit to protect your hardware. "
                "Setting it too high may cause CPU instability, overheating, or permanent damage. "
                "Only increase this if you know your CPU's safe operating limits."));
            gtk_label_set_line_wrap(GTK_LABEL(warning_label), TRUE);
            gtk_widget_set_hexpand(warning_label, TRUE);
            gtk_box_pack_start(GTK_BOX(limit_box), warning_label, FALSE, FALSE, 0);
        }

        /* Hard limit spinner (in GHz, step 0.1, range 0.5 - 8.0) */
        {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            GtkWidget *label = gtk_label_new(_("Hard Limit (GHz):"));
            gtk_widget_set_halign(label, GTK_ALIGN_END);
            gtk_label_set_width_chars(GTK_LABEL(label), 20);
            gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);

            app->cpu_hard_limit_adjustment = gtk_adjustment_new(3.6, 0.5, 8.0, 0.1, 0.5, 0);
            app->cpu_hard_limit_spinner = gtk_spin_button_new(app->cpu_hard_limit_adjustment, 0.1, 1);
            gtk_widget_set_halign(app->cpu_hard_limit_spinner, GTK_ALIGN_START);
            gtk_widget_set_tooltip_text(app->cpu_hard_limit_spinner, _(CPU_MAX_FLOOR_NOTE));
            gtk_box_pack_start(GTK_BOX(row), app->cpu_hard_limit_spinner, FALSE, FALSE, 0);

            app->cpu_hard_limit_apply_btn = gtk_button_new_with_label(_("Apply"));
            gtk_widget_set_halign(app->cpu_hard_limit_apply_btn, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(row), app->cpu_hard_limit_apply_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_hard_limit_apply_btn, "clicked", G_CALLBACK(on_cpu_hard_limit_apply_clicked), app);

            gtk_box_pack_start(GTK_BOX(limit_box), row, FALSE, FALSE, 0);
        }
    }

    return vbox;
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
    g_print("[GUI] %s -> %s\n", cmd, r ? r : "NULL");
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

    GtkWidget *status_frame = gtk_frame_new(_("Keyboard Daemon Status"));
    gtk_container_set_border_width(GTK_CONTAINER(status_frame), 6);
    gtk_container_add(GTK_CONTAINER(status_frame), status_box);
    gtk_box_pack_start(GTK_BOX(vbox), status_frame, FALSE, FALSE, 0);

    /* --- Profile management section --- */
    GtkWidget *profile_frame = gtk_frame_new(_("Keyboard Profiles"));
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
    GtkWidget *off_switch_frame = gtk_frame_new(_("Backlight"));
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

    GtkWidget *off_switch_note = gtk_label_new(_("When ON, overrides all profile settings until disabled."));
    gtk_widget_set_name(off_switch_note, "kbd-off-switch-note");
    gtk_label_set_xalign(GTK_LABEL(off_switch_note), 0.0);
    gtk_box_pack_start(GTK_BOX(off_switch_vbox), off_switch_note, FALSE, FALSE, 0);

    /* Backlight settings on the left, the LED Wizard on the right (1.24.2:
     * it is the key-to-LED mapping, not part of the zone editor) */
    GtkWidget *backlight_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(backlight_row), off_switch_vbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(backlight_row), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    GtkWidget *wizard_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
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
    GtkWidget *preset_frame = gtk_frame_new(_("Preset Colors"));
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
        GtkWidget *whole_btn = gtk_button_new_with_label(_("Whole Keyboard"));
        gtk_widget_set_tooltip_text(whole_btn, _("Stop editing a zone: colors go to the whole keyboard (base color)"));
        g_signal_connect(whole_btn, "clicked", G_CALLBACK(on_kbd_whole_keyboard_clicked), app);
        gtk_box_pack_end(GTK_BOX(preset_hbox), whole_btn, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(preset_vbox), preset_hbox, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(preset_frame), preset_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), preset_frame, FALSE, FALSE, 0);

    /* --- Visual Zone Editor section --- */
    GtkWidget *zone_frame = gtk_frame_new(_("Visual Zone Editor"));
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

    return vbox;
}

/* Create main window */
static GtkWidget *create_main_window(AppData *app) {
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), _("Dudes Control Center"));
    
    /* Set application icon */
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file("DudesControlCenter.png", NULL);
    if (pixbuf) {
        gtk_window_set_icon(GTK_WINDOW(app->window), pixbuf);
        g_object_unref(pixbuf);
    }
    
    /* Handle window close button - minimize to tray instead of closing */
    g_signal_connect(app->window, "delete-event", G_CALLBACK(on_window_delete_event), app);
    
    /* Handle actual destroy (from Exit menu or programmatic quit) */
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_window_destroy), app);
    
    /* Create notebook with pages */
    app->notebook = gtk_notebook_new();
    
    GtkWidget *page1 = create_status_page(app);
    GtkWidget *page2 = create_curve_editor_page(app);
GtkWidget *page4 = create_log_page(app);
    GtkWidget *cpu_page = create_cpu_page(app);
    /* Wrap CPU page in a scrolled window so all sections are reachable */
    GtkWidget *cpu_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cpu_scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(cpu_scrolled), cpu_page);
    GtkWidget *page6 = create_about_page();
    GtkWidget *help_page = create_help_page();   /* README, Changelog, License (1.34.0) */
    GtkWidget *kbd_page = create_keyboard_page(app);
    /* Wrap keyboard page in a scrolled window */
    GtkWidget *kbd_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(kbd_scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(kbd_scrolled), kbd_page);

    GtkWidget *settings_page = create_settings_page(app);
    app->page_widgets[PAGE_STATUS] = page1;
    app->page_widgets[PAGE_FAN] = page2;
    app->page_widgets[PAGE_CPU] = cpu_scrolled;
    app->page_widgets[PAGE_KEYBOARD] = kbd_scrolled;
    app->page_widgets[PAGE_SETTINGS] = settings_page;
    app->page_widgets[PAGE_LOGS] = page4;
    app->page_widgets[PAGE_HELP] = help_page;
    app->page_widgets[PAGE_ABOUT] = page6;
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page1, gtk_label_new(_("Status")));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page2, gtk_label_new(_("Fan Profiles")));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), cpu_scrolled, gtk_label_new(_("CPU Profiles")));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), kbd_scrolled, gtk_label_new(_("Keyboard Backlight")));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), settings_page, gtk_label_new(_("Settings")));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page4, gtk_label_new(_("Logs")));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), help_page, gtk_label_new(_("Help")));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page6, gtk_label_new(_("About")));
    
    settings_apply_visibility(app);   /* hide the tabs of switched-off components */
    /* desktop theme changes while following the system (1.29.0) */
    g_signal_connect(gtk_settings_get_default(), "notify::gtk-theme-name", G_CALLBACK(on_gtk_theme_changed), app);
    g_signal_connect(gtk_settings_get_default(), "notify::gtk-application-prefer-dark-theme",
                     G_CALLBACK(on_gtk_theme_changed), app);

    /* Connect notebook page-switch signal to update profile tab when switching to it */
    app->page_switched_handler_id = g_signal_connect(app->notebook, "switch-page", G_CALLBACK(on_notebook_page_switched), app);
    
    /* Create statusbar */
    app->statusbar = gtk_statusbar_new();
    
    /* Create main vbox */
    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(main_vbox), app->notebook, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(main_vbox), app->statusbar, FALSE, FALSE, 0);
    
    gtk_container_add(GTK_CONTAINER(app->window), main_vbox);
    
    return app->window;
}

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
        g_print("[GUI] Settings: %s control %s\n", name, job->enable ? "switched on" : "switched off");
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
        g_print("[GUI] Settings: cannot run pkexec: %s\n", err ? err->message : "?");
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
    g_print("[GUI] Language set to '%s' (after a restart)\n", code);
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
    g_print("[GUI] Restarting for the new language\n");
    disconnect_from_daemon(app);
    disconnect_from_cpu_daemon(app);
    if (app->kbd_socket_fd >= 0) { close(app->kbd_socket_fd); app->kbd_socket_fd = -1; }
    execv("/proc/self/exe", gui_argv);
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

static GtkWidget *create_settings_page(AppData *app) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

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
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);

    /* Language (1.31.0) */
    {
        GtkWidget *lframe2 = gtk_frame_new(_("Language"));
        GtkWidget *lbox2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(lbox2), 10);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new(_("Language:")), FALSE, FALSE, 0);
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
        gtk_box_pack_start(GTK_BOX(row), app->settings_language_combo, FALSE, FALSE, 0);
        app->settings_restart_btn = gtk_button_new_with_label(_("Restart now"));
        g_signal_connect(app->settings_restart_btn, "clicked", G_CALLBACK(on_settings_restart_clicked), app);
        gtk_box_pack_start(GTK_BOX(row), app->settings_restart_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(lbox2), row, FALSE, FALSE, 0);
        app->settings_language_note = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(app->settings_language_note), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(app->settings_language_note), TRUE);
        gtk_box_pack_start(GTK_BOX(lbox2), app->settings_language_note, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(lframe2), lbox2);
        gtk_box_pack_start(GTK_BOX(vbox), lframe2, FALSE, FALSE, 0);
        settings_update_language_note(app);
    }

    /* Appearance (1.29.0) */
    {
        GtkWidget *aframe = gtk_frame_new(_("Appearance"));
        GtkWidget *abox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(abox), 10);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new(_("Mode:")), FALSE, FALSE, 0);
        app->settings_appearance_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->settings_appearance_combo), "system", _("Follow the system"));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->settings_appearance_combo), "dark", _("Dark"));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->settings_appearance_combo), "light", _("Light"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->settings_appearance_combo), app->appearance);
        g_signal_connect(app->settings_appearance_combo, "changed", G_CALLBACK(on_settings_appearance_changed), app);
        gtk_box_pack_start(GTK_BOX(row), app->settings_appearance_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(abox), row, FALSE, FALSE, 0);
        app->settings_appearance_note = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(app->settings_appearance_note), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(app->settings_appearance_note), TRUE);
        gtk_box_pack_start(GTK_BOX(abox), app->settings_appearance_note, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(aframe), abox);
        gtk_box_pack_start(GTK_BOX(vbox), aframe, FALSE, FALSE, 0);
        settings_update_appearance_note(app);
    }

    /* Keyboard layout (1.28.0) */
    {
        GtkWidget *lframe = gtk_frame_new(_("Keyboard Layout"));
        GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(lbox), 10);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new(_("Layout:")), FALSE, FALSE, 0);
        app->settings_layout_combo = gtk_combo_box_text_new();
        for (int i = 0; i < KBD_NUM_LAYOUTS; i++)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app->settings_layout_combo), kbd_layouts[i].code, _(kbd_layouts[i].label));
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(app->settings_layout_combo), kbd_layout_code);
        g_signal_connect(app->settings_layout_combo, "changed", G_CALLBACK(on_settings_layout_changed), app);
        gtk_box_pack_start(GTK_BOX(row), app->settings_layout_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(lbox), row, FALSE, FALSE, 0);
        app->settings_layout_note = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(app->settings_layout_note), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(app->settings_layout_note), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(app->settings_layout_note), 110);
        gtk_box_pack_start(GTK_BOX(lbox), app->settings_layout_note, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(lframe), lbox);
        gtk_box_pack_start(GTK_BOX(vbox), lframe, FALSE, FALSE, 0);
        settings_update_layout_note(app);
    }

    GtkWidget *note = gtk_label_new(_("Switching asks for the administrator password and takes effect right away. "
                                    "The setting applies to the whole system and stays after reboots and updates."));
    gtk_label_set_xalign(GTK_LABEL(note), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_set_name(note, "kbd-off-switch-note");
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);

    settings_sync_switches(app);
    return vbox;
}

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
    if (app->kbd_connected && app->kbd_socket_fd >= 0) {
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
    g_print("[GUI] Tray profile selected: index %d (%s)\n", selected_idx, app->profile_names[selected_idx]);
    
    if (selected_idx >= 0 && selected_idx < app->num_profiles && selected_idx != app->active_profile_idx) {
        g_print("[GUI] Activating profile %d: %s\n", selected_idx, app->profile_names[selected_idx]);
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "SET_ACTIVE_PROFILE %d", selected_idx);
        const char *response = send_command(app, cmd);
        if (response && strstr(response, "OK")) {
            g_print("[GUI] Profile activated successfully\n");
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
    g_print("[GUI] Tray keyboard profile '%s': %s\n", pname, r ? r : "NULL");
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
    
    g_print("[GUI] Tray CPU profile selected: index %d (%s)\n", selected_idx, profile_name ? profile_name : "(null)");
    
    if (selected_idx >= 0 && selected_idx != app->cpu_active_custom_profile_idx) {
        g_print("[GUI] Activating CPU profile %d\n", selected_idx);
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "APPLY_CUSTOM_PROFILE|%d", selected_idx);
        const char *response = send_cpu_command(app, cmd);
        if (response && strncmp(response, "OK", 2) == 0) {
            g_print("[GUI] CPU profile activated successfully\n");
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

/* ~/.config/dudes-control-center/config.json as a JSON object (empty if missing) */
static cJSON *config_read(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/dudes-control-center/config.json", g_get_user_config_dir());
    gchar *data = NULL;
    cJSON *json = NULL;
    if (g_file_get_contents(path, &data, NULL, NULL)) json = cJSON_Parse(data);
    g_free(data);
    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        json = cJSON_CreateObject();
    }
    return json;
}

static void config_write(cJSON *json) {
    char path[512];
    snprintf(path, sizeof path, "%s/dudes-control-center/config.json", g_get_user_config_dir());
    char *text = cJSON_Print(json);
    if (text) {
        gchar *with_nl = g_strconcat(text, "\n", NULL);
        if (!g_file_set_contents(path, with_nl, -1, NULL)) g_print("[GUI] WARNING: cannot write %s\n", path);
        g_free(with_nl);
        cJSON_free(text);
    }
}

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
            g_print("[GUI] Appearance system: desktop theme %s\n", theme);
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
            g_print("[GUI] Appearance %s: theme %s instead of %s\n", appearance_names[mode], cand[i], theme ? theme : "?");
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
    g_print("[GUI] Desktop theme changed (%s): %s\n", key, t ? t : "?");
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
    const char *key = NULL;
    GSettings *desk = appearance_desktop_settings(&key);
    if (desk && key) {
        gchar *sig = g_strdup_printf("changed::%s", key);
        g_signal_connect(desk, sig, G_CALLBACK(on_desktop_theme_changed), app);
        g_free(sig);
    }
    g_print("[GUI] Appearance: %s, window is %s\n", appearance_names[app->appearance], app->is_dark ? "dark" : "light");
}

static void appearance_save_choice(int mode) {
    cJSON *json = config_read();
    cJSON_DeleteItemFromObject(json, "appearance");
    cJSON_AddStringToObject(json, "appearance", appearance_names[mode]);
    config_write(json);
    cJSON_Delete(json);
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

/* Save window position to config.json */
static void save_window_position(AppData *app) {
    const char *config_dir = g_get_user_config_dir();
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/dudes-control-center/config.json", config_dir);
    
    /* keep the other settings (keyboard_layout, 1.28.0) */
    cJSON *json = config_read();
    cJSON_DeleteItemFromObject(json, "window_x");
    cJSON_DeleteItemFromObject(json, "window_y");
    cJSON_DeleteItemFromObject(json, "window_width");
    cJSON_DeleteItemFromObject(json, "window_height");
    cJSON_AddNumberToObject(json, "window_x", app->window_x);
    cJSON_AddNumberToObject(json, "window_y", app->window_y);
    cJSON_AddNumberToObject(json, "window_width", app->window_width);
    cJSON_AddNumberToObject(json, "window_height", app->window_height);
    
    char *json_str = cJSON_Print(json);
    FILE *f = fopen(config_path, "w");
    if (f) {
        fprintf(f, "%s\n", json_str);
        fclose(f);
        g_print("[GUI] Saved window position to config.json: %d,%d %ux%u\n",
                app->window_x, app->window_y, app->window_width, app->window_height);
    }
    
    cJSON_free(json_str);
    cJSON_Delete(json);
}

/* Load window position from config.json */
static void load_window_position(AppData *app) {
    const char *config_dir = g_get_user_config_dir();
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/dudes-control-center/config.json", config_dir);
    
    FILE *f = fopen(config_path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *json_str = malloc(fsize + 1);
        if (json_str) {
            size_t read = fread(json_str, 1, fsize, f);
            json_str[read] = '\0';
            
            cJSON *json = cJSON_Parse(json_str);
            if (json) {
                cJSON *val;
                if ((val = cJSON_GetObjectItem(json, "window_x"))) app->window_x = val->valueint;
                if ((val = cJSON_GetObjectItem(json, "window_y"))) app->window_y = val->valueint;
                if ((val = cJSON_GetObjectItem(json, "window_width"))) app->window_width = (guint)val->valueint;
                if ((val = cJSON_GetObjectItem(json, "window_height"))) app->window_height = (guint)val->valueint;
                cJSON_Delete(json);
            }
            free(json_str);
        }
        fclose(f);
        g_print("[GUI] Loaded window position from config.json: %d,%d %ux%u\n",
                app->window_x, app->window_y, app->window_width, app->window_height);
    } else {
        /* No saved position - center the window on screen */
        app->window_width = 800;
        app->window_height = 600;
        
        /* Get primary monitor size for centering */
        GdkScreen *screen = gtk_widget_get_screen(app->window);
        if (screen) {
            GdkDisplay *display = gdk_screen_get_display(screen);
            GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
            if (monitor) {
                GdkRectangle geometry;
                gdk_monitor_get_geometry(monitor, &geometry);
                
                /* Calculate centered position */
                app->window_x = geometry.width / 2 - app->window_width / 2;
                app->window_y = geometry.height / 2 - app->window_height / 2;
                
                g_print("[GUI] No saved position, centering on screen: %d,%d\n",
                        app->window_x, app->window_y);
            }
        }
    }
    
    /* Apply the loaded/default window position */
    gtk_window_move(GTK_WINDOW(app->window), app->window_x, app->window_y);
    
    /* Set default size but allow window to shrink based on content */
    gtk_window_set_default_size(GTK_WINDOW(app->window), (gint)app->window_width, (gint)app->window_height);
}

/* Handle window close button - minimize to tray instead of closing */
static gboolean on_window_delete_event(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    (void)event;
    AppData *app = (AppData *)user_data;
    
    /* Check for unsaved curve point changes before closing */
    if (app->curve_points_modified) {
        int result = check_unsaved_changes(app);
        if (result == 1) {
            /* User chose to apply changes - save them before hiding */
            on_apply_clicked(NULL, app);
        } else if (result == 0) {
            /* User chose to discard changes - clear the flag */
            app->curve_points_modified = 0;
        } else {
            /* User clicked Cancel - abort the close */
            return TRUE;
        }
    }
    if (cpu_check_unsaved_changes(app) < 0) return TRUE;   /* CPU profile (1.23.0) */
    if (gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook)) == PAGE_KEYBOARD &&
        kbd_leave_tab(app) < 0) return TRUE;                 /* keyboard profile (1.24.0) */
    
    /* Save current window position before hiding */
    gtk_window_get_position(GTK_WINDOW(widget), &app->window_x, &app->window_y);
    app->window_width = gtk_widget_get_allocated_width(widget);
    app->window_height = gtk_widget_get_allocated_height(widget);
    save_window_position(app);
    
    g_print("[GUI] Window close button clicked, minimizing to tray\n");
    gtk_widget_hide(widget);
    
    /* Return TRUE to prevent the window from being destroyed */
    return TRUE;
}

/* Refresh timer callback - fetch STATUS and update UI every 2 seconds */
/* Status tab: the profiles in use right now (1.30.0) */
static void status_set(GtkWidget *label, const char *text, int grey) {
    if (!label) return;
    if (grey) {
        gchar *m = g_markup_printf_escaped("<span foreground='#888888'>%s</span>", text);
        gtk_label_set_markup(GTK_LABEL(label), m);
        g_free(m);
    } else {
        gtk_label_set_text(GTK_LABEL(label), text);
    }
}

static void status_update_active_profiles(AppData *app) {
    if (!app->active_fan_label) return;
    char buf[256];
    /* fan */
    if (dcc_component_disabled(COMP_FAN)) status_set(app->active_fan_label, _("Fan control switched off"), 1);
    else if (!app->connected) status_set(app->active_fan_label, _("Fan daemon not connected"), 1);
    else {
        const char *name = (app->profile_names && app->active_profile_idx >= 0 && app->active_profile_idx < app->num_profiles)
                         ? app->profile_names[app->active_profile_idx] : "--";
        if (app->in_manual_mode) snprintf(buf, sizeof buf, _("%s (manual mode: %d%%)"), name, app->manual_current_speed);
        else snprintf(buf, sizeof buf, "%s", name);
        status_set(app->active_fan_label, buf, 0);
    }
    /* CPU profile and power mode */
    if (dcc_component_disabled(COMP_CPU)) {
        status_set(app->active_cpu_label, _("CPU control switched off"), 1);
        status_set(app->active_mode_label, _("CPU control switched off"), 1);
    } else if (!app->cpu_connected) {
        status_set(app->active_cpu_label, _("CPU daemon not connected"), 1);
        status_set(app->active_mode_label, _("CPU daemon not connected"), 1);
    } else {
        const char *cpu = "--";
        gchar *disp = NULL;
        GtkTreeIter it;
        if (app->cpu_running_profile_idx >= 0 && app->cpu_custom_profile_store &&
            gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(app->cpu_custom_profile_store), &it, NULL, app->cpu_running_profile_idx)) {
            gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &it, 0, &disp, -1);
            if (disp) cpu = cpu_profile_name_from_display(disp);
        }
        status_set(app->active_cpu_label, cpu, 0);
        g_free(disp);
        const char *mode = app->cpu_page_governor_label ? gtk_label_get_text(GTK_LABEL(app->cpu_page_governor_label)) : NULL;
        status_set(app->active_mode_label, mode && *mode ? mode : "--", 0);
    }
    /* keyboard: asked over an existing connection only (no reconnect here) */
    if (dcc_component_disabled(COMP_KEYBOARD)) status_set(app->active_kbd_label, _("Keyboard backlight switched off"), 1);
    else if (!app->kbd_connected || app->kbd_socket_fd < 0) status_set(app->active_kbd_label, _("Keyboard daemon not connected"), 1);
    else {
        const char *r = send_keyboard_command_once(app, "LIST_PROFILES");
        const char *act = r ? strstr(r, "[*] ") : NULL;
        if (act) {
            act += 4;
            size_t n = strcspn(act, "\n");
            snprintf(buf, sizeof buf, "%.*s", (int)MIN(n, sizeof buf - 1), act);
            status_set(app->active_kbd_label, buf, 0);
        } else {
            status_set(app->active_kbd_label, "--", 0);
        }
    }
}

static gboolean refresh_timer_callback(gpointer user_data) {
    AppData *app = (AppData *)user_data;
    update_daemon_status_labels(app);
    status_update_active_profiles(app);
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    
    /* Every 6 s: keep the keyboard connection alive (reconnects after a
     * daemon restart) and the Backlight Off switch in sync with the daemon */
    static int kbd_tick = 0;
    if (++kbd_tick >= 3 && !dcc_component_disabled(COMP_KEYBOARD)) {
        kbd_tick = 0;
        kbd_sync_power_switch(app);
    }

    if (dcc_component_disabled(COMP_FAN)) return G_SOURCE_CONTINUE;   /* fan control off */

    if (!app->connected) {
        /* Try to reconnect when disconnected (e.g., after suspend) */
        g_print("%s [GUI] Not connected, attempting to reconnect...\n", ts);
        connect_to_daemon(app);
        return G_SOURCE_CONTINUE;
    }
    
    g_print("%s [GUI] >>> refresh_timer_callback firing (app->in_manual_mode=%d, app->auto_mode_enabled=%d) <<<\n", ts, app->in_manual_mode, app->auto_mode_enabled);
    
    /* Fetch current status from daemon and parse it to update UI */
    const char *response = send_command(app, "STATUS");
    if (response != NULL) {
        /* Only parse if we got a valid response, not an error */
        if (strncmp(response, "ERROR", 5) != 0) {
            parse_status(app, response);
        } else {
            g_print("%s [GUI] Received error response: %s\n", ts, response);
        }
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] <<< refresh_timer_callback done (app->in_manual_mode=%d, app->auto_mode_enabled=%d) <<<\n", ts, app->in_manual_mode, app->auto_mode_enabled);
    }
    
    return G_SOURCE_CONTINUE;
}

/* UI language (1.31.0): config.json "language" ("system" or a code). The
 * catalog is chosen through LANGUAGE before GTK starts; "en" (the texts as
 * written) has no catalog, so it shows the original English even when the
 * system language has one. A change takes effect after a restart. */

/* gettext ignores LANGUAGE in the C locale (on glibc also in C.UTF-8): use
 * an installed UTF-8 locale for the messages then */
static void ui_language_ensure_messages_locale(void) {
    const char *cur = setlocale(LC_MESSAGES, NULL);
    if (cur && strcmp(cur, "C") && strcmp(cur, "POSIX") && strncmp(cur, "C.", 2)) return;
    const char *cands[] = { "en_US.UTF-8", "en_GB.UTF-8", "de_DE.UTF-8", "de_AT.UTF-8", "es_ES.UTF-8",
                            "ja_JP.UTF-8", "ko_KR.UTF-8", NULL };
    for (int i = 0; cands[i]; i++)
        if (setlocale(LC_MESSAGES, cands[i])) return;
}

static cJSON *config_read(void);
static void ui_language_init(void) {
    setlocale(LC_ALL, "");
    cJSON *json = config_read();
    cJSON *v = cJSON_GetObjectItem(json, "language");
    if (cJSON_IsString(v))
        for (int i = 0; i < UI_NUM_LANGUAGES; i++)
            if (!strcmp(v->valuestring, ui_languages[i].code))
                snprintf(ui_language_code, sizeof ui_language_code, "%s", ui_languages[i].code);
    cJSON_Delete(json);
    if (strcmp(ui_language_code, "system") != 0) {
        setenv("LANGUAGE", ui_language_code, 1);
        ui_language_ensure_messages_locale();
    }
    bindtextdomain(DCC_TEXT_DOMAIN, DCC_LOCALE_DIR);
    bind_textdomain_codeset(DCC_TEXT_DOMAIN, "UTF-8");
    textdomain(DCC_TEXT_DOMAIN);
}

/* Check if another instance is already running */
static int check_single_instance(void) {
    char pid_path[256];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(pid_path, sizeof(pid_path), "%s/.config/dudes-control-center/gui.pid", home);
    } else {
        return 0; /* Can't determine home directory, skip check */
    }
    
    /* Try to open existing PID file */
    FILE *fp = fopen(pid_path, "r");
    if (fp) {
        int old_pid;
        char line[32];
        if (fgets(line, sizeof(line), fp)) {
            old_pid = atoi(line);
            
            /* Check if process is still running (not this process itself:
             * "Restart now" in Settings re-executes with the same PID) */
            if (old_pid != getpid() && kill(old_pid, 0) == 0) {
                /* Process exists and we can signal it (it's running) */
                fclose(fp);
                return old_pid;
            }
        }
        fclose(fp);
    }
    
    return 0; /* No other instance found */
}

/* Write current PID to file */
static void write_pid_file(void) {
    char pid_path[256];
    const char *home = getenv("HOME");
    if (!home) {
        return;
    }
    
    snprintf(pid_path, sizeof(pid_path), "%s/.config/dudes-control-center/gui.pid", home);
    
    /* Ensure directory exists */
    char dir_path[256];
    snprintf(dir_path, sizeof(dir_path), "%s/.config/dudes-control-center", home);
    mkdir(dir_path, 0755);
    
    /* Write PID file */
    FILE *fp = fopen(pid_path, "w");
    if (fp) {
        fprintf(fp, "%d\n", getpid());
        fclose(fp);
    }
}

int main(int argc, char *argv[]) {
    gui_argv = argv;
    /* --debug: include DEBUG-level detail in GUI.log */
    LogLevel gui_log_level = LOG_LEVEL_INFO;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) gui_log_level = LOG_LEVEL_DEBUG;
    }

    /* Initialize logging first (NULL = use default user directory) */
    if (log_init("GUI", gui_log_level, NULL) != 0) {
        fprintf(stderr, "Failed to initialize logging system\n");
    }
    gui_install_log_handlers();

    LOG_INFO("Application starting (version %s)...", APP_VERSION);
    
    /* Check for another running instance before initializing GTK */
    int existing_pid = check_single_instance();
    if (existing_pid > 0) {
        LOG_ERROR("Another instance already running (PID: %d)", existing_pid);
        log_cleanup();
        return 1;
    }
    
    /* Write PID file BEFORE GTK init to avoid memory issues */
    write_pid_file();
    LOG_INFO("PID file written, process ID: %d", getpid());
    
    ui_language_init();   /* before GTK builds anything (1.31.0) */
    gtk_init(&argc, &argv);
    
    g_print("[GUI] GTK initialized successfully\n");
    g_print("[GUI] DISPLAY=%s\n", g_getenv("DISPLAY"));
    g_print("[GUI] XDG_RUNTIME_DIR=%s\n", g_getenv("XDG_RUNTIME_DIR"));
    
    AppData app = {0};
    app.emergency_auto_seen = -1;   /* first STATUS only records the count */
    app.cpu_running_profile_idx = -1;
    app.kbd_sel_zone = -1;
    
    /* Create main window */
    appearance_init(&app);   /* theme and dark/light before the window is built (1.29.0) */
    kbd_layout_init();   /* before the keyboard tab is built */
    kbd_mapping_set_defaults(&app);   /* until the wizard CSV is loaded */
    GtkWidget *window = create_main_window(&app);
    
    if (!window) {
        g_printerr("[GUI] ERROR: Failed to create main window!\n");
        log_cleanup();
        return 1;
    }
    
    g_print("[GUI] Main window created successfully\n");
    
    /* Load saved window position */
    load_window_position(&app);
    
    /* Connect to daemon */
    connect_to_daemon(&app);

    /* Connect to CPU daemon */
    connect_to_cpu_daemon(&app);

    /* Connect to keyboard daemon */
    connect_to_keyboard_daemon(&app);

    /* Initialize pending selection to current active profile */
    app.pending_active_profile_idx = app.active_profile_idx;
    
    /* Refresh profile list and load curve points for active profile */
    if (app.connected) {
        refresh_profile_list(&app);
        if (app.active_profile_idx >= 0) {
            load_curve_points_for_profile(&app, app.active_profile_idx);
        }
    }
    
    /* Start refresh timer (every 2 seconds) */
    app.refresh_timer_id = g_timeout_add_seconds(2, refresh_timer_callback, &app);

    /* CPU refresh timer (every 5 seconds); it also reconnects, so it runs
     * even if the CPU daemon is not reachable at startup */
    app.cpu_refresh_timer_id = g_timeout_add_seconds(5, cpu_refresh_timer_callback, &app);
    if (app.cpu_connected) cpu_load_initial_data(&app);

    /* Load CPU custom profiles at startup so they appear in the tray menu immediately.
     * Must run after STATUS query (cpu_core_count) and before tray creation. */
    if (app.cpu_connected) {
        load_cpu_custom_profiles(&app);
    }

    /* Create system tray icon */
    create_system_tray(&app);
    
    g_print("[GUI] System tray icon created\n");
    
    /* Start minimized - hide window initially, show only when user clicks tray icon */
    g_print("[GUI] Starting minimized to system tray\n");
    gtk_widget_hide(window);
    
    g_print("[GUI] Window hidden, ready for tray activation\n");
    
    /* Run main loop */
    gtk_main();
    
    LOG_INFO("Application shutting down...");
    log_cleanup();
    
    return 0;
}
