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
#include <sys/utsname.h>
#include <ctype.h>
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
#include <dirent.h>

/* UI languages (1.31.0): gettext catalogs dudes-control-center.mo */
#define DCC_TEXT_DOMAIN "dudes-control-center"
#ifndef DCC_LOCALE_DIR
#define DCC_LOCALE_DIR "/usr/share/locale"
#endif
#define _(s) gettext(s)
#define N_(s) (s)

/* Numbers from the daemons and data files always use a decimal point; atof()
 * follows the locale (German: comma) and stopped at the point (1.37.0) */
static double num_c(const char *s) { return g_ascii_strtod(s, NULL); }

/* GUI.log at INFO: events; plain g_print() goes to DEBUG (1.34.3) */
static void gui_event(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
/* Daemon connection lost / back (1.44.1): never dropped as a repeat, so a
 * "reconnecting" line always has its "connected" line */
static void gui_event_conn(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static int g_log_keep_repeats;
/* Daemons whose connection was lost: the next connect says "connected again" */
static int g_conn_lost_fan, g_conn_lost_cpu, g_conn_lost_kbd;
static void gui_log_connected(const char *daemon, int *lost);

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
static int gui_debug_flag;   /* started with --debug (kept by "Restart now") */

/* Define M_PI if not already defined */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 (M_PI / 2.0)
#endif

#include "dcc-log.h"
#include "dcc-common.h"
#include "dcc-report.h"
#include "dcc-cpu-common.h"
#include "dcc-kbd-common.h"
#include <cairo.h>
#include <cairo-gobject.h>
#include "gui/gui-kbd-layout.c"


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
        if (level > LOG_LEVEL_WARN && !g_log_keep_repeats && gui_log_is_repeat(msg)) continue;  /* never drop errors */
        log_write(level, "%s", msg);
    }
    g_free(copy);
}

/* g_print() is the GUI's old step-by-step trace (refresh timer, every
 * daemon reply, widget updates): DEBUG since 1.34.3, so GUI.log keeps days
 * of real events instead of a few hours. Events (connections, profile and
 * settings changes, results) use gui_event(), logged at INFO. Messages with
 * ERROR/Failed/WARN in them are raised to ERROR/WARN either way. */
static void gui_print_handler(const gchar *text) {
    gui_log_text(LOG_LEVEL_DEBUG, text);
    if (isatty(STDOUT_FILENO)) { fputs(text, stdout); fflush(stdout); }
}

static void gui_event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    gchar *text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gui_log_text(LOG_LEVEL_INFO, text);
    if (isatty(STDOUT_FILENO)) { fputs(text, stdout); fflush(stdout); }
    g_free(text);
}

static void gui_event_conn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    gchar *text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_log_keep_repeats = 1;
    gui_event("%s", text);
    g_log_keep_repeats = 0;
    g_free(text);
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
    /* Status tab (1.37.0): power, GPU, battery, NVMe */
    GtkWidget *pw_cpu_label, *pw_gpu_label, *pw_bat_label;
    GtkWidget *gpu_name_label, *gpu_clock_label, *gpu_load_label, *gpu_vram_label;
    GtkWidget *bat_charge_label, *bat_state_label, *bat_health_label;
    GtkWidget *nvme_box;                /* one label per NVMe drive */
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
    int refused_page;                  /* tab whose switch was cancelled (1.38.1) */

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
    GtkWidget *cpu_pp_row, *cpu_pp_combo;   /* firmware power profile of a CPU profile (1.43.0) */
    char cpu_pp_choices[256];               /* PLATFORM_PROFILE_CHOICES, "" = not offered */
    char cpu_pp_current[32];                /* PLATFORM_PROFILE */
    char cpu_snap_pp[32];
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
    int refresh_sec;                   /* refresh interval of the live values (1.40.0) */
    GtkWidget *settings_refresh_combo;
    /* Settings pages (1.49.0) */
    GtkWidget *settings_stack, *settings_other_page;
    int settings_other_warn;           /* sidebar title state: 0 unset, 1 plain, 2 with warning sign */
    /* Battery charge limit (1.46.0), from the CPU daemon's STATUS */
    int bat_limit;                     /* 0-100, -1 not offered, -2 unknown (no answer yet) */
    char bat_limit_dev[64];
    int bat_limit_seen;
    GtkWidget *settings_bat_combo, *settings_bat_note, *bat_limit_label;
    int settings_bat_syncing;
    GtkWidget *settings_minimized_switch;   /* start minimized to tray (1.40.0) */
    /* Direct EC access (1.41.0): from the fan daemon's STATUS */
    char ec_access[12];                /* "module", "hwmon", "ports", "none", "" = unknown */
    char ec_reason[200];
    GtkWidget *fan_ec_label, *manual_row;
    GtkWidget *settings_ec_switch, *settings_ec_state;
    GtkWidget *ec_dialog;                 /* the open warning, NULL if none */
    GtkWidget *settings_hw_switch, *settings_hw_state;   /* generic hwmon fans (1.43.0) */
    gulong settings_hw_sig;
    char hw_driver[32];                   /* HWMON_FANS_DRIVER of the fan daemon */
    gulong settings_ec_sig;
    guint instance_watch;              /* second start -> show window (1.40.0) */
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
    GtkWidget *kbd_fx_combo, *kbd_fx_speed, *kbd_fx_dir;   /* effect of zone / base (1.35.0) */
    int kbd_fx_syncing;
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
    /* Kind of keyboard backlight from the daemon (1.44.0): KBD_UI_* */
    int kbd_kind;
    GtkWidget *kbd_kind_label;
    GtkWidget *kbd_profile_frame, *kbd_backlight_frame, *kbd_preset_frame, *kbd_zone_frame;
    GtkWidget *kbd_wizard_box, *kbd_wizard_sep, *kbd_whole_btn, *kbd_off_note;
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
static void settings_update_battery(AppData *app);

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
static gboolean refresh_timer_callback(gpointer user_data);
static void on_tray_show_app(GtkMenuItem *menuitem, gpointer user_data);
static void status_show_ec(AppData *app);
static void settings_update_ec(AppData *app);
static void settings_update_hw(AppData *app);
static void label_set_changed(GtkWidget *l, const char *t);
static void tooltip_set_changed(GtkWidget *w, const char *t);
static void widget_set_shown(GtkWidget *w, gboolean show);
/* app->kbd_kind (1.44.0); UNKNOWN = older daemon: everything shown */
enum { KBD_UI_UNKNOWN = 0, KBD_UI_PER_KEY, KBD_UI_RGB, KBD_UI_WHITE, KBD_UI_NONE };
static void refresh_timers_start(AppData *app);
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

/* A lost daemon connection (1.44.1): the daemon closing it (restart, e.g.
 * during a package update, or stop) is normal and logged at INFO; the GUI
 * reconnects by itself. Other socket errors stay errors. */
static void gui_log_lost(const char *daemon, const char *what, int err) {
    if (!strcmp(daemon, "Fan")) g_conn_lost_fan = 1;
    else if (!strcmp(daemon, "CPU")) g_conn_lost_cpu = 1;
    else if (!strcmp(daemon, "Keyboard")) g_conn_lost_kbd = 1;
    if (err == EPIPE || err == ECONNRESET || err == 0) {
        gui_event_conn("[GUI] %s daemon closed the connection (restarted or stopped) - reconnecting\n", daemon);
        return;
    }
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    g_print("%s [GUI] %s ERROR: %s (%s), marking disconnected\n", ts, daemon, what, strerror(err));
}

/* "Fan daemon connected", or "... connected again" after a lost connection:
 * the same pair of lines for all three daemons (1.44.1) */
static void gui_log_connected(const char *daemon, int *lost) {
    gui_event_conn("[GUI] %s daemon connected%s\n", daemon, *lost ? " again" : "");
    *lost = 0;
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
        gui_log_lost("Fan", "Failed to send command", errno);
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
        gui_log_lost("Fan", "Failed to receive response", errno);
        app->connected = 0;
        if (app->socket_fd >= 0) {
            close(app->socket_fd);
            app->socket_fd = -1;
        }
        strcpy(response, "ERROR Failed to receive response");
        return response;
    }
    if (received == 0) {
        gui_log_lost("Fan", "Connection closed", 0);
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
        gui_log_lost("CPU", "Failed to send command", errno);
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
            gui_log_lost("CPU", "poll failed", errno);
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
            gui_log_lost("CPU", "Failed to receive response", errno);
            app->cpu_connected = 0;
            if (app->cpu_socket_fd >= 0) {
                close(app->cpu_socket_fd);
                app->cpu_socket_fd = -1;
            }
            strcpy(response, "ERROR Failed to receive CPU response");
            return response;
        }
        if (received == 0) {
            gui_log_lost("CPU", "Connection closed", 0);
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
    settings_update_battery(app);   /* the charge limit needs the CPU daemon (1.46.0) */
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
    gui_log_connected("Fan", &g_conn_lost_fan);
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
        gui_event("[GUI] CPU daemon socket not found at %s. CPU tab will be unavailable.\n", CPU_SOCKET_PATH);
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
    gui_log_connected("CPU", &g_conn_lost_cpu);
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
        gui_event("[GUI] Keyboard daemon socket not found at %s. Keyboard tab will be unavailable.\n", KBD_SOCKET_PATH);
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
    gui_log_connected("Keyboard", &g_conn_lost_kbd);

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
        gui_log_lost("Keyboard", "Failed to send command", errno);
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
            gui_log_lost("Keyboard", "poll failed", errno);
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
            gui_log_lost("Keyboard", "Failed to receive response", errno);
            app->kbd_connected = 0;
            if (app->kbd_socket_fd >= 0) {
                close(app->kbd_socket_fd);
                app->kbd_socket_fd = -1;
            }
            strcpy(response, "ERROR Failed to receive keyboard response");
            return response;
        }
        if (received == 0) {
            gui_log_lost("Keyboard", "Connection closed", 0);
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
    if (!app->kbd_connected) {          /* the send/receive found a dead connection (logged there) */
        if (kbd_ensure_connected(app)) r = send_keyboard_command_once(app, cmd);
    }
    return r;
}

/* A tab switch was cancelled (unsaved changes). GTK has already moved its
 * focus tab to the clicked tab before "switch-page", so another click on
 * that tab did nothing until a different tab was clicked (1.38.1). Switching
 * to it and back without the checks puts the focus tab on the shown page;
 * both happen before the next redraw, so nothing flickers. */
static gboolean notebook_refocus_idle(gpointer user_data) {
    AppData *app = (AppData *)user_data;
    GtkNotebook *nb = GTK_NOTEBOOK(app->notebook);
    int shown = gtk_notebook_get_current_page(nb);
    int other = app->refused_page;
    if (other >= 0 && other != shown && other < gtk_notebook_get_n_pages(nb)) {
        app->switching_page = 1;
        gtk_notebook_set_current_page(nb, other);
        gtk_notebook_set_current_page(nb, shown);
        app->switching_page = 0;
    }
    app->refused_page = -1;
    return G_SOURCE_REMOVE;
}

static void notebook_refuse_switch(AppData *app, GtkNotebook *notebook, int stay, int wanted) {
    gtk_notebook_set_current_page(notebook, stay);
    g_signal_stop_emission_by_name(G_OBJECT(notebook), "switch-page");
    app->refused_page = wanted;
    g_idle_add(notebook_refocus_idle, app);
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

            /* Stay on this page (and keep GTK's focus tab on it) */
            notebook_refuse_switch(app, notebook, current_page, page_num);

            g_print("%s [GUI] -> Page switch cancelled, staying on page %d\n", ts, current_page);

            /* Return early to prevent further execution of this callback */
            return;
        }
    }

    /* Leaving the Keyboard tab: unsaved changes, then the active profile again (1.24.0) */
    if (current_page == PAGE_KEYBOARD && page_num != PAGE_KEYBOARD) {
        if (kbd_leave_tab(app) < 0) {
            notebook_refuse_switch(app, notebook, current_page, page_num);
            return;
        }
    }

    /* Leaving the CPU tab with unsaved profile changes (1.23.0) */
    if (current_page == PAGE_CPU && page_num != PAGE_CPU && cpu_profile_has_unsaved_changes(app)) {
        if (cpu_check_unsaved_changes(app) < 0) {
            notebook_refuse_switch(app, notebook, current_page, page_num);
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
/* The tabs (1.48.1): one file each in src/gui/, part of this translation unit */
#include "gui/gui-fan.c"
#include "gui/gui-cpu.c"
#include "gui/gui-keyboard.c"
#include "gui/gui-status.c"
#include "gui/gui-logs.c"
#include "gui/gui-help.c"
#include "gui/gui-settings.c"
#include "gui/gui-tray.c"
#include "gui/gui-appearance.c"


/* Create main window */
static GtkWidget *create_main_window(AppData *app) {
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), _("Dudes Control Center"));

    /* Window icon from the icon theme (hicolor, all sizes), like the menu entry;
     * the old relative "DudesControlCenter.png" only worked from the source
     * folder (1.34.4) */
    if (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), "dudes-control-center")) {
        gtk_window_set_icon_name(GTK_WINDOW(app->window), "dudes-control-center");
    } else {
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file("/usr/share/icons/hicolor/256x256/apps/dudes-control-center.png", NULL);
        if (!pixbuf) pixbuf = gdk_pixbuf_new_from_file("DudesControlCenter.png", NULL);
        if (pixbuf) {
            gtk_window_set_icon(GTK_WINDOW(app->window), pixbuf);
            g_object_unref(pixbuf);
        }
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

    GtkWidget *settings_page = create_settings_page(app);   /* pages scroll themselves (1.49.0) */
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

/* Second start (1.40.0): a running GUI listens on a socket in the user's
 * runtime directory; a new start that finds it asks it to show its window
 * and quits. Without the socket (older GUI) the new start only quits. */
static void instance_socket_path(char *buf, size_t size) {
    const char *rt = g_getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) snprintf(buf, size, "%s/dcc-gui.sock", rt);
    else snprintf(buf, size, "%s/dudes-control-center/gui.sock", g_get_user_config_dir());
}

static gboolean instance_on_accept(GIOChannel *ch, GIOCondition cond, gpointer user_data) {
    (void)cond;
    AppData *app = (AppData *)user_data;
    int fd = accept(g_io_channel_unix_get_fd(ch), NULL, NULL);
    if (fd < 0) return G_SOURCE_CONTINUE;
    char buf[16] = "";
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 500) > 0) {
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n > 0) buf[n] = '\0';
    }
    close(fd);
    if (strncmp(buf, "SHOW", 4) == 0 && app->window) {
        gui_event("[GUI] Started again: showing the window\n");
        on_tray_show_app(NULL, app);
    }
    return G_SOURCE_CONTINUE;
}

static void instance_listen(AppData *app) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    instance_socket_path(a.sun_path, sizeof a.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    unlink(a.sun_path);   /* left over from a GUI that did not quit cleanly */
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 4) != 0) {
        g_print("[GUI] WARNING: no instance socket %s: %s\n", a.sun_path, strerror(errno));
        close(fd);
        return;
    }
    chmod(a.sun_path, 0600);
    GIOChannel *ch = g_io_channel_unix_new(fd);
    g_io_channel_set_close_on_unref(ch, TRUE);
    app->instance_watch = g_io_add_watch(ch, G_IO_IN, instance_on_accept, app);
    g_io_channel_unref(ch);
}

/* 1 if a running GUI took the request */
static int instance_send_show(void) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    instance_socket_path(a.sun_path, sizeof a.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    int ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0 && write(fd, "SHOW\n", 5) == 5;
    close(fd);
    return ok;
}

/* Fan Speed section: what the fan daemon can do (1.41.0). Without the
 * dcc_fan module and without direct EC access nothing is controlled; with
 * direct access it says so, because that is only safe on Clevo EC layouts. */
/* Set only what changed: these run on every refresh, and each label change
 * makes GTK lay the window out again */
static void label_set_changed(GtkWidget *l, const char *t) {
    if (strcmp(gtk_label_get_text(GTK_LABEL(l)), t)) gtk_label_set_text(GTK_LABEL(l), t);
}

static void tooltip_set_changed(GtkWidget *w, const char *t) {
    gchar *cur = gtk_widget_get_tooltip_text(w);
    if (g_strcmp0(cur, t)) gtk_widget_set_tooltip_text(w, t);
    g_free(cur);
}

static void widget_set_shown(GtkWidget *w, gboolean show) {
    if (gtk_widget_get_visible(w) == show) return;
    if (show) gtk_widget_show(w); else gtk_widget_hide(w);
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

static gboolean refresh_timer_callback(gpointer user_data) {
    AppData *app = (AppData *)user_data;
    update_daemon_status_labels(app);
    status_update_active_profiles(app);
    status_update_local(app);   /* battery, NVMe (1.37.0) */
    char ts[32];
    get_timestamp(ts, sizeof(ts));

    /* Every 6 s: keep the keyboard connection alive (reconnects after a
     * daemon restart) and the Backlight Off switch in sync with the daemon */
    static int kbd_tick = 0;
    if (++kbd_tick >= refresh_kbd_ticks(app->refresh_sec) && !dcc_component_disabled(COMP_KEYBOARD)) {
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
        } else if (app->connected) {
            /* a lost connection was logged already */
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
/* Is pid a running dcc-gui (1.38.0)? kill(pid, 0) alone also succeeds for
 * a zombie: a GUI started from the desktop's menu that has quit stays one
 * until the desktop collects it, which Cinnamon did not, so every new start
 * thought it was already running. A reused PID of another program does not
 * count either. */
static int gui_pid_alive(int pid) {
    if (pid <= 0 || kill(pid, 0) != 0) return 0;
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 1;                         /* no /proc: trust kill() */
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* "pid (comm) state ...": comm may contain spaces and ')' */
    char *open = strchr(buf, '('), *close = strrchr(buf, ')');
    if (!open || !close || close < open || close[1] != ' ') return 1;
    char state = close[2];
    if (state == 'Z' || state == 'X' || state == 'x') return 0;   /* zombie or dead */
    *close = '\0';
    return strcmp(open + 1, "dcc-gui") == 0;
}

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
            if (old_pid != getpid() && gui_pid_alive(old_pid)) {
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
    int start_flag = -1;   /* --minimized: 1, --show: 0, none: the setting (1.40.0) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) { gui_log_level = LOG_LEVEL_DEBUG; gui_debug_flag = 1; }
        else if (strcmp(argv[i], "--minimized") == 0) start_flag = 1;
        else if (strcmp(argv[i], "--show") == 0) start_flag = 0;
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
        /* Started again: show the running GUI's window (1.40.0). Also with
         * --minimized, which the menu entry has when "Start minimized" is
         * on: the option is for a fresh start only */
        if (instance_send_show())
            LOG_INFO("Already running (PID: %d): asked it to show its window", existing_pid);
        else
            LOG_INFO("Another instance already running (PID: %d)", existing_pid);
        log_cleanup();
        return 0;
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

    gui_event("[GUI] Main window created successfully\n");

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

    /* Refresh timers for the live values, interval from Settings (1.40.0);
     * the CPU timer also reconnects, so it runs even if the CPU daemon is
     * not reachable at startup */
    refresh_timers_start(&app);
    if (app.cpu_connected) cpu_load_initial_data(&app);

    /* Load CPU custom profiles at startup so they appear in the tray menu immediately.
     * Must run after STATUS query (cpu_core_count) and before tray creation. */
    if (app.cpu_connected) {
        load_cpu_custom_profiles(&app);
    }

    /* Create system tray icon */
    create_system_tray(&app);

    gui_event("[GUI] System tray icon created\n");

    /* Second starts show this window (1.40.0) */
    instance_listen(&app);

    /* Keep the desktop entries in step with the setting (the package's
     * menu entry may have changed); only once the user has chosen */
    if (start_minimized_is_set()) desktop_entries_update(start_minimized_load());

    /* Start minimized to the tray, or with the window open (1.40.0) */
    int minimized = start_flag >= 0 ? start_flag : start_minimized_load();
    gtk_widget_hide(window);
    if (minimized) {
        gui_event("[GUI] Starting minimized to system tray\n");
        g_print("[GUI] Window hidden, ready for tray activation\n");
    } else {
        gui_event("[GUI] Starting with the window open\n");
        on_tray_show_app(NULL, &app);
    }

    /* Run main loop */
    gtk_main();

    LOG_INFO("Application shutting down...");
    log_cleanup();

    return 0;
}
