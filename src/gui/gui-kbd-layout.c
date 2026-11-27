// SPDX-License-Identifier: GPL-2.0-only
/*
 * gui-kbd-layout.c
 * Keyboard layouts: key positions and names per layout (keys.csv), matching keys of two layouts.
 * Part of dcc-gui.c, which #includes it: one translation unit, so the
 * static functions and AppData stay shared (split up in 1.48.1).
 */


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
    char fx[24];                       /* effect (1.35.0), "" = static */
} KbdZoneEdit;
typedef struct {
    int has_base;
    unsigned char base[3];
    char base_fx[24];                  /* effect of the base color (1.35.0) */
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
        gui_event("[GUI] Keyboard layout '%s': cannot read %s\n", code, path);
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
        KeyPos k = { num_c(f[cx]), num_c(f[cy]), num_c(f[cw]), num_c(f[ch]) };
        if (k.w <= 0 || k.h <= 0) continue;
        snprintf(L->names[L->n], sizeof L->names[0], "%s", f[ck]);
        L->row[L->n] = (cr >= 0 && cr < nf && f[cr][0]) ? atoi(f[cr]) : -1;
        L->pos[L->n++] = k;
        if (k.x + k.w > L->right) L->right = k.x + k.w;
    }
    g_free(data);
    if (L->n == 0) gui_event("[GUI] Keyboard layout '%s': no keys in %s\n", code, path);
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
    gui_event("[GUI] Keyboard layout '%s': %zu keys\n", code, L.n);
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
