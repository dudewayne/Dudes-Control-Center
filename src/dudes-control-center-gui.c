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

/* Define M_PI if not already defined */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 (M_PI / 2.0)
#endif

#include "dudes-control-center-log.h"
#include "dudes-control-center-cpu-common.h"
#include "dudes-control-center-kbd-common.h"
#include <cairo.h>
#include <cairo-gobject.h>

/* --- Keyboard Visual Zone Editor --- */
typedef struct {
    double x, y, w, h;
} KeyPos;

/* Keyboard image dimensions */
#define KBD_IMAGE_WIDTH 1330
#define KBD_IMAGE_HEIGHT 440

/* Mapping from visual position index (in key_positions[]) to LED ID */
static const int kbd_position_to_led_id[120] = {
    41, 77, 14, 66, 1, 56, 109, 46, 96, 33, 7, 61, 114,
    50, 102, 39, 90, 27, 79, 16,
    3, 58, 111, 47, 98, 35, 86, 23, 74, 11, 118, 89, 54, 44,
    31, 94, 83, 20, 71, 8,
    52, 115, 104, 42, 92, 29, 81, 18, 69, 5, 112, 49, 100, 37,
    88, 19, 76, 13, 65, 113, 0,
    45, 108, 95, 32, 84, 21, 72, 9, 62, 116, 105, 43, 93, 82,
    70, 6, 60,
    101, 22, 89, 26, 78, 15, 67, 2, 57, 110, 97, 34, 85,
    73, 87, 10, 63, 117, 53, 55, 106,
    80, 48, 36, 24, 59, 119, 40, 68, 17, 75,
    12, 38, 107, 91, 25, 30, 103, 99, 119,
    51, 64, 4, 103
};

/* Key names from keyboard_keys_de.csv (one per position index, 120 entries) */
static const char *kbd_key_names[120] = {
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

/* QWERTZ keyboard key positions - from keyboard_keys_de.csv */
static const KeyPos kbd_key_positions[120] = {
    {24, 20, 58, 56 }, {88, 20, 58, 56 }, {151, 20, 58, 56 }, {215, 20, 58, 56 },
    {278, 20, 58, 56 }, {342, 20, 58, 56 }, {405, 20, 58, 56 }, {469, 20, 58, 56 },
    {533, 20, 58, 56 }, {596, 20, 58, 56 }, {660, 20, 58, 56 }, {723, 20, 58, 56 },
    {787, 20, 58, 56 }, {851, 20, 58, 56 }, {914, 20, 58, 56 }, {978, 20, 58, 56 },
    {1064, 20, 62, 56 }, {1132, 20, 62, 56 }, {1200, 20, 62, 56 }, {1268, 20, 62, 56 },
    {24, 88, 62, 62 }, {92, 88, 62, 62 }, {160, 88, 62, 62 }, {228, 88, 62, 62 },
    {296, 88, 62, 62 }, {364, 88, 62, 62 }, {432, 88, 62, 62 }, {500, 88, 62, 62 },
    {568, 88, 62, 62 }, {636, 88, 62, 62 }, {704, 88, 62, 62 }, {772, 88, 62, 62 },
    {772, 88, 62, 62 }, {840, 88, 62, 62 }, {908, 88, 130, 62 }, {908, 88, 130, 62 },
    {1064, 88, 62, 62 }, {1132, 88, 62, 62 }, {1200, 88, 62, 62 }, {1268, 88, 62, 62 },
    {24, 156, 96, 62 }, {24, 156, 96, 62 }, {126, 156, 62, 62 }, {194, 156, 62, 62 },
    {262, 156, 62, 62 }, {330, 156, 62, 62 }, {398, 156, 62, 62 }, {466, 156, 62, 62 },
    {534, 156, 62, 62 }, {602, 156, 62, 62 }, {670, 156, 62, 62 }, {738, 156, 62, 62 },
    {806, 156, 62, 62 }, {874, 156, 62, 62 }, {942, 156, 96, 130 }, {942, 156, 96, 130 },
    {1064, 156, 62, 62 }, {1132, 156, 62, 62 }, {1200, 156, 62, 62 }, {1268, 156, 62, 130 },
    {1268, 156, 62, 130 }, {24, 224, 113, 62 }, {24, 224, 113, 62 },
    {143, 224, 62, 62 }, {211, 224, 62, 62 }, {279, 224, 62, 62 }, {347, 224, 62, 62 },
    {415, 224, 62, 62 }, {483, 224, 62, 62 }, {551, 224, 62, 62 }, {619, 224, 62, 62 },
    {687, 224, 62, 62 }, {755, 224, 62, 62 }, {823, 224, 62, 62 }, {891, 224, 62, 62 },
    {1064, 224, 62, 62 }, {1132, 224, 62, 62 }, {1200, 224, 62, 62 },
    {24, 292, 79, 62 }, {24, 292, 79, 62 }, {109, 292, 62, 62 },
    {177, 292, 62, 62 }, {245, 292, 62, 62 }, {313, 292, 62, 62 }, {381, 292, 62, 62 },
    {449, 292, 62, 62 }, {517, 292, 62, 62 }, {585, 292, 62, 62 }, {653, 292, 62, 62 },
    {721, 292, 62, 62 }, {789, 292, 62, 62 }, {857, 292, 127, 62 }, {857, 292, 127, 62 },
    {996, 292, 62, 62 }, {1064, 292, 62, 62 }, {1132, 292, 62, 62 }, {1200, 292, 62, 62 },
    {1268, 292, 62, 130 }, {1268, 292, 62, 130 },
    {24, 360, 76, 62 }, {106, 360, 62, 62 }, {174, 360, 62, 62 }, {242, 360, 62, 62 },
    {310, 360, 375, 62 }, {690, 360, 62, 62 }, {758, 360, 62, 62 }, {826, 360, 89, 62 },
    {928, 360, 62, 62 }, {996, 360, 62, 62 }, {1064, 360, 62, 62 },
    {1132, 360, 62, 62 }, {1200, 360, 62, 62 }, {1268, 360, 62, 62 }
};

#define SOCKET_PATH "/run/dudes-control-center-fan.sock"
#define CPU_SOCKET_PATH "/run/dudes-control-center-cpu.sock"
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
    GtkListStore *curve_store;
    GtkWidget *curve_treeview;
    GtkWidget *curve_graph; /* Drawing area for temperature curve visualization */
    int num_profiles;
    char **profile_names;
    int active_profile_idx;
    
    /* Page 3: Profile List widgets */
    GtkWidget *profile_list_view;
    GtkWidget *placeholder_label;
    GSList *profile_radios; /* Track radio buttons for cleanup */
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
    GtkTextBuffer *gui_log_buffer;
    GtkTextBuffer *daemon_log_buffer;
    GtkTextBuffer *cpu_daemon_log_buffer;
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
    int cpu_active_custom_profile_idx;
    int cpu_freq_profiles_modified; /* Track unsaved changes */

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
    GtkWidget *kbd_status_label;
    guint kbd_refresh_timer_id;
    int kbd_active_profile_idx;
    int kbd_profiles_modified; /* Track unsaved changes */

    /* Keyboard zone editor widgets (dynamic zones) */
    GtkWidget *kbd_zone_name_entries[32];
    GtkWidget *kbd_zone_color_buttons[32];
    GtkWidget *kbd_zone_delete_buttons[32];
    GtkWidget *kbd_add_zone_btn;
    GtkWidget *kbd_off_switch; /* Toggle to disable keyboard backlight */
    int kbd_off_active; /* 1 when off switch is active */
    GtkWidget *kbd_apply_zones_btn;
    int kbd_num_zones;
    
    /* LED Wizard state */
    int kbd_wizard_active; /* 1 when wizard is running */
    int kbd_wizard_current_led; /* Current LED being assigned (0-119) */
    int kbd_wizard_mapping[120]; /* LED ID -> key position index */
    GtkWidget *kbd_led_wizard_btn; /* LED Wizard button */
    GtkWidget *kbd_led_wizard_label; /* Label showing current LED */
    GtkWidget *kbd_led_wizard_skip_btn; /* Skip button */
    GtkWidget *kbd_led_wizard_save_btn; /* Save & Finish button */
    GtkWidget *kbd_led_wizard_cancel_btn; /* Cancel button */
} AppData;

/* Forward declarations for callbacks */
static void on_apply_clicked(GtkWidget *widget, gpointer user_data);
static void on_cancel_clicked(GtkWidget *widget, gpointer user_data);
static void on_create_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_delete_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_rename_profile_clicked(GtkWidget *widget, gpointer user_data);

/* Forward declarations for page creation */
static GtkWidget *create_status_page(AppData *app);
static GtkWidget *create_curve_editor_page(AppData *app);
static GtkWidget *create_profile_list_page(AppData *app);
static GtkWidget *create_log_page(AppData *app);
static GtkWidget *create_about_page(void);
static GtkWidget *create_cpu_page(AppData *app);
static GtkWidget *create_keyboard_page(AppData *app);

/* Helper function to create clickable link label */
static GtkWidget *create_clickable_link(const char *url);

/* Wrapper for gtk_widget_show_all to use with g_idle_add */
static gboolean show_widget_idle(gpointer data) {
    gtk_widget_show_all(GTK_WIDGET(data));
    return FALSE; /* Don't repeat */
}

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
static void disconnect_from_cpu_daemon(AppData *app);
static const char *send_cpu_command(AppData *app, const char *cmd);
static void parse_cpu_status(AppData *app, const char *response);

/* Forward declarations for keyboard daemon connection */
static void connect_to_keyboard_daemon(AppData *app);
static void disconnect_from_keyboard_daemon(AppData *app);
static const char *send_keyboard_command(AppData *app, const char *cmd);
static void load_keyboard_profiles(AppData *app);
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
static void on_led_wizard_save_finish_clicked(GtkWidget *widget, gpointer user_data);
static void on_led_wizard_cancel_clicked(GtkWidget *widget, gpointer user_data);
static void kbd_wizard_step(AppData *app, const char *prev_key_name);
static void kbd_wizard_finish(AppData *app);
static void kbd_save_led_mapping_csv(AppData *app);
static void kbd_load_led_mapping_csv(AppData *app);
static gboolean cpu_refresh_timer_callback(gpointer user_data);
static void load_cpu_governors(AppData *app);
static void on_cpu_governor_changed(GtkWidget *widget, gpointer user_data);

/* Forward declarations for CPU custom frequency profile management */
static gboolean populate_cpu_core_grid_idle(gpointer data);
static void load_cpu_custom_profiles(AppData *app);
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
static void on_cpu_hard_limit_apply_clicked(GtkWidget *widget, gpointer user_data);

/* Forward declarations for profile and curve management */
static void refresh_profile_list(AppData *app);
static void load_curve_points_for_profile(AppData *app, int profile_idx);
static void gui_save_profiles_to_disk(AppData *app);
static void on_profile_radio_toggled(GtkWidget *widget, gpointer user_data);
static void on_apply_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_cancel_profile_clicked(GtkWidget *widget, gpointer user_data);
static void on_notebook_page_switched(GtkNotebook *notebook, GtkWidget *page, gint page_num, gpointer user_data);
static void update_profile_tab_for_manual_mode(AppData *app, int manual_mode_enabled);
static void on_disable_manual_mode_clicked(GtkWidget *widget, gpointer user_data);
static int check_unsaved_changes(AppData *app); /* Returns 1 if changes applied, 0 if discarded, -1 if cancelled */

/* System tray functions */
static GtkWidget* create_tray_menu(AppData *app);
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
                                  "<b>Warning:</b> You have unsaved changes to the curve points.\n\nWhat would you like to do?");
    
    /* Add buttons with custom response IDs */
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", -102);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Discard Changes", -100);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Apply Changes", -101);
    
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
    ssize_t sent = write(app->socket_fd, cmd_with_newline, strlen(cmd_with_newline));
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
    static char response[8192];

    if (!app->cpu_connected || app->cpu_socket_fd < 0) {
        strcpy(response, "ERROR Not connected to CPU daemon");
        return response;
    }

    char cmd_with_newline[256];
    snprintf(cmd_with_newline, sizeof(cmd_with_newline), "%s\n", cmd);

    if (strcmp(cmd, "STATUS") != 0) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] CPU Sending: %s\n", ts, cmd);
    }

    ssize_t sent = write(app->cpu_socket_fd, cmd_with_newline, strlen(cmd_with_newline));
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
        int poll_ret = poll(&pfd, 1, 200); /* 200ms timeout */
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
        total_received += received;
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
    int auto_mode = 0;
    char active_governor[64] = "N/A";
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
        } else if (strncmp(line, "AUTO_MODE=", 10) == 0) {
            auto_mode = atoi(line + 10);
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
                    strncpy(freq_tmp, freq_marker + 6, sizeof(freq_tmp) - 1);
                    freq_tmp[sizeof(freq_tmp) - 1] = '\0';
                    strncpy(core_freqs[core_idx], freq_tmp, sizeof(core_freqs[core_idx]) - 1);
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
    gtk_label_set_text(GTK_LABEL(app->cpu_page_governor_label), active_governor);
    gtk_label_set_text(GTK_LABEL(app->cpu_page_freq_label), freq_values);
    gtk_label_set_text(GTK_LABEL(app->cpu_page_min_max_freq_label), min_max_freq);

    /* Populate per-core frequency labels on first call, update on subsequent calls */

    if (app->cpu_core_count == 0 && core_count > 0) {
        /* First time: create labels in a 4-column grid */
        app->cpu_core_count = core_count;

        for (int i = 0; i < core_count && i < 32; i++) {
            char label_text[128];
            snprintf(label_text, sizeof(label_text), "Core %d: %s", i, core_freqs[i]);
            
            app->cpu_core_freq_labels[i] = gtk_label_new(label_text);
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
                snprintf(label_text, sizeof(label_text), "Core %d: %s", i, core_freqs[i]);
                gtk_label_set_text(GTK_LABEL(app->cpu_core_freq_labels[i]), label_text);
            }
        }
    }
}

/* CPU refresh timer callback - fetch CPU STATUS and update UI every 5 seconds */
static gboolean cpu_refresh_timer_callback(gpointer user_data) {
    AppData *app = (AppData *)user_data;

    if (!app->cpu_connected) {
        return G_SOURCE_CONTINUE;
    }

    const char *response = send_cpu_command(app, "STATUS");
    if (response != NULL && strncmp(response, "ERROR", 5) != 0) {
        parse_cpu_status(app, response);
    }

    return G_SOURCE_CONTINUE;
}

/* Load governor profiles into the CPU combo box */
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
        char gov_str[32] = "schedutil";
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

    g_print("[GUI] Applying governor: %s\n", governor_name);

    /* Build command: APPLY_GOVERNOR <name> */
    char command[256];
    snprintf(command, sizeof(command), "APPLY_GOVERNOR|%s", governor_name);

    const char *response = send_cpu_command(app, command);
    if (response && strncmp(response, "OK", 2) == 0) {
        show_status(app, "Governor applied successfully");
        g_print("[GUI] Governor '%s' applied successfully\n", governor_name);
    } else {
        show_status(app, "Failed to apply governor");
        g_print("[GUI] Failed to apply governor: %s\n", response);
    }

    g_free(governor_name);
}

/* ============================================================
 * CPU Custom Frequency Profile Management callbacks
 * ============================================================ */

/* Load custom frequency profiles from daemon into combo box */
static void load_cpu_custom_profiles(AppData *app) {
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
        profile_count++;

        line = strtok(NULL, "\n");
    }
    g_free(resp_copy);

    /* Select the active profile */
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

    g_print("[GUI] Loaded %d custom frequency profiles (active idx=%d)\n", profile_count, active_idx);

}

/* Callback: Custom profile combo box changed */
static void on_cpu_custom_profile_changed(GtkWidget *widget, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    int active = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
    if (active < 0) return;

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

    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "Profile selected - click Apply to activate");
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
static void on_cpu_create_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Create frequency profile clicked\n");

    /* Show dialog to enter profile name */
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Create CPU Frequency Profile",
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Create", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);

    GtkWidget *label = gtk_label_new("Enter profile name:");
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
                                                                 "A CPU frequency profile with the name '%s' already exists.",
                                                                 name);
                gtk_dialog_run(GTK_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
                gtk_widget_destroy(dialog);
                return;
            }

            /* Send CREATE_CUSTOM_PROFILE command to daemon */
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "CREATE_CUSTOM_PROFILE|%s|schedutil|%u|0", name, HARD_MAX_FREQ_HZ_DEFAULT);
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
                              g_strconcat("Created profile: ", name, NULL));
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
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "No profile selected");
        return;
    }

    /* Get profile name for confirmation */
    gchar *profile_name = NULL;
    GtkTreeIter iter;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->cpu_custom_profile_combo), &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter, 0, &profile_name, -1);
    }

    if (profile_name == NULL) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "Could not read profile name");
        return;
    }

    /* Show confirmation dialog */
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_QUESTION,
                                               GTK_BUTTONS_YES_NO,
                                               "Are you sure you want to delete the CPU frequency profile '%s'?",
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
                          g_strconcat("Deleted profile: ", profile_name, NULL));
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
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "No profile selected");
        return;
    }

    /* Get current profile name */
    gchar *current_name = NULL;
    GtkTreeIter iter;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->cpu_custom_profile_combo), &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_custom_profile_store), &iter, 0, &current_name, -1);
    }

    if (current_name == NULL) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "Could not read profile name");
        return;
    }

    /* Extract clean profile name from display string */
    const char *clean_name = cpu_profile_name_from_display(current_name);

    /* Show dialog to enter new name */
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Rename CPU Frequency Profile",
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Rename", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);

    GtkWidget *label = gtk_label_new("Enter new profile name:");
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

            /* Reload the profile list */
            load_cpu_custom_profiles(app);

            /* Keep selection on the renamed profile */
            g_signal_handler_block(app->cpu_custom_profile_combo, app->cpu_custom_profile_combo_signal_id);
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo), idx);
            g_signal_handler_unblock(app->cpu_custom_profile_combo, app->cpu_custom_profile_combo_signal_id);

            /* Save profiles to disk (GUI writes directly) */
            gui_save_profiles_to_disk(app);

            gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label),
                              g_strconcat("Renamed profile to: ", new_name, NULL));
        } else {
            gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "Name cannot be empty");
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
    double core_min_mhz[32];
    double core_max_mhz[32];
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
    char core_data[1024] = {0};
    size_t pos = 0;
    for (int i = 0; i < num_cores && i < 32; i++) {
        gulong min_hz = (gulong)(ctx->core_min_mhz[i] * 1000000);
        gulong max_hz = (gulong)(ctx->core_max_mhz[i] * 1000000);
        if (i > 0) {
            pos += snprintf(core_data + pos, sizeof(core_data) - pos, ",");
        }
        pos += snprintf(core_data + pos, sizeof(core_data) - pos, "%lu,%lu", min_hz, max_hz);
    }

    /* Send single batch command */
    char batch_cmd[2048];
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

    /* Reload profile list */
    load_cpu_custom_profiles(app);
    g_signal_handler_block(app->cpu_custom_profile_combo, app->cpu_custom_profile_combo_signal_id);
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo), idx);
    g_signal_handler_unblock(app->cpu_custom_profile_combo, app->cpu_custom_profile_combo_signal_id);
    LOG_INFO("Profile list reloaded");

    char status_msg[128];
    snprintf(status_msg, sizeof(status_msg), "Saved profile");
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), status_msg);

    /* Re-enable the save button */
    gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, TRUE);
    LOG_INFO("<<< Save frequency profile completed");

    g_free(ctx);
    return G_SOURCE_REMOVE;
}

static void on_cpu_save_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    LOG_INFO(">>> Save frequency profile clicked");

    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo));
    if (idx < 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "No profile selected");
        LOG_INFO("<<< Save aborted: no profile selected");
        return;
    }

    int num_cores = app->cpu_core_count;
    if (num_cores <= 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "Core count not available");
        LOG_INFO("<<< Save aborted: core count not available");
        return;
    }

    /* Capture all current UI state into a context struct */
    CpuSaveContext *ctx = g_new0(CpuSaveContext, 1);
    ctx->app = app;
    ctx->profile_idx = idx;
    ctx->num_cores = num_cores;
    ctx->apply_all_cores = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->cpu_apply_all_cores_checkbox)) ? 1 : 0;

    /* Read governor */
    {
        GtkTreeIter gov_iter;
        if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->cpu_profile_combo), &gov_iter)) {
            gchar *gov_name = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(app->cpu_profile_store), &gov_iter, 1, &gov_name, -1);
            strncpy(ctx->governor, gov_name ? gov_name : "", sizeof(ctx->governor) - 1);
            ctx->governor[sizeof(ctx->governor) - 1] = '\0';
            g_free(gov_name);
        }
    }

    /* Read spinner values */
    for (int i = 0; i < num_cores && i < 32; i++) {
        ctx->core_min_mhz[i] = gtk_adjustment_get_value(app->cpu_core_min_adjustments[i]);
        ctx->core_max_mhz[i] = gtk_adjustment_get_value(app->cpu_core_max_adjustments[i]);
    }

    /* Immediately disable the save button to clear its pressed visual state,
     * then re-enable it after the save completes in the idle callback. */
    gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "Saving...");

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
    snprintf(status_msg, sizeof(status_msg), "Activated profile");
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), status_msg);

    return FALSE; /* One-shot */
}

/* Callback: Activate (apply) the selected frequency profile to the CPU */
static void on_cpu_activate_freq_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Activate frequency profile clicked\n");

    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->cpu_custom_profile_combo));
    if (idx < 0) {
        gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "No profile selected");
        return;
    }

    /* Defer the apply + reload + save to an idle callback so GTK can finish
     * processing the button click before we destroy/rebuild the grid widgets.
     * This prevents race conditions with pending signal handlers. */
    ActivateIdleData *data = g_new0(ActivateIdleData, 1);
    data->app = app;
    data->profile_idx = idx;
    g_idle_add(activate_profile_idle_callback, data);
    gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "Activating...");
}

/* Callback: Apply hard frequency limit */
static void on_cpu_hard_limit_apply_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;

    double ghz = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->cpu_hard_limit_spinner));
    uint32_t hz = (uint32_t)(ghz * 1000000000.0);

    g_print("[GUI] Applying hard limit: %.2f GHz (%u Hz)\n", ghz, hz);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "SET_HARD_LIMIT|%u", hz);
    const char *response = send_cpu_command(app, cmd);
    g_print("[GUI] SET_HARD_LIMIT response: %s\n", response ? response : "NULL");

    if (response && strncmp(response, "OK", 2) == 0) {
        char status_msg[256];
        snprintf(status_msg, sizeof(status_msg), "Hard limit applied: %.2f GHz", ghz);
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
            response ? response : "Failed to apply hard limit");
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
        snprintf(core_name, sizeof(core_name), "Core %d", i);
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
                uint32_t hz = (uint32_t)strtoul(eq + 11, NULL, 10);
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
    GtkWidget *min_header = gtk_label_new("Min (MHz)");
    gtk_widget_set_halign(min_header, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(min_header, 2);
    gtk_grid_attach(GTK_GRID(app->cpu_core_freq_grid), min_header, col_min, num_cores, 1, 1);

    GtkWidget *max_header = gtk_label_new("Max (MHz)");
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
    char response_copy[BUFFER_SIZE];
    strncpy(response_copy, response, sizeof(response_copy) - 1);
    response_copy[sizeof(response_copy) - 1] = '\0';

    char *line = strtok(response_copy, "\n");
    while (line != NULL) {
        /* Trim leading/trailing whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        int core_id = -1;
        unsigned int freq_hz = 0;

        if (sscanf(p, "CORE_%d_MAX:%u", &core_id, &freq_hz) == 2) {
            if (core_id >= 0 && core_id < MAX_CPUS && app->cpu_core_max_adjustments[core_id] != NULL) {
                /* Convert Hz to MHz for spinner */
                double freq_mhz = (double)freq_hz / 1000000.0;
                gtk_adjustment_set_value(GTK_ADJUSTMENT(app->cpu_core_max_adjustments[core_id]), freq_mhz);
            }
        } else if (sscanf(p, "CORE_%d_MIN:%u", &core_id, &freq_hz) == 2) {
            if (core_id >= 0 && core_id < MAX_CPUS && app->cpu_core_min_adjustments[core_id] != NULL) {
                double freq_mhz = (double)freq_hz / 1000000.0;
                gtk_adjustment_set_value(GTK_ADJUSTMENT(app->cpu_core_min_adjustments[core_id]), freq_mhz);
            }
        }

        line = strtok(NULL, "\n");
    }

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
        snprintf(speed_text, sizeof(speed_text), "%d", app->manual_current_speed);
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

/* Callback: Manual speed + button clicked */
static void on_manual_speed_plus_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;
    AppData *app = (AppData *)user_data;
    int current_pct = app->manual_current_speed;
    if (current_pct < 100) { /* Allow reaching 100% even from 91-99% */
        current_pct += 10;
        if (current_pct > 100) current_pct = 100; /* Cap at 100% */
        app->manual_current_speed = current_pct; /* Update state immediately */
        
        /* Update the label display immediately */
        char speed_text[16];
        snprintf(speed_text, sizeof(speed_text), "%d%%", current_pct);
        gtk_label_set_text(GTK_LABEL(app->manual_speed_label), speed_text);
        
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "SET_MANUAL %d", current_pct);
        const char *response = send_command(app, cmd);
        show_status(app, response);
    }
}

/* Callback: Manual speed - button clicked */
static void on_manual_speed_minus_clicked(GtkWidget *button, gpointer user_data) {
    (void)button;
    AppData *app = (AppData *)user_data;
    int current_pct = app->manual_current_speed;
    if (current_pct > 10) { /* Leave room for -10% */
        current_pct -= 10;
        app->manual_current_speed = current_pct; /* Update state immediately */
        
        /* Update the label display immediately */
        char speed_text[16];
        snprintf(speed_text, sizeof(speed_text), "%d%%", current_pct);
        gtk_label_set_text(GTK_LABEL(app->manual_speed_label), speed_text);
        
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "SET_MANUAL %d", current_pct);
        const char *response = send_command(app, cmd);
        show_status(app, response);
    }
}

/* Callback: Apply curve changes - save reordered curve points to daemon */
static void on_apply_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    
    int profile_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->profile_combo));
    if (profile_idx < 0) {
        show_status(app, "ERROR No profile selected");
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

/* Callback: Cancel changes - reload profiles from daemon */
static void on_cancel_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    
    /* Reload curve points for current profile from daemon */
    int profile_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->profile_combo));
    if (profile_idx >= 0) {
        load_curve_points_for_profile(app, profile_idx);
    }
    
    /* Clear the modified flag since we're discarding changes */
    app->curve_points_modified = 0;
    
    show_status(app, "Changes cancelled - reloaded from daemon");
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

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Create New Profile",
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL,
                                                    "Cancel", GTK_RESPONSE_CANCEL,
                                                    "Create", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Profile name");
    
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
                                                                "A profile with the name '%s' already exists.",
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
            show_status(app, "ERROR Profile name cannot be empty");
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
        show_status(app, "ERROR No profile selected");
        return;
    }
    
    const char *profile_name = app->profile_names[idx];
    
    /* Don't allow deleting the Default profile */
    if (strcmp(profile_name, "Default") == 0) {
        show_status(app, "ERROR Cannot delete Default profile");
        return;
    }
    
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_QUESTION,
                                               GTK_BUTTONS_YES_NO,
                                               "Are you sure you want to delete the profile '%s'?", profile_name);
    
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
        show_status(app, "ERROR No profile selected");
        return;
    }

    const char *profile_name = app->profile_names[idx];

    /* Don't allow renaming the Default profile */
    if (strcmp(profile_name, "Default") == 0) {
        show_status(app, "ERROR Cannot rename Default profile");
        return;
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Rename Profile",
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL,
                                                    "Cancel", GTK_RESPONSE_CANCEL,
                                                    "Rename", GTK_RESPONSE_ACCEPT,
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
            show_status(app, "ERROR Profile name cannot be empty");
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
        edit_iter ? "Edit Curve Point" : "Add Curve Point",
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL,
        edit_iter ? "Update" : "Add",
        GTK_RESPONSE_ACCEPT,
        NULL);
    
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 20);
    
    GtkWidget *temp_label = gtk_label_new("Temperature (°C):");
    GtkWidget *temp_entry = gtk_spin_button_new_with_range(0, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(temp_entry), edit_temp);
    
    GtkWidget *fan_label = gtk_label_new("Fan Speed (%)");
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
            "A curve point with temperature %d°C already exists. Please use a different temperature.",
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
        show_status(app, "Curve point updated (click Apply to save)");
    } else {
        show_status(app, "Curve point added (click Apply to save)");
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
        
        show_status(app, "Curve point deleted (click Apply to save)");
        
        /* Mark curve points as modified */
        app->curve_points_modified = 1;
        
        /* Update the graph visualization */
        update_curve_graph(app);
    } else {
        show_status(app, "Please select a curve point to delete");
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
    
    /* Track whether we blocked the combo box signal (for matching unblock) */
    int signal_blocked = 0;
    
    char buffer[BUFFER_SIZE];
    
    /* Free old profile names */
    if (app->profile_names) {
        for (int i = 0; i < app->num_profiles; i++) {
            g_free(app->profile_names[i]);
        }
        g_free(app->profile_names);
        app->profile_names = NULL;
    }
    
    /* Clear the combo box */
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->profile_combo));
    
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
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->profile_combo), name_start);
                idx++;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    
    g_print("[GUI] Refreshed profile list: %d profiles, active_profile_idx=%d\n", app->num_profiles, app->active_profile_idx);
    
    /* Select the active profile in combo box (block signal to prevent callback).
     * We keep the signal blocked until the end of this function to prevent the
     * callback from firing during early returns when refreshing_profile_list
     * has already been cleared. */
    if (app->active_profile_idx >= 0 && app->active_profile_idx < app->num_profiles) {
        if (app->profile_combo && GTK_IS_COMBO_BOX(app->profile_combo)) {
            g_signal_handler_block(app->profile_combo, app->profile_combo_signal_id);
            signal_blocked = 1;
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->profile_combo), app->active_profile_idx);
            // Note: unblock moved to end of function / early returns
        } else {
            g_print("[GUI] WARNING: profile_combo is invalid\n");
        }
    }
    
    /* Update the Profile Management page with radio buttons */
    /* First, clear all children from profile_list_view and free the radios list */
    if (!app->profile_list_view || !GTK_IS_WIDGET(app->profile_list_view)) {
        g_print("[GUI] WARNING: profile_list_view is NULL or invalid in refresh_profile_list\n");
        app->refreshing_profile_list = 0;
        if (signal_blocked) {
            g_signal_handler_unblock(app->profile_combo, app->profile_combo_signal_id);
        }
        return;
    }
    
    /* Check if widget is realized and valid */
    if (!gtk_widget_get_realized(app->profile_list_view)) {
        g_print("[GUI] WARNING: profile_list_view not realized yet, skipping update\n");
        app->refreshing_profile_list = 0;
        if (signal_blocked) {
            g_signal_handler_unblock(app->profile_combo, app->profile_combo_signal_id);
        }
        return; /* Early return - widget will be refreshed when tab becomes visible */
    }
    
    GtkContainer *container = GTK_CONTAINER(app->profile_list_view);
    GSList *children = NULL;
    children = (GSList *)gtk_container_get_children(container);
    GSList *child;
    for (child = children; child != NULL; child = g_slist_next(child)) {
        if (child->data && GTK_IS_WIDGET(child->data)) {
            /* Don't destroy placeholder_label - it should stay in the container */
            GtkWidget *widget = GTK_WIDGET(child->data);
            const char *name = gtk_widget_get_name(widget);
            if (strcmp(name, "placeholder_label") != 0) {
                gtk_widget_destroy(widget);
            }
        }
    }
    g_slist_free(children);
    
    /* Free the old radios list */
    if (app->profile_radios != NULL) {
        g_slist_free(app->profile_radios);
    }
    app->profile_radios = NULL;
    
    /* Show/hide placeholder based on profile count */
    if (app->placeholder_label && GTK_IS_WIDGET(app->placeholder_label)) {
        if (app->num_profiles == 0) {
            gtk_widget_show(app->placeholder_label);
        } else {
            gtk_widget_hide(app->placeholder_label);
        }
    }
    
    /* Create radio buttons for each profile */
    GtkWidget *first_radio = NULL;
    GtkWidget *pending_radio = NULL; /* Store reference to set selection after creation */
    
    for (int i = 0; i < app->num_profiles; i++) {
        
        char label_text[128];
        if (i == app->active_profile_idx) {
            snprintf(label_text, sizeof(label_text), "%s (Active)", app->profile_names[i]);
        } else {
            snprintf(label_text, sizeof(label_text), "%s", app->profile_names[i]);
        }
        
        GtkWidget *radio;
        if (first_radio == NULL) {
            radio = gtk_radio_button_new_with_label(NULL, label_text);
            first_radio = radio;
        } else {
            radio = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(first_radio), label_text);
        }
        
        /* Set widget name to store profile index (e.g., "prof_0", "prof_1") */
        char widget_name[16];
        snprintf(widget_name, sizeof(widget_name), "prof_%d", i);
        gtk_widget_set_name(radio, widget_name);
        
        /* Connect toggle signal */
        g_signal_connect(radio, "toggled", G_CALLBACK(on_profile_radio_toggled), app);
        
        gtk_box_pack_start(GTK_BOX(app->profile_list_view), radio, FALSE, FALSE, 0);
        
        /* Track for cleanup */
        app->profile_radios = g_slist_append(app->profile_radios, radio);
        
        /* Remember the pending selection to set it after all radios are created */
        if (i == app->pending_active_profile_idx) {
            pending_radio = radio;
        }
    }
    
    /* Set the pending selection as active (after all radios are in the group) */
    if (pending_radio != NULL) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pending_radio), TRUE);
    }
    
    /* Ensure all widgets in profile_list_view are shown (defer to idle to avoid race conditions) */
    if (app->profile_list_view && GTK_IS_WIDGET(app->profile_list_view)) {
        g_idle_add(show_widget_idle, app->profile_list_view);
    } else {
        g_print("[GUI] WARNING: Cannot show profile_list_view, widget is invalid\n");
    }
    
    /* Clear the refreshing flag BEFORE unblocking the signal.
     * This ensures the combo box callback (which loads curve points)
     * does NOT return early when it fires after unblock. */
    get_timestamp(ts, sizeof(ts));
    g_print("%s [GUI] -> Set refreshing_profile_list=0\n", ts);
    app->refreshing_profile_list = 0;
    
    /* Unblock the combo box signal now that refresh is fully complete */
    if (signal_blocked) {
        g_signal_handler_unblock(app->profile_combo, app->profile_combo_signal_id);
    }
    
    /* Update profile tab UI for manual mode state (show/hide warning label and radios) */
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

    /* Build JSON root */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        g_print("[GUI] ERROR: Failed to create JSON root for profile save\n");
        return;
    }
    cJSON_AddStringToObject(root, "version", "1.0");

    /* Build fan_profiles section (nested object) */
    cJSON *fan_section = cJSON_CreateObject();
    cJSON_AddNumberToObject(fan_section, "active_profile_index", app->active_profile_idx);

    /* Query fan daemon for all profiles */
    cJSON *fan_profiles_array = cJSON_CreateArray();
    for (int i = 0; i < app->num_profiles; i++) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "GET_PROFILE %d", i);
        const char *response = send_command(app, cmd);
        if (!response || strncmp(response, "ERROR", 5) == 0) {
            continue;
        }

        /* Parse PROFILE_INFO line: "PROFILE_INFO name=XXX points=N active=N" */
        char *resp_copy = g_strdup(response);
        char *line = strtok(resp_copy, "\n");
        char profile_name[256] = "";
        int is_active = 0;

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
                char *active_ptr = strstr(line, "active=");
                if (active_ptr) {
                    is_active = atoi(active_ptr + 7);
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
    cJSON_AddItemToObject(root, "fan_profiles", fan_section);

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
                    uint32_t hz = (uint32_t)(ghz * 1000000000.0);
                    if (hz > 0) {
                        cJSON_AddNumberToObject(cpu_section, "cpu_hard_limit_hz", (double)hz);
                    }
                }

                cJSON_AddItemToObject(cpu_section, "profiles", cpu_profiles_array);
                cJSON_AddItemToObject(root, "cpu_freq_profiles", cpu_section);
            } else {
                g_print("[GUI] WARNING: Failed to parse CPU profiles JSON\n");
                if (cpu_profiles_array) cJSON_Delete(cpu_profiles_array);
            }
        }
    } else {
        /* CPU not connected: still write an empty cpu section */
        cJSON *cpu_section = cJSON_CreateObject();
        cJSON_AddNumberToObject(cpu_section, "active_profile_index", 0);
        cJSON_AddItemToObject(cpu_section, "profiles", cJSON_CreateArray());
        cJSON_AddItemToObject(root, "cpu_freq_profiles", cpu_section);
    }

    /* Write to file */
    char *json_str = cJSON_Print(root);
    if (json_str) {
        FILE *fp = fopen(profiles_path, "w");
        if (fp) {
            fprintf(fp, "%s\n", json_str);
            fclose(fp);
            g_print("[GUI] Saved profiles to %s\n", profiles_path);
        } else {
            g_print("[GUI] ERROR: Could not write %s: %s\n", profiles_path, strerror(errno));
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
    if (app->profile_radios != NULL) {
        g_slist_free(app->profile_radios);
        app->profile_radios = NULL;
    }
    
    /* Disconnect from daemon */
    disconnect_from_daemon(app);

    /* Disconnect from CPU daemon */
    disconnect_from_cpu_daemon(app);

    gtk_main_quit();
}

/* Connect to daemon */
static void connect_to_daemon(AppData *app) {
    
    /* Check if socket file exists first */
    struct stat st;
    if (stat(SOCKET_PATH, &st) != 0) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    GTK_MESSAGE_ERROR,
                                                    GTK_BUTTONS_OK,
                                                    "Daemon is not running!\n\n"
                                                    "The socket file does not exist at: %s\n\n"
                                                    "To start the daemon, run in a terminal:\n"
                                                    "  sudo dudes-control-center-fan-daemon --foreground\n\n"
                                                    "Or enable the systemd service:\n"
                                                    "  sudo systemctl enable --now dudes-control-center-fan-daemon",
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
                                                    "Failed to create socket: %s",
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
                                                    "Failed to connect to daemon: %s",
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
    struct stat st;
    if (stat(KBD_SOCKET_PATH, &st) != 0) {
        g_print("[GUI] Keyboard daemon socket not found at %s. Keyboard tab will be unavailable.\n", KBD_SOCKET_PATH);
        app->kbd_socket_fd = -1;
        app->kbd_connected = 0;
        if (app->kbd_status_label) {
            gtk_label_set_text(GTK_LABEL(app->kbd_status_label), "Keyboard daemon not connected");
        }
        return;
    }

    app->kbd_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (app->kbd_socket_fd < 0) {
        g_print("[GUI] Failed to create keyboard socket: %s\n", strerror(errno));
        if (app->kbd_status_label) {
            gtk_label_set_text(GTK_LABEL(app->kbd_status_label), "Keyboard daemon socket creation failed");
        }
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
        if (app->kbd_status_label) {
            gtk_label_set_text(GTK_LABEL(app->kbd_status_label), "Keyboard daemon connection failed");
        }
        return;
    }

    app->kbd_connected = 1;
    if (app->kbd_status_label) {
        gtk_label_set_text(GTK_LABEL(app->kbd_status_label), "Keyboard daemon connected");
    }
    g_print("[GUI] Connected to keyboard daemon successfully!\n");

    /* Turn on keyboard backlight at startup */
    send_keyboard_command(app, "TURN_ON");
    g_print("[GUI] Sent TURN_ON at startup\n");
    
    /* Load LED mapping from CSV */
    kbd_load_led_mapping_csv(app);
}

/* Disconnect from keyboard daemon */
static void disconnect_from_keyboard_daemon(AppData *app) {
    if (app->kbd_socket_fd >= 0) {
        close(app->kbd_socket_fd);
        app->kbd_socket_fd = -1;
        app->kbd_connected = 0;
        if (app->kbd_status_label) {
            gtk_label_set_text(GTK_LABEL(app->kbd_status_label), "Keyboard daemon not connected");
        }
    }
}

/* Send command to keyboard daemon */
static const char *send_keyboard_command(AppData *app, const char *cmd) {
    static char response[8192];

    if (!app->kbd_connected || app->kbd_socket_fd < 0) {
        strcpy(response, "ERROR Not connected to keyboard daemon");
        return response;
    }

    char cmd_with_newline[256];
    snprintf(cmd_with_newline, sizeof(cmd_with_newline), "%s\n", cmd);

    ssize_t sent = write(app->kbd_socket_fd, cmd_with_newline, strlen(cmd_with_newline));
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
        int poll_ret = poll(&pfd, 1, 200);
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
        total_received += received;
    }

    return response;
}

/* Load keyboard profiles from daemon */
static void load_keyboard_profiles(AppData *app) {
    if (!app->kbd_connected) {
        g_print("[GUI] Keyboard daemon not connected, skipping profile load\n");
        return;
    }

    const char *response = send_keyboard_command(app, "LIST_PROFILES");
    if (response == NULL || strncmp(response, "ERROR", 5) == 0) {
        g_print("[GUI] Failed to load keyboard profiles: %s\n", response ? response : "(null)");
        return;
    }

    gtk_list_store_clear(app->kbd_profile_store);

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
            profile_count++;
        }

        line = strtok(NULL, "\n");
    }
    g_free(resp_copy);

    /* Select the active profile */
    if (active_idx >= 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->kbd_profile_combo), active_idx);
        app->kbd_active_profile_idx = active_idx;
    }

    /* Update status label */
    if (app->kbd_connected) {
        gtk_label_set_text(GTK_LABEL(app->kbd_status_label),
                          "Keyboard daemon connected");
    }

    g_print("[GUI] Loaded %d keyboard profiles (active idx=%d)\n", profile_count, active_idx);
}

/* Callback: Create new keyboard profile */
static void on_kbd_create_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Create keyboard profile clicked\n");

    if (!app->kbd_connected) {
        g_print("[GUI] Keyboard daemon not connected, skipping\n");
        return;
    }

    /* Show dialog to enter profile name */
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Create Keyboard Profile",
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Create", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);

    GtkWidget *label = gtk_label_new("Enter profile name:");
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
            if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->kbd_profile_store), &iter)) {
                do {
                    gchar *existing_name = NULL;
                    gtk_tree_model_get(GTK_TREE_MODEL(app->kbd_profile_store), &iter, 0, &existing_name, -1);
                    if (existing_name && strcmp(kbd_profile_name_from_display(existing_name), name) == 0) {
                        duplicate = 1;
                        g_free(existing_name);
                        break;
                    }
                    g_free(existing_name);
                } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->kbd_profile_store), &iter));
            }

            if (duplicate) {
                GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                                 GTK_MESSAGE_ERROR,
                                                                 GTK_BUTTONS_OK,
                                                                 "A keyboard profile with the name '%s' already exists.",
                                                                 name);
                gtk_dialog_run(GTK_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
                gtk_widget_destroy(dialog);
                return;
            }

            /* Send CREATE_PROFILE command using :: delimiter for names with spaces */
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "CREATE_PROFILE::%s", name);
            const char *response = send_keyboard_command(app, cmd);
            g_print("[GUI] CREATE_PROFILE response: %s\n", response ? response : "NULL");

            /* Reload the profile list */
            load_keyboard_profiles(app);

            /* Find and select the newly created profile */
            int new_idx = -1;
            if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->kbd_profile_store), &iter)) {
                int i = 0;
                do {
                    gchar *existing_name = NULL;
                    gtk_tree_model_get(GTK_TREE_MODEL(app->kbd_profile_store), &iter, 0, &existing_name, -1);
                    if (existing_name && strcmp(kbd_profile_name_from_display(existing_name), name) == 0) {
                        new_idx = i;
                        g_free(existing_name);
                        break;
                    }
                    g_free(existing_name);
                    i++;
                } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->kbd_profile_store), &iter));
            }

            if (new_idx >= 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(app->kbd_profile_combo), new_idx);
                app->kbd_active_profile_idx = new_idx;
                g_print("[GUI] Selected newly created keyboard profile at index %d\n", new_idx);
            }

            gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label),
                              "Profile created successfully");
        }
    }

    gtk_widget_destroy(dialog);
}

/* Callback: Delete keyboard profile */
static void on_kbd_delete_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Delete keyboard profile clicked\n");

    if (!app->kbd_connected) {
        g_print("[GUI] Keyboard daemon not connected, skipping\n");
        return;
    }

    /* Get currently selected profile */
    int active = gtk_combo_box_get_active(GTK_COMBO_BOX(app->kbd_profile_combo));
    if (active < 0) {
        g_print("[GUI] No profile selected for deletion\n");
        return;
    }

    /* Get the profile name */
    gchar *profile_name = NULL;
    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->kbd_profile_combo), &iter)) {
        g_print("[GUI] Could not get selected profile\n");
        return;
    }
    gtk_tree_model_get(GTK_TREE_MODEL(app->kbd_profile_store), &iter, 0, &profile_name, -1);
    if (!profile_name) {
        g_print("[GUI] Could not get profile name\n");
        return;
    }

    /* Extract clean profile name from display string */
    const char *clean_name = kbd_profile_name_from_display(profile_name);

    /* Show confirmation dialog */
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                GTK_MESSAGE_QUESTION,
                                                GTK_BUTTONS_YES_NO,
                                                "Are you sure you want to delete keyboard profile '%s'?",
                                                clean_name);
    gtk_window_set_title(GTK_WINDOW(dialog), "Delete Profile");

    int result = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (result == GTK_RESPONSE_YES) {
        /* Send DELETE_PROFILE command using :: delimiter for names with spaces */
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "DELETE_PROFILE::%s", clean_name);
        const char *response = send_keyboard_command(app, cmd);
        g_print("[GUI] DELETE_PROFILE response: %s\n", response ? response : "NULL");

        /* Reload the profile list */
        load_keyboard_profiles(app);

        gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label),
                          "Profile deleted successfully");
    }

    g_free(profile_name);
}

/* Callback: Rename keyboard profile */
static void on_kbd_rename_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Rename keyboard profile clicked\n");

    if (!app->kbd_connected) {
        g_print("[GUI] Keyboard daemon not connected, skipping\n");
        return;
    }

    /* Get currently selected profile */
    int active = gtk_combo_box_get_active(GTK_COMBO_BOX(app->kbd_profile_combo));
    if (active < 0) {
        g_print("[GUI] No profile selected for rename\n");
        return;
    }

    /* Get the profile name */
    gchar *old_name = NULL;
    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->kbd_profile_combo), &iter)) {
        g_print("[GUI] Could not get selected profile\n");
        return;
    }
    gtk_tree_model_get(GTK_TREE_MODEL(app->kbd_profile_store), &iter, 0, &old_name, -1);
    if (!old_name) {
        g_print("[GUI] Could not get profile name\n");
        return;
    }

    /* Extract clean profile name from display string */
    const char *clean_old_name = kbd_profile_name_from_display(old_name);

    /* Show dialog to enter new profile name */
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Rename Keyboard Profile",
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Rename", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);

    GtkWidget *label = gtk_label_new("Enter new profile name:");
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), clean_old_name);
    gtk_box_pack_start(GTK_BOX(content), entry, TRUE, TRUE, 0);

    gtk_widget_show_all(content);

    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const gchar *new_name = gtk_entry_get_text(GTK_ENTRY(entry));

        if (strlen(new_name) > 0 && strcmp(new_name, clean_old_name) != 0) {
            /* Check for duplicate profile name */
            int duplicate = 0;
            GtkTreeIter iter2;
            if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->kbd_profile_store), &iter2)) {
                do {
                    gchar *existing_name = NULL;
                    gtk_tree_model_get(GTK_TREE_MODEL(app->kbd_profile_store), &iter2, 0, &existing_name, -1);
                    if (existing_name && strcmp(kbd_profile_name_from_display(existing_name), new_name) == 0) {
                        duplicate = 1;
                        g_free(existing_name);
                        break;
                    }
                    g_free(existing_name);
                } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->kbd_profile_store), &iter2));
            }

            if (duplicate) {
                GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                                 GTK_MESSAGE_ERROR,
                                                                 GTK_BUTTONS_OK,
                                                                 "A keyboard profile with the name '%s' already exists.",
                                                                 new_name);
                gtk_dialog_run(GTK_MESSAGE_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
                gtk_widget_destroy(dialog);
                g_free(old_name);
                return;
            }

            /* Send RENAME_PROFILE command using :: delimiter for names with spaces */
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "RENAME_PROFILE::%s::%s", clean_old_name, new_name);
            const char *response = send_keyboard_command(app, cmd);
            g_print("[GUI] RENAME_PROFILE response: %s\n", response ? response : "NULL");

            /* Reload the profile list */
            load_keyboard_profiles(app);

            gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label),
                              "Profile renamed successfully");
        }
    }

    gtk_widget_destroy(dialog);
    g_free(old_name);
}

/* Callback: Save keyboard profile */
static void on_kbd_save_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Save keyboard profile clicked\n");

    if (!app->kbd_connected) {
        g_print("[GUI] Keyboard daemon not connected, skipping\n");
        return;
    }

    /* Get currently selected profile name */
    int active = gtk_combo_box_get_active(GTK_COMBO_BOX(app->kbd_profile_combo));
    if (active < 0) {
        g_print("[GUI] No keyboard profile selected to save\n");
        gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label), "No profile selected");
        return;
    }

    gchar *profile_name = NULL;
    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->kbd_profile_combo), &iter)) {
        g_print("[GUI] Could not get selected profile\n");
        return;
    }
    gtk_tree_model_get(GTK_TREE_MODEL(app->kbd_profile_store), &iter, 0, &profile_name, -1);
    if (!profile_name) {
        g_print("[GUI] Could not get profile name\n");
        return;
    }

    /* Extract clean profile name from display string */
    const char *clean_name = kbd_profile_name_from_display(profile_name);

    /* Send SAVE_PROFILE command using :: delimiter for names with spaces */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "SAVE_PROFILE::%s", clean_name);
    const char *response = send_keyboard_command(app, cmd);
    g_print("[GUI] SAVE_PROFILE response: %s\n", response ? response : "NULL");

    /* Reload the profile list */
    load_keyboard_profiles(app);

    gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label),
                      g_strconcat("Profile saved: ", clean_name, NULL));

    g_free(profile_name);
}

/* Callback: Activate keyboard profile */
static void on_kbd_activate_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = (AppData *)user_data;
    g_print("[GUI] Activate keyboard profile clicked\n");

    if (!app->kbd_connected) {
        g_print("[GUI] Keyboard daemon not connected, skipping\n");
        return;
    }

    /* Get currently selected profile name */
    int active = gtk_combo_box_get_active(GTK_COMBO_BOX(app->kbd_profile_combo));
    if (active < 0) {
        g_print("[GUI] No keyboard profile selected to activate\n");
        gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label), "No profile selected");
        return;
    }

    gchar *profile_name = NULL;
    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->kbd_profile_combo), &iter)) {
        g_print("[GUI] Could not get selected profile\n");
        return;
    }
    gtk_tree_model_get(GTK_TREE_MODEL(app->kbd_profile_store), &iter, 0, &profile_name, -1);
    if (!profile_name) {
        g_print("[GUI] Could not get profile name\n");
        return;
    }

    /* Extract clean profile name from display string */
    const char *clean_name = kbd_profile_name_from_display(profile_name);

    /* Send LOAD_PROFILE command using :: delimiter for names with spaces */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "LOAD_PROFILE::%s", clean_name);
    const char *response = send_keyboard_command(app, cmd);
    g_print("[GUI] LOAD_PROFILE response: %s\n", response ? response : "NULL");

    /* Reload the profile list to update the [*] marker */
    load_keyboard_profiles(app);

    gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label),
                      g_strconcat("Activated profile: ", clean_name, NULL));

    g_free(profile_name);
}

/* Callback: Keyboard backlight off switch toggled */
static gboolean on_kbd_off_switch_toggled(GtkSwitch *sw, gboolean new_state, gpointer user_data) {
    (void)sw;
    AppData *app = (AppData *)user_data;
    gboolean is_on = new_state; /* Use new_state, gtk_switch_get_state() returns old state during signal */

    if (!app->kbd_connected) {
        g_print("[GUI] Keyboard daemon not connected, skipping off switch toggle\n");
        return;
    }

    if (is_on) {
        /* Turn off all LEDs */
        app->kbd_off_active = 1;
        const char *response = send_keyboard_command(app, "TURN_OFF");
        g_print("[GUI] TURN_OFF response: %s\n", response ? response : "NULL");
        gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label), "Backlight turned OFF");
        g_print("[GUI] Keyboard backlight turned OFF (override active)\n");
    } else {
        /* Turn backlight back on with default color */
        app->kbd_off_active = 0;
        const char *response = send_keyboard_command(app, "TURN_ON");
        g_print("[GUI] TURN_ON response: %s\n", response ? response : "NULL");
        gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label), "Backlight turned ON");
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
    AppData *app = (AppData *)user_data;
    
    /* Initialize wizard state */
    app->kbd_wizard_active = 1;
    app->kbd_wizard_current_led = 0;
    memset(app->kbd_wizard_mapping, -1, sizeof(app->kbd_wizard_mapping));
    
    /* Update UI */
    gtk_widget_set_sensitive(app->kbd_led_wizard_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_skip_btn, TRUE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_save_btn, TRUE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_cancel_btn, TRUE);
    
    /* Show first step */
    kbd_wizard_step(app, NULL);
}

/* LED Wizard: Skip current LED */
static void on_led_wizard_skip_clicked(GtkWidget *widget, gpointer user_data) {
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

/* LED Wizard: Save & Finish */
static void on_led_wizard_save_finish_clicked(GtkWidget *widget, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    
    app->kbd_wizard_active = 0;
    
    /* Restore all LEDs */
    send_keyboard_command(app, "TURN_ON");
    
    /* Save mapping to CSV */
    kbd_save_led_mapping_csv(app);
    
    /* Update UI */
    gtk_widget_set_sensitive(app->kbd_led_wizard_btn, TRUE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_skip_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_save_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_cancel_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(app->kbd_led_wizard_label), "LED Wizard saved!");
    
    /* Clear selection and redraw */
    app->kbd_num_selected_leds = 0;
    kbd_redraw_canvas(app);
}

/* LED Wizard: Cancel */
static void on_led_wizard_cancel_clicked(GtkWidget *widget, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    
    app->kbd_wizard_active = 0;
    
    /* Restore all LEDs */
    send_keyboard_command(app, "TURN_ON");
    
    /* Update UI */
    gtk_widget_set_sensitive(app->kbd_led_wizard_btn, TRUE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_skip_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_save_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_cancel_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(app->kbd_led_wizard_label), "LED Wizard cancelled");
    
    /* Clear selection and redraw */
    app->kbd_num_selected_leds = 0;
    kbd_redraw_canvas(app);
}

/* LED Wizard: Update UI for current step */
static void kbd_wizard_step(AppData *app, const char *prev_key_name) {
    char buf[256];
    
    /* Build label showing previous assignment and current LED */
    if (prev_key_name && app->kbd_wizard_current_led > 0) {
        snprintf(buf, sizeof(buf), "LED %d -> %s | Click key for LED %d / 119",
                 app->kbd_wizard_current_led - 1, prev_key_name, app->kbd_wizard_current_led);
    } else {
        snprintf(buf, sizeof(buf), "LED Wizard: Click the key for LED %d / 119", app->kbd_wizard_current_led);
    }
    gtk_label_set_text(GTK_LABEL(app->kbd_led_wizard_label), buf);
    
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
    
    /* Update UI */
    gtk_widget_set_sensitive(app->kbd_led_wizard_btn, TRUE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_skip_btn, FALSE);
    gtk_widget_set_sensitive(app->kbd_led_wizard_cancel_btn, FALSE);
    gtk_label_set_text(GTK_LABEL(app->kbd_led_wizard_label), "LED Wizard completed!");
    
    /* Clear selection and redraw */
    app->kbd_num_selected_leds = 0;
    kbd_redraw_canvas(app);
    
    /* Save mapping to CSV */
    kbd_save_led_mapping_csv(app);
}

/* Save LED mapping to CSV file */
static void kbd_save_led_mapping_csv(AppData *app) {
    char csv_path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(csv_path, sizeof(csv_path), "%s/.config/dudes-control-center/keyboard_led_mapping.csv", home);
    
    FILE *f = fopen(csv_path, "w");
    if (!f) {
        g_print("[GUI] WARNING: Could not write LED mapping CSV to %s\n", csv_path);
        return;
    }
    
    /* Write header (semicolon delimiter) */
    fprintf(f, "led_id;position;led_index\n");
    
    /* Track which positions have been written and count LEDs per position */
    int pos_led_count[120] = {0};
    
    /* First pass: count LEDs per position */
    for (int led = 0; led < 120; led++) {
        int pos = app->kbd_wizard_mapping[led];
        if (pos >= 0 && pos < 120) {
            pos_led_count[pos]++;
        }
    }
    
    /* Second pass: write entries */
    int pos_led_written[120] = {0};
    for (int led = 0; led < 120; led++) {
        int pos = app->kbd_wizard_mapping[led];
        if (pos >= 0 && pos < 120) {
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
}

/* Load LED mapping from CSV file */
static void kbd_load_led_mapping_csv(AppData *app) {
    char csv_path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(csv_path, sizeof(csv_path), "%s/.config/dudes-control-center/keyboard_led_mapping.csv", home);
    
    FILE *f = fopen(csv_path, "r");
    if (!f) {
        g_print("[GUI] No LED mapping CSV found at %s\n", csv_path);
        return;
    }
    
    char line[256];
    /* Skip header line */
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return;
    }
    
    /* Initialize mapping */
    memset(app->kbd_wizard_mapping, -1, sizeof(app->kbd_wizard_mapping));
    
    while (fgets(line, sizeof(line), f)) {
        /* Remove newline */
        line[strcspn(line, "\n")] = 0;
        
        /* Parse: led_id;position;led_index (semicolon delimiter) */
        int led_id = -1;
        int position = -1;
        
        char *token = strtok(line, ";");
        if (token) led_id = atoi(token);
        token = strtok(NULL, ";");
        if (token) position = atoi(token);
        
        if (led_id < 0 || led_id >= 120) continue;
        if (position < 0 || position >= 120) continue; /* Unmapped LED */
        
        app->kbd_wizard_mapping[led_id] = position;
    }
    
    fclose(f);
    g_print("[GUI] LED mapping loaded from %s\n", csv_path);
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
        double scaled_h = img_height * scale;
        double offset_x = (width - scaled_w) / 2.0;
        double offset_y = (height - scaled_h) / 2.0;
        
        cairo_save(cr);
        cairo_translate(cr, offset_x, offset_y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, app->kbd_keyboard_image, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        
        /* Draw key overlays */
        size_t num_keys = sizeof(kbd_key_positions) / sizeof(kbd_key_positions[0]);
        for (size_t i = 0; i < num_keys; i++) {
            const KeyPos *key = &kbd_key_positions[i];
            
            /* Check if this LED is selected */
            int led_id = kbd_position_to_led_id[i];
            int is_selected = 0;
            for (int s = 0; s < app->kbd_num_selected_leds; s++) {
                if (app->kbd_selected_led_indices[s] == led_id) {
                    is_selected = 1;
                    break;
                }
            }
            
            /* Also check if this position has a mapping from CSV (loaded wizard data) - only when wizard is not active */
            int has_mapping = 0;
            if (!app->kbd_wizard_active) {
                for (int led = 0; led < 120; led++) {
                    if (app->kbd_wizard_mapping[led] == i) {
                        has_mapping = 1;
                        break;
                    }
                }
            }
            
            /* Draw overlay */
            if (is_selected || has_mapping) {
                /* Selected keys - green highlight, mapped keys - blue highlight */
                if (is_selected) {
                    cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 0.3);
                } else {
                    cairo_set_source_rgba(cr, 0.0, 0.0, 1.0, 0.25);
                }
                cairo_rectangle(cr,
                    offset_x + key->x * scale,
                    offset_y + key->y * scale,
                    key->w * scale,
                    key->h * scale);
                cairo_fill(cr);
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
    } else {
        /* No image loaded, show placeholder text */
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, width/2 - 80, height/2);
        cairo_show_text(cr, "Keyboard schematic loading...");
        cairo_stroke(cr);
    }
    
    return FALSE;
}

/* Click handler for keyboard canvas */
static gboolean kbd_canvas_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    
    if (!app->kbd_keyboard_image || event->button != 1) {
        return FALSE;
    }
    
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    double img_width = cairo_image_surface_get_width(app->kbd_keyboard_image);
    double img_height = cairo_image_surface_get_height(app->kbd_keyboard_image);
    
    /* Same scaling as draw callback */
    double scale_x = (double)width / img_width;
    double scale_y = (double)height / img_height;
    double scale = (scale_x < scale_y) ? scale_x : scale_y;
    
    double scaled_w = img_width * scale;
    double scaled_h = img_height * scale;
    double offset_x = (width - scaled_w) / 2.0;
    double offset_y = (height - scaled_h) / 2.0;
    
    /* Convert click to image coordinates */
    double unscaled_x = (event->x - offset_x) / scale;
    double unscaled_y = (event->y - offset_y) / scale;
    
    /* Find which key was clicked */
    size_t num_keys = sizeof(kbd_key_positions) / sizeof(kbd_key_positions[0]);
    for (size_t i = 0; i < num_keys; i++) {
        const KeyPos *key = &kbd_key_positions[i];
        if (unscaled_x >= key->x && unscaled_x <= key->x + key->w &&
            unscaled_y >= key->y && unscaled_y <= key->y + key->h) {
            
            int led_id = kbd_position_to_led_id[i];
            
            /* Handle wizard mode */
            if (app->kbd_wizard_active) {
                /* Assign this key position to the current LED */
                int current_led = app->kbd_wizard_current_led;
                app->kbd_wizard_mapping[current_led] = (int)i;
                const char *key_name = (i < 120) ? kbd_key_names[i] : "Unknown";
                
                /* Highlight using the actual LED ID (not the wizard step index) */
                app->kbd_num_selected_leds = 1;
                app->kbd_selected_led_indices[0] = led_id;
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
            
            /* Normal mode: toggle selection */
            int found = 0;
            for (int s = 0; s < app->kbd_num_selected_leds; s++) {
                if (app->kbd_selected_led_indices[s] == led_id) {
                    /* Remove from selection */
                    app->kbd_selected_led_indices[s] = app->kbd_selected_led_indices[app->kbd_num_selected_leds - 1];
                    app->kbd_num_selected_leds--;
                    found = 1;
                    break;
                }
            }
            
            if (!found && app->kbd_num_selected_leds < 120) {
                /* Add to selection */
                app->kbd_selected_led_indices[app->kbd_num_selected_leds++] = led_id;
            }
            
            /* Redraw canvas */
            kbd_redraw_canvas(app);
            return TRUE;
        }
    }
    
    return FALSE;
}

/* Callback: Preset color button clicked */
static void on_preset_color_clicked(GtkWidget *widget, AppData *app) {
    if (!app->kbd_connected) {
        g_print("[GUI] Keyboard daemon not connected, skipping preset color\n");
        return;
    }

    int *rgb = (int *)g_object_get_data(G_OBJECT(widget), "preset-rgb");
    if (!rgb) {
        g_print("[GUI] No RGB data for preset button\n");
        return;
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "SET_COLOR %d %d %d", rgb[0], rgb[1], rgb[2]);
    const char *response = send_keyboard_command(app, cmd);
    g_print("[GUI] SET_COLOR %d %d %d response: %s\n", rgb[0], rgb[1], rgb[2], response ? response : "NULL");

    /* Update status label */
    const char *name = gtk_button_get_label(GTK_BUTTON(widget));
    gtk_label_set_text(GTK_LABEL(app->kbd_profile_status_label),
                      g_strconcat("Color set to: ", name, NULL));
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
    int auto_mode = 1, fan_duty = 0;
    
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
        } else if (strncmp(token, "FAN_DUTY=", 11) == 0) {
            fan_duty = atoi(token + 11);
            app->last_known_fan_duty = fan_duty; /* Cache for manual mode */
        }
        token = strtok(NULL, "\n");
    }
    
    /* Update labels */
    char cpu_text[32], gpu_text[32], cpu_rpm_text[32], gpu_rpm_text[32];
    snprintf(cpu_text, sizeof(cpu_text), "CPU: %d°C", cpu_temp);
    snprintf(gpu_text, sizeof(gpu_text), "GPU: %d°C", gpu_temp);
    snprintf(cpu_rpm_text, sizeof(cpu_rpm_text), "CPU Fan: %d RPM (%d%%)", cpu_fan_rpm, (cpu_fan_rpm * 100) / 6022);
    snprintf(gpu_rpm_text, sizeof(gpu_rpm_text), "GPU Fan: %d RPM (%d%%)", gpu_fan_rpm, (gpu_fan_rpm * 100) / 5859);
    
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
    
    /* Title */
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='large' weight='bold'>Fan Status &amp; Manual Control</span>");
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 0);
    
    /* Temperature display */
    GtkWidget *temp_frame = gtk_frame_new("Temperatures");
    GtkWidget *temp_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(temp_vbox), 15);
    
    app->cpu_temp_label = gtk_label_new("CPU: --°C");
    gtk_widget_set_halign(app->cpu_temp_label, GTK_ALIGN_START);
    GtkWidget *gpu_temp_label_holder = gtk_label_new("GPU: --°C");
    gtk_widget_set_halign(gpu_temp_label_holder, GTK_ALIGN_START);
    
    app->cpu_temp_label = gtk_label_new("CPU: --°C");
    gtk_widget_set_halign(app->cpu_temp_label, GTK_ALIGN_START);
    app->gpu_temp_label = gtk_label_new("GPU: --°C");
    gtk_widget_set_halign(app->gpu_temp_label, GTK_ALIGN_START);
    
    gtk_box_pack_start(GTK_BOX(temp_vbox), app->cpu_temp_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(temp_vbox), app->gpu_temp_label, FALSE, FALSE, 0);
    
    gtk_container_add(GTK_CONTAINER(temp_frame), temp_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), temp_frame, FALSE, FALSE, 0);
    
    /* Fan speed display */
    GtkWidget *fan_frame = gtk_frame_new("Fan Speed");
    GtkWidget *fan_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(fan_vbox), 15);
    
    app->cpu_fan_rpm_label = gtk_label_new("CPU Fan: -- RPM (--%)");
    gtk_widget_set_halign(app->cpu_fan_rpm_label, GTK_ALIGN_START);
    app->gpu_fan_rpm_label = gtk_label_new("GPU Fan: -- RPM (--%)");
    gtk_widget_set_halign(app->gpu_fan_rpm_label, GTK_ALIGN_START);
    
    gtk_box_pack_start(GTK_BOX(fan_vbox), app->cpu_fan_rpm_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fan_vbox), app->gpu_fan_rpm_label, FALSE, FALSE, 0);
    
    gtk_container_add(GTK_CONTAINER(fan_frame), fan_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), fan_frame, FALSE, FALSE, 0);
    
    /* Manual control */
    GtkWidget *manual_frame = gtk_frame_new("Manual Fan Control");
    GtkWidget *manual_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_container_set_border_width(GTK_CONTAINER(manual_vbox), 15);
    
    /* Manual mode switch */
    GtkWidget *switch_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    app->manual_mode_switch = GTK_WIDGET(gtk_switch_new());
    GtkWidget *switch_label = gtk_label_new("Manual Mode:");
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
    GtkWidget *info_label = gtk_label_new("Note: Enable Manual Mode to override automatic fan control. Changes take effect immediately.");
    gtk_label_set_line_wrap(GTK_LABEL(info_label), TRUE);
    gtk_box_pack_start(GTK_BOX(manual_vbox), info_label, FALSE, FALSE, 0);
    
    gtk_container_add(GTK_CONTAINER(manual_frame), manual_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), manual_frame, FALSE, FALSE, 0);
    
    /* Per-Core CPU Frequencies */
    {
        GtkWidget *core_frame = gtk_frame_new("Per-Core CPU Frequencies");
        GtkWidget *core_scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(core_scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(core_scrolled), 400);
        gtk_container_set_border_width(GTK_CONTAINER(core_scrolled), 5);
        gtk_container_add(GTK_CONTAINER(core_frame), core_scrolled);
        gtk_box_pack_start(GTK_BOX(vbox), core_frame, TRUE, TRUE, 0);
        
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

/* Create Page 2: Curve Editor */
static GtkWidget *create_curve_editor_page(AppData *app) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 20);
    
    /* Title */
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='large' weight='bold'>Curve Editor</span>");
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 0);
    
    /* Profile selection */
    GtkWidget *profile_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *profile_label = gtk_label_new("Profile:");
    app->profile_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->profile_combo), "Default");
    
    /* Connect profile change to load curve points and store handler ID */
    app->profile_combo_signal_id = g_signal_connect(app->profile_combo, "changed", G_CALLBACK(on_profile_combo_changed), app);
    
    gtk_box_pack_start(GTK_BOX(profile_box), profile_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(profile_box), app->profile_combo, TRUE, TRUE, 0);
    
    /* Create profile button */
    GtkWidget *create_btn = gtk_button_new_with_label("New Profile");
    g_signal_connect(create_btn, "clicked", G_CALLBACK(on_create_profile_clicked), app);
    gtk_box_pack_end(GTK_BOX(profile_box), create_btn, FALSE, FALSE, 0);
    
    /* Delete profile button */
    GtkWidget *delete_profile_btn = gtk_button_new_with_label("Delete Profile");
    g_signal_connect(delete_profile_btn, "clicked", G_CALLBACK(on_delete_profile_clicked), app);
    gtk_box_pack_end(GTK_BOX(profile_box), delete_profile_btn, FALSE, FALSE, 0);

    /* Rename profile button */
    GtkWidget *rename_profile_btn = gtk_button_new_with_label("Rename Profile");
    g_signal_connect(rename_profile_btn, "clicked", G_CALLBACK(on_rename_profile_clicked), app);
    gtk_box_pack_end(GTK_BOX(profile_box), rename_profile_btn, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(vbox), profile_box, FALSE, FALSE, 0);
    
    /* Create horizontal box for tree view and graph side by side */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    
    /* Left side: Curve points tree view */
    GtkWidget *tree_frame = gtk_frame_new("Temperature Curve Points");
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    
    /* Create list store with columns: Temperature (text), Fan % (int) */
    app->curve_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
    
    app->curve_treeview = GTK_WIDGET(gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->curve_store)));
    
    /* Temperature column */
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *temp_col = gtk_tree_view_column_new_with_attributes(
        "Temperature", renderer, "text", 0, NULL);
    gtk_tree_view_column_set_fixed_width(temp_col, 80);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->curve_treeview), temp_col);
    
    /* Fan % column */
    renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *fan_col = gtk_tree_view_column_new_with_attributes(
        "Fan Speed (%)", renderer, "text", 1, NULL);
    gtk_tree_view_column_set_fixed_width(fan_col, 70);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->curve_treeview), fan_col);
    
    /* Connect double-click to edit curve point */
    g_signal_connect(app->curve_treeview, "row-activated",
                    G_CALLBACK(on_curve_point_activated), app);
    
    gtk_container_add(GTK_CONTAINER(scrolled), app->curve_treeview);
    gtk_container_add(GTK_CONTAINER(tree_frame), scrolled);
    gtk_box_pack_start(GTK_BOX(hbox), tree_frame, FALSE, FALSE, 0);
    
    /* Right side: Temperature curve graph */
    GtkWidget *graph_frame = gtk_frame_new("Temperature Curve Graph");
    app->curve_graph = gtk_drawing_area_new();
    gtk_widget_set_size_request(app->curve_graph, 1000, 430);
    
    /* Connect draw signal */
    g_signal_connect(app->curve_graph, "draw", G_CALLBACK(draw_curve_graph), app);
    
    gtk_container_add(GTK_CONTAINER(graph_frame), app->curve_graph);
    gtk_box_pack_start(GTK_BOX(hbox), graph_frame, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    
    /* Curve point controls */
    GtkWidget *point_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    
    GtkWidget *add_btn = gtk_button_new_with_label("Add Point");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_curve_point_clicked), app);
    gtk_box_pack_start(GTK_BOX(point_controls), add_btn, FALSE, FALSE, 0);
    
    GtkWidget *delete_btn = gtk_button_new_with_label("Delete Point");
    g_signal_connect(delete_btn, "clicked", G_CALLBACK(on_delete_curve_point_clicked), app);
    gtk_box_pack_start(GTK_BOX(point_controls), delete_btn, FALSE, FALSE, 0);
    
    GtkWidget *control_label = gtk_label_new("  (Double-click to edit, points are auto-sorted by temperature)");
    gtk_box_pack_start(GTK_BOX(point_controls), control_label, FALSE, FALSE, 10);
    
    gtk_box_pack_start(GTK_BOX(vbox), point_controls, FALSE, FALSE, 5);
    
    /* Cancel and Apply buttons */
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), app);
    GtkWidget *apply_btn = gtk_button_new_with_label("Apply Changes");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_apply_clicked), app);
    gtk_box_pack_start(GTK_BOX(button_box), cancel_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(button_box), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), button_box, FALSE, FALSE, 15);
    
    return vbox;
}

/* Update the curve graph by triggering a redraw */
static void update_curve_graph(AppData *app) {
    if (app->curve_graph) {
        gtk_widget_queue_draw(app->curve_graph);
    }
}

/* Draw callback for the temperature curve graph using Cairo */
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
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);  /* White */
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
    
    /* Draw grid lines and axes */
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);  /* Light gray for grid */
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
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);  /* Black for axes */
    cairo_set_line_width(cr, 2.0);
    
    /* X-axis (temperature) */
    cairo_move_to(cr, left_margin, height - bottom_margin);
    cairo_line_to(cr, width - right_margin, height - bottom_margin);
    
    /* Y-axis (fan speed) */
    cairo_move_to(cr, left_margin, top_margin);
    cairo_line_to(cr, left_margin, height - bottom_margin);
    cairo_stroke(cr);
    
    /* Draw axis labels */
    cairo_set_font_size(cr, 10.0);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    
    /* Y-axis labels (fan speed %) - every 20% */
    for (int i = 0; i <= 5; i++) {
        double y = top_margin + (graph_height * (1.0 - (i * 0.2)));
        char label[16];
        snprintf(label, sizeof(label), "%d%%", i * 20);
        cairo_move_to(cr, left_margin - 55, y + 4);
        cairo_show_text(cr, label);
    }
    
    /* X-axis labels (temperature °C) - every 10°C */
    for (int i = 0; i <= 10; i++) {
        double x = left_margin + (graph_width * (i * 0.1));
        char label[16];
        snprintf(label, sizeof(label), "%d", i * 10);
        cairo_move_to(cr, x - 5, height - bottom_margin + 15);
        cairo_show_text(cr, label);
    }
    
    /* Axis titles */
    cairo_set_font_size(cr, 12.0);
    cairo_save(cr);
    cairo_translate(cr, width / 2, height - bottom_margin + 35);
    cairo_rotate(cr, 0);
    cairo_move_to(cr, -40, 0);
    cairo_show_text(cr, "Temperature (°C)");
    cairo_restore(cr);
    
    cairo_save(cr);
    cairo_translate(cr, 15, height / 2);
    cairo_rotate(cr, -M_PI_2);
    cairo_move_to(cr, -10, 0);
    cairo_show_text(cr, "Fan Speed (%)");
    cairo_restore(cr);
    
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
        
        cairo_arc(cr, x, y, 5.0, 0, 2 * M_PI);
        cairo_fill(cr);
        
        /* White outline for better visibility */
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_arc(cr, x, y, 5.0, 0, 2 * M_PI);
        cairo_stroke(cr);
    }
    
    return TRUE;
}

/* Callback: Profile radio button toggled - store pending selection */
static void on_profile_radio_toggled(GtkWidget *widget, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    
    /* Get the profile index from the button's name */
    const gchar *name = gtk_widget_get_name(widget);
    int idx = atoi(name + 5); /* Skip "prof_" prefix */
    
    /* Only act if this radio button is now active */
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
        /* Store pending selection (will be applied when Apply button is clicked) */
        app->pending_active_profile_idx = idx;
        show_status(app, "Profile selected - click Apply to activate");
    }
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
    
    /* Fall back to pending selection for Page 3 */
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
        
        /* Refresh profile list to update indicators on Page 3 */
        refresh_profile_list(app);
        
        /* Update tray menu to reflect new active profile */
        update_tray_menu(app);

        /* Persist the active_profile_index to profiles.json */
        gui_save_profiles_to_disk(app);
    } else {
        show_status(app, "No profile selected");
    }
}

/* Callback: Cancel profile selection */
static void on_cancel_profile_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget; /* Unused */
    AppData *app = (AppData *)user_data;
    
    /* Reset pending selection to current active profile */
    app->pending_active_profile_idx = app->active_profile_idx;
    
    /* Update labels on existing radio buttons without full refresh */
    GSList *node = app->profile_radios;
    int idx = 0;
    while (node != NULL && idx < app->num_profiles) {
        GtkWidget *radio = (GtkWidget *)node->data;
        char label_text[128];
        if (idx == app->active_profile_idx) {
            snprintf(label_text, sizeof(label_text), "%s (Active)", app->profile_names[idx]);
        } else {
            snprintf(label_text, sizeof(label_text), "%s", app->profile_names[idx]);
        }
        gtk_button_set_label(GTK_BUTTON(radio), label_text);
        node = g_slist_next(node);
        idx++;
    }
    
    /* Set the radio button for active profile as selected */
    if (app->profile_radios != NULL && app->active_profile_idx >= 0) {
        GSList *node2 = app->profile_radios;
        int idx2 = 0;
        while (node2 != NULL && idx2 < app->num_profiles) {
            if (idx2 == app->active_profile_idx) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(node2->data), TRUE);
                break;
            }
            node2 = g_slist_next(node2);
            idx2++;
        }
    }
    
    show_status(app, "Selection cancelled");
}

/* Update the profile tab UI based on manual mode state */
static void update_profile_tab_for_manual_mode(AppData *app, int manual_mode_enabled) {
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    g_print("%s [GUI] Updating profile tab for manual mode: %s\n", ts, manual_mode_enabled ? "ENABLED" : "DISABLED");
    g_print("%s [GUI] -> app->manual_mode_warning_label = %p\n", ts, (void*)app->manual_mode_warning_label);
    g_print("%s [GUI] -> app->disable_manual_mode_btn = %p\n", ts, (void*)app->disable_manual_mode_btn);
    g_print("%s [GUI] -> app->profile_radios = %p\n", ts, (void*)app->profile_radios);
    
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
    
    /* Enable/disable all profile radio buttons */
    if (app->profile_radios != NULL) {
        GSList *node;
        for (node = app->profile_radios; node != NULL; node = g_slist_next(node)) {
            GtkWidget *radio = GTK_WIDGET(node->data);
            gtk_widget_set_sensitive(radio, !manual_mode_enabled);
        }
    }
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
                show_status(app, "Manual mode disabled - returned to auto mode");
            } else {
                show_status(app, "Failed to reactivate previous profile");
            }
        }
        
        /* Update the profile tab UI */
        update_profile_tab_for_manual_mode(app, 0);
    } else {
        show_status(app, "Failed to disable manual mode");
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
    
    /* Check if we're leaving the Curve Editor tab (page 1) with unsaved changes */
    gint current_page = gtk_notebook_get_current_page(notebook);
    if (current_page == 1 && page_num != 1 && app->curve_points_modified) {
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
    
    /* Clear status messages when switching tabs */
    if (current_page != page_num) {
        show_status(app, "");
        
        /* Clear CPU profile status label when leaving the CPU Profiles tab */
        if (current_page == 3 && app->cpu_freq_profile_status_label) {
            gtk_label_set_text(GTK_LABEL(app->cpu_freq_profile_status_label), "");
        }
    }

    /* Page 2 is the Curve Editor tab (0-indexed: Status=0, Curve Editor=1, Profiles=2) */
    if (page_num == 1) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Switching to Curve Editor tab\n", ts);
        
        /* Load curve points for the currently active profile */
        load_curve_points_for_profile(app, app->active_profile_idx);
    }
    
    /* Page 3 is the Profiles tab (0-indexed: Status=0, Curve Editor=1, Profiles=2) */
    if (page_num == 2) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Switching to Profiles tab\n", ts);
        
        /* Sync pending selection with actual active profile from daemon */
        app->pending_active_profile_idx = app->active_profile_idx;
        g_print("%s [GUI] -> Synced pending_active_profile_idx=%d with active_profile_idx=%d\n",
                ts, app->pending_active_profile_idx, app->active_profile_idx);
        
        /* Defer refresh until GTK has realized the widgets on this page.
         * GTK3 lazy-realizes notebook pages: the page is only realized after
         * the first switch-page signal fires. By using g_idle_add, we ensure
         * the widgets are fully realized before refresh_profile_list() checks.
         * This fixes the bug where profiles appear empty on first visit. */
        g_idle_add(refresh_profile_tab_idle, app);
    }

    /* Page 3 is the CPU Profiles tab (0-indexed: Status=0, Curve Editor=1, Profiles=2, CPU Profiles=3) */
    if (page_num == 3) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Switching to CPU Profiles tab\n", ts);


        /* Load CPU custom profiles and select the active one */
        load_cpu_custom_profiles(app);

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
    if (page_num == 4) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        g_print("%s [GUI] Switching to Keyboard Backlight tab\n", ts);

        /* Load keyboard profiles */
        load_keyboard_profiles(app);
        
        /* Redraw canvas to show loaded LED mapping */
        if (app->kbd_keyboard_canvas) {
            gtk_widget_queue_draw(app->kbd_keyboard_canvas);
        }
    }
}

/* Create Page 3: Profile List */
static GtkWidget *create_profile_list_page(AppData *app) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 20);
    
    /* Title */
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='large' weight='bold'>Profile Management</span>");
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 0);
    
    /* Info label */
    GtkWidget *info_label = gtk_label_new("Select which profile is currently active. The active profile controls your fan behavior.");
    gtk_label_set_line_wrap(GTK_LABEL(info_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), info_label, FALSE, FALSE, 0);
    
    /* Manual mode warning label (initially hidden) */
    app->manual_mode_warning_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app->manual_mode_warning_label), "<span size='x-large' weight='bold' foreground='#d9534f'>Manual Mode Active!</span>");
    gtk_widget_set_halign(app->manual_mode_warning_label, GTK_ALIGN_CENTER);
    gtk_widget_hide(app->manual_mode_warning_label); /* Hide by default */
    gtk_box_pack_start(GTK_BOX(vbox), app->manual_mode_warning_label, FALSE, FALSE, 0);
    
    /* Disable Manual Mode button (initially hidden) */
    app->disable_manual_mode_btn = gtk_button_new_with_label("Disable Manual Mode");
    gtk_widget_set_halign(app->disable_manual_mode_btn, GTK_ALIGN_CENTER);
    g_signal_connect(app->disable_manual_mode_btn, "clicked", G_CALLBACK(on_disable_manual_mode_clicked), app);
    gtk_widget_hide(app->disable_manual_mode_btn); /* Hide by default */
    gtk_box_pack_start(GTK_BOX(vbox), app->disable_manual_mode_btn, FALSE, FALSE, 0);
    
    /* Profile list frame */
    GtkWidget *frame = gtk_frame_new("Available Fan Profiles");
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    
    app->profile_list_view = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(app->profile_list_view), 15);
    gtk_widget_set_valign(app->profile_list_view, GTK_ALIGN_START);
    
    /* Add a placeholder label */
    GtkWidget *placeholder = gtk_label_new("No profiles available. Create one in the Curve Editor page.");
    gtk_widget_set_name(placeholder, "placeholder_label"); /* Set name so we can skip it during cleanup */
    gtk_box_pack_start(GTK_BOX(app->profile_list_view), placeholder, FALSE, FALSE, 0);
    app->placeholder_label = placeholder; /* Store reference for later removal */
    
    gtk_container_add(GTK_CONTAINER(scrolled), app->profile_list_view);
    gtk_container_add(GTK_CONTAINER(frame), scrolled);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    
    /* Apply and Cancel buttons */
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
    gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), button_box, FALSE, FALSE, 0);
    
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_profile_clicked), app);
    gtk_box_pack_start(GTK_BOX(button_box), cancel_btn, FALSE, FALSE, 0);
    
    GtkWidget *apply_btn = gtk_button_new_with_label("Apply");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_apply_profile_clicked), app);
    gtk_box_pack_start(GTK_BOX(button_box), apply_btn, FALSE, FALSE, 0);
    
    return vbox;
}

/* Callback for clickable links - destroys the duplicated URL string */
static gboolean on_link_clicked(GtkWidget *widget __attribute__((unused)), GdkEventButton *event, gpointer user_data) {
    if (event->button == 1) { /* Left click */
        const gchar *url = (const gchar *)user_data;
        GError *error = NULL;
        gboolean success = g_app_info_launch_default_for_uri(url, NULL, &error);
        if (!success && error) {
            g_printerr("[GUI] Failed to open URI: %s\n", error->message);
            g_error_free(error);
        }
        /* Free the duplicated URL string */
        g_free((gpointer)url);
        return TRUE; /* Event handled */
    }
    return FALSE; /* Event not handled */
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
    g_signal_connect(event_box, "button-press-event", G_CALLBACK(on_link_clicked), g_strdup(url));
    
    return event_box;
}

/* Forward declarations for log viewer */
static gboolean refresh_logs_timer(gpointer user_data);
static void load_gui_logs(AppData *app);
static void load_daemon_logs(AppData *app);
static void load_cpu_daemon_logs(AppData *app);

/* Create Page 4: Log Viewer */
static GtkWidget *create_log_page(AppData *app) {
    /* Create a notebook for tabbed daemon logs */
    GtkWidget *notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(notebook), 5);
    
    /* Tab 1: GUI Log */
    {
        GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 300);
        
        app->gui_log_view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(app->gui_log_view), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->gui_log_view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->gui_log_view), 5);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->gui_log_view), 5);
        gtk_style_context_add_class(gtk_widget_get_style_context(app->gui_log_view), "monospace-log-view");
        app->gui_log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->gui_log_view));
        gtk_container_add(GTK_CONTAINER(scrolled), app->gui_log_view);
        
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled, gtk_label_new("GUI Log"));
    }
    
    /* Tab 2: Fan Daemon Log */
    {
        GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 300);
        
        app->daemon_log_view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(app->daemon_log_view), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->daemon_log_view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->daemon_log_view), 5);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->daemon_log_view), 5);
        gtk_style_context_add_class(gtk_widget_get_style_context(app->daemon_log_view), "monospace-log-view");
        app->daemon_log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->daemon_log_view));
        gtk_container_add(GTK_CONTAINER(scrolled), app->daemon_log_view);
        
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled, gtk_label_new("Fan Daemon Log"));
    }
    
    /* Tab 3: CPU Daemon Log */
    {
        GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 300);
        
        app->cpu_daemon_log_view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(app->cpu_daemon_log_view), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->cpu_daemon_log_view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->cpu_daemon_log_view), 5);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->cpu_daemon_log_view), 5);
        gtk_style_context_add_class(gtk_widget_get_style_context(app->cpu_daemon_log_view), "monospace-log-view");
        app->cpu_daemon_log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->cpu_daemon_log_view));
        gtk_container_add(GTK_CONTAINER(scrolled), app->cpu_daemon_log_view);
        
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled, gtk_label_new("CPU Daemon Log"));
    }
    
    /* Start the log refresh timer (every 500ms) */
    app->log_refresh_timer_id = g_timeout_add(500, refresh_logs_timer, app);
    
    /* Initial load of logs */
    load_gui_logs(app);
    load_daemon_logs(app);
    load_cpu_daemon_logs(app);
    
    return notebook;
}

/* Timer callback to refresh logs periodically */
static gboolean refresh_logs_timer(gpointer user_data) {
    AppData *app = (AppData *)user_data;
    
    /* Check if we're on the log page before refreshing to save resources */
    gint current_page = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    if (current_page == 4) {  /* Log page is index 4 (0-based: Status=0, Curve=1, Profiles=2, CPU=3, Logs=4, About=5) */
        load_gui_logs(app);
        load_daemon_logs(app);
        load_cpu_daemon_logs(app);
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
    
    fseek(f, last_file_size, SEEK_SET);
    size_t bytes_read = fread(new_content, 1, new_size, f);
    new_content[bytes_read] = '\0';
    fclose(f);
    
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
    
    last_file_size = file_size;
    
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
                "<span foreground='#e74c3c'>Unable to read daemon log. Check permissions or ensure the daemon is running.</span>\n", -1);
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
    
    fseek(f, last_file_size, SEEK_SET);
    size_t bytes_read = fread(new_content, 1, new_size, f);
    new_content[bytes_read] = '\0';
    fclose(f);
    
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
    
    last_file_size = file_size;
    
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
                "<span foreground='#e74c3c'>Unable to read CPU daemon log. Check permissions or ensure the daemon is running.</span>\n", -1);
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
    
    fseek(f, last_file_size, SEEK_SET);
    size_t bytes_read = fread(new_content, 1, new_size, f);
    new_content[bytes_read] = '\0';
    fclose(f);
    
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
    
    last_file_size = file_size;
    
    gtk_text_buffer_get_start_iter(app->cpu_daemon_log_buffer, &start_iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->cpu_daemon_log_view), &start_iter, 0.0, FALSE, 0.0, 0.0);
}

/* Create Page 5: About */
static GtkWidget *create_about_page(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 30);
    
    /* Main title with fancy styling */
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='xx-large' weight='bold' foreground='#2c3e50'>Dudes Control Center</span>");
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 0);
    
    /* Version info */
    GtkWidget *version = gtk_label_new(NULL);
    {
        char version_text[256];
        snprintf(version_text, sizeof(version_text), "<span size='large' foreground='#7f8c8d'>Version %s</span>", APP_VERSION);
        gtk_label_set_markup(GTK_LABEL(version), version_text);
    }
    gtk_widget_set_halign(version, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), version, FALSE, FALSE, 5);
    
    /* Separator */
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sep1, FALSE, FALSE, 8);
    
    /* AI-powered message */
    GtkWidget *ai_text = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(ai_text), "<span size='large' weight='bold'>This application was completely coded by AI</span>");
    gtk_widget_set_halign(ai_text, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), ai_text, FALSE, FALSE, 8);
    
    /* Separator */
    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sep2, FALSE, FALSE, 10);
    
    /* Thanks container with spacing */
    GtkWidget *thanks_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_halign(thanks_vbox, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), thanks_vbox, FALSE, FALSE, 15);
    
    /* Thank you to Bionic AI Agent */
    GtkWidget *thanks1_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_halign(thanks1_vbox, GTK_ALIGN_CENTER);
    GtkWidget *thanks1_text = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(thanks1_text), "<span size='medium'>Special thanks to the creators of <b>Bionic AI Agent</b></span>");
    gtk_box_pack_start(GTK_BOX(thanks1_vbox), thanks1_text, TRUE, FALSE, 0);
    GtkWidget *thanks1_link = create_clickable_link("https://lmstudio.ai/");
    gtk_box_pack_start(GTK_BOX(thanks1_vbox), thanks1_link, TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(thanks_vbox), thanks1_vbox, TRUE, FALSE, 0);
    
    /* Thank you to original fan-control creator */
    GtkWidget *thanks2_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_halign(thanks2_vbox, GTK_ALIGN_CENTER);
    GtkWidget *thanks2_text = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(thanks2_text), "<span size='medium'>Grateful to the original creator of <b>fan-control</b></span>");
    gtk_box_pack_start(GTK_BOX(thanks2_vbox), thanks2_text, TRUE, FALSE, 0);
    GtkWidget *thanks2_link = create_clickable_link("https://github.com/steiiin/fan-control");
    gtk_box_pack_start(GTK_BOX(thanks2_vbox), thanks2_link, TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(thanks_vbox), thanks2_vbox, TRUE, FALSE, 0);
    
    /* Thank you to Linus Torvalds */
    GtkWidget *thanks3_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_halign(thanks3_vbox, GTK_ALIGN_CENTER);
    GtkWidget *thanks3_text = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(thanks3_text), "<span size='medium'>Honoring <b>Linus Torvalds</b>, the visionary founder of Linux</span>");
    gtk_box_pack_start(GTK_BOX(thanks3_vbox), thanks3_text, TRUE, FALSE, 0);
    GtkWidget *thanks3_link = create_clickable_link("https://torvalds-family.blogspot.com/");
    gtk_box_pack_start(GTK_BOX(thanks3_vbox), thanks3_link, TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(thanks_vbox), thanks3_vbox, TRUE, FALSE, 0);
    
    /* Thank you to Linux Community */
    GtkWidget *thanks4 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(thanks4), "<span size='medium'>Appreciation for the <b>Great Linux Community</b></span>");
    gtk_widget_set_halign(thanks4, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(thanks_vbox), thanks4, TRUE, FALSE, 0);
    
    /* Thank you to Clevo */
    GtkWidget *thanks5_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_halign(thanks5_vbox, GTK_ALIGN_CENTER);
    GtkWidget *thanks5_text = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(thanks5_text), "<span size='medium'>Thanks to <b>Clevo</b> for producing &amp; selling awesome Laptops</span>");
    gtk_box_pack_start(GTK_BOX(thanks5_vbox), thanks5_text, TRUE, FALSE, 0);
    GtkWidget *thanks5_link = create_clickable_link("https://www.clevo.com.tw/");
    gtk_box_pack_start(GTK_BOX(thanks5_vbox), thanks5_link, TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(thanks_vbox), thanks5_vbox, TRUE, FALSE, 0);
    
    /* Contact info section */
    GtkWidget *contact_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_halign(contact_sep, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(thanks_vbox), contact_sep, FALSE, FALSE, 25);
    
    GtkWidget *contact_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(contact_title), "<span size='large' weight='bold' foreground='#2c3e50'>Contact</span>");
    gtk_widget_set_halign(contact_title, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(thanks_vbox), contact_title, TRUE, FALSE, 10);
    
    GtkWidget *contact_name = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(contact_name), "<span size='medium' weight='bold'>Dudemeister</span>");
    gtk_widget_set_halign(contact_name, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(thanks_vbox), contact_name, TRUE, FALSE, 5);
    
    GtkWidget *contact_github_text = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(contact_github_text), "<span size='small'>GitHub:</span>");
    gtk_widget_set_halign(contact_github_text, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(thanks_vbox), contact_github_text, TRUE, FALSE, 2);
    
    GtkWidget *contact_github = create_clickable_link("https://github.com/dudewayne/Dudes-Control-Center");
    gtk_box_pack_start(GTK_BOX(thanks_vbox), contact_github, TRUE, FALSE, 0);
    
    return vbox;
}

/* Create Page 6: CPU Control */
static GtkWidget *create_cpu_page(AppData *app) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 20);

    /* Section: CPU Info */
    GtkWidget *info_frame = gtk_frame_new("CPU Information");
    GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(info_box), 10);
    gtk_container_add(GTK_CONTAINER(info_frame), info_box);
    gtk_box_pack_start(GTK_BOX(vbox), info_frame, FALSE, FALSE, 0);

    /* CPU Temperature */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new("CPU Temperature:");
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_temp_label = gtk_label_new("N/A");
        gtk_widget_set_halign(app->cpu_page_temp_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_temp_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_box), row, FALSE, FALSE, 0);
    }

    /* CPU Load */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new("CPU Load:");
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_load_label = gtk_label_new("N/A");
        gtk_widget_set_halign(app->cpu_page_load_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_load_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_box), row, FALSE, FALSE, 0);
    }

    /* Number of CPUs */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new("Number of CPUs:");
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_num_label = gtk_label_new("N/A");
        gtk_widget_set_halign(app->cpu_page_num_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_num_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_box), row, FALSE, FALSE, 0);
    }

    /* Current Frequency */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new("Current Frequency:");
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_freq_label = gtk_label_new("N/A");
        gtk_widget_set_halign(app->cpu_page_freq_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_freq_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_box), row, FALSE, FALSE, 0);
    }

    /* Min/Max Frequency */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new("Min / Max Frequency:");
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_min_max_freq_label = gtk_label_new("N/A");
        gtk_widget_set_halign(app->cpu_page_min_max_freq_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_min_max_freq_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_box), row, FALSE, FALSE, 0);
    }

    /* Active Governor */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new("Active Governor:");
        gtk_widget_set_halign(label, GTK_ALIGN_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 20);
        gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
        app->cpu_page_governor_label = gtk_label_new("N/A");
        gtk_widget_set_halign(app->cpu_page_governor_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), app->cpu_page_governor_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(info_box), row, FALSE, FALSE, 0);
    }

    /* Section: Governor Profiles */
    GtkWidget *profile_frame = gtk_frame_new("Governor Profiles");
    GtkWidget *profile_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(profile_box), 10);
    gtk_container_add(GTK_CONTAINER(profile_frame), profile_box);
    gtk_box_pack_start(GTK_BOX(vbox), profile_frame, FALSE, FALSE, 0);

    /* Profile combo box */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new("Select Profile:");
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
        GtkWidget *freq_frame = gtk_frame_new("Per-Core Frequency Profiles");
        GtkWidget *freq_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_container_set_border_width(GTK_CONTAINER(freq_box), 10);
        gtk_container_add(GTK_CONTAINER(freq_frame), freq_box);
        gtk_box_pack_start(GTK_BOX(vbox), freq_frame, FALSE, FALSE, 0);

        /* Profile combo box */
        {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            GtkWidget *label = gtk_label_new("Frequency Profile:");
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
            app->cpu_apply_all_cores_checkbox = gtk_check_button_new_with_label("Apply values to all cores");
            gtk_box_pack_start(GTK_BOX(freq_box), app->cpu_apply_all_cores_checkbox, FALSE, FALSE, 0);
            gtk_widget_set_sensitive(app->cpu_apply_all_cores_checkbox, FALSE); /* Disabled until profile selected */
            app->cpu_apply_all_cores_signal_id = g_signal_connect(app->cpu_apply_all_cores_checkbox, "toggled", G_CALLBACK(on_cpu_apply_all_cores_toggled), app);
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

        /* Create/Delete/Apply buttons */
        {
            GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_box_pack_start(GTK_BOX(freq_box), btn_box, FALSE, FALSE, 0);

            app->cpu_freq_profile_create_btn = gtk_button_new_with_label("Create Profile");
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_create_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_create_btn, "clicked", G_CALLBACK(on_cpu_create_freq_profile_clicked), app);

            app->cpu_freq_profile_delete_btn = gtk_button_new_with_label("Delete Profile");
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_delete_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_delete_btn, "clicked", G_CALLBACK(on_cpu_delete_freq_profile_clicked), app);
            gtk_widget_set_sensitive(app->cpu_freq_profile_delete_btn, FALSE); /* Disabled until profile selected */

            app->cpu_freq_profile_rename_btn = gtk_button_new_with_label("Rename Profile");
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_rename_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_rename_btn, "clicked", G_CALLBACK(on_cpu_rename_freq_profile_clicked), app);
            gtk_widget_set_sensitive(app->cpu_freq_profile_rename_btn, FALSE); /* Disabled until profile selected */

            app->cpu_freq_profile_save_btn = gtk_button_new_with_label("Save Profile");
            gtk_box_pack_start(GTK_BOX(btn_box), app->cpu_freq_profile_save_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_freq_profile_save_btn, "clicked", G_CALLBACK(on_cpu_save_freq_profile_clicked), app);
            gtk_widget_set_sensitive(app->cpu_freq_profile_save_btn, FALSE); /* Disabled until profile selected */

            app->cpu_freq_profile_activate_btn = gtk_button_new_with_label("Activate Profile");
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
        GtkWidget *limit_frame = gtk_frame_new("Hard Frequency Limit");
        GtkWidget *limit_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_container_set_border_width(GTK_CONTAINER(limit_box), 10);
        gtk_container_add(GTK_CONTAINER(limit_frame), limit_box);
        gtk_box_pack_start(GTK_BOX(vbox), limit_frame, FALSE, FALSE, 0);

        /* Warning label */
        {
            GtkWidget *warning_label = gtk_label_new("");
            gtk_label_set_markup(GTK_LABEL(warning_label),
                "<span foreground='#e74c3c' weight='bold'>WARNING:</span> "
                "This is a safety limit to protect your hardware. "
                "Setting it too high may cause CPU instability, overheating, or permanent damage. "
                "Only increase this if you know your CPU's safe operating limits.");
            gtk_label_set_line_wrap(GTK_LABEL(warning_label), TRUE);
            gtk_widget_set_hexpand(warning_label, TRUE);
            gtk_box_pack_start(GTK_BOX(limit_box), warning_label, FALSE, FALSE, 0);
        }

        /* Hard limit spinner (in GHz, step 0.1, range 0.5 - 8.0) */
        {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            GtkWidget *label = gtk_label_new("Hard Limit (GHz):");
            gtk_widget_set_halign(label, GTK_ALIGN_END);
            gtk_label_set_width_chars(GTK_LABEL(label), 20);
            gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);

            app->cpu_hard_limit_adjustment = gtk_adjustment_new(3.6, 0.5, 8.0, 0.1, 0.5, 0);
            app->cpu_hard_limit_spinner = gtk_spin_button_new(app->cpu_hard_limit_adjustment, 0.1, 1);
            gtk_widget_set_halign(app->cpu_hard_limit_spinner, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(row), app->cpu_hard_limit_spinner, FALSE, FALSE, 0);

            app->cpu_hard_limit_apply_btn = gtk_button_new_with_label("Apply");
            gtk_widget_set_halign(app->cpu_hard_limit_apply_btn, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(row), app->cpu_hard_limit_apply_btn, FALSE, FALSE, 0);
            g_signal_connect(app->cpu_hard_limit_apply_btn, "clicked", G_CALLBACK(on_cpu_hard_limit_apply_clicked), app);

            gtk_box_pack_start(GTK_BOX(limit_box), row, FALSE, FALSE, 0);
        }
    }

    return vbox;
}

/* Create Page 7: Keyboard Backlight Control */
static GtkWidget *create_keyboard_page(AppData *app) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    /* --- Status section --- */
    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    app->kbd_status_label = gtk_label_new("Keyboard daemon not connected");
    gtk_label_set_xalign(GTK_LABEL(app->kbd_status_label), 0.0);
    gtk_widget_set_hexpand(app->kbd_status_label, TRUE);
    gtk_box_pack_start(GTK_BOX(status_box), app->kbd_status_label, FALSE, FALSE, 0);

    GtkWidget *status_frame = gtk_frame_new("Keyboard Status");
    gtk_container_set_border_width(GTK_CONTAINER(status_frame), 6);
    gtk_container_add(GTK_CONTAINER(status_frame), status_box);
    gtk_box_pack_start(GTK_BOX(vbox), status_frame, FALSE, FALSE, 0);

    /* --- Profile management section --- */
    GtkWidget *profile_frame = gtk_frame_new("Keyboard Profiles");
    gtk_container_set_border_width(GTK_CONTAINER(profile_frame), 6);
    GtkWidget *profile_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    /* Profile combo box */
    GtkWidget *profile_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_label_set_xalign(GTK_LABEL(gtk_label_new("Profile:")), 0.0);
    GtkWidget *profile_label = gtk_label_new("Profile:");
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
    gtk_box_pack_start(GTK_BOX(profile_hbox), app->kbd_profile_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(profile_vbox), profile_hbox, FALSE, FALSE, 0);

    /* Profile buttons row */
    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->kbd_profile_create_btn = gtk_button_new_from_stock(GTK_STOCK_ADD);
    g_signal_connect(app->kbd_profile_create_btn, "clicked", G_CALLBACK(on_kbd_create_profile_clicked), app);
    app->kbd_profile_delete_btn = gtk_button_new_from_stock(GTK_STOCK_REMOVE);
    g_signal_connect(app->kbd_profile_delete_btn, "clicked", G_CALLBACK(on_kbd_delete_profile_clicked), app);
    app->kbd_profile_rename_btn = gtk_button_new_with_label("Rename");
    g_signal_connect(app->kbd_profile_rename_btn, "clicked", G_CALLBACK(on_kbd_rename_profile_clicked), app);
    app->kbd_profile_save_btn = gtk_button_new_with_label("Save Profile");
    g_signal_connect(app->kbd_profile_save_btn, "clicked", G_CALLBACK(on_kbd_save_profile_clicked), app);
    app->kbd_profile_activate_btn = gtk_button_new_with_label("Activate Profile");
    g_signal_connect(app->kbd_profile_activate_btn, "clicked", G_CALLBACK(on_kbd_activate_profile_clicked), app);
    gtk_box_pack_start(GTK_BOX(btn_row), app->kbd_profile_create_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_row), app->kbd_profile_delete_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_row), app->kbd_profile_rename_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_row), app->kbd_profile_save_btn, FALSE, FALSE, 0);
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
    GtkWidget *off_switch_frame = gtk_frame_new("Backlight Power");
    gtk_container_set_border_width(GTK_CONTAINER(off_switch_frame), 6);
    GtkWidget *off_switch_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    GtkWidget *off_switch_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *off_switch_label = gtk_label_new("Backlight Off:");
    gtk_label_set_xalign(GTK_LABEL(off_switch_label), 0.0);
    gtk_box_pack_start(GTK_BOX(off_switch_hbox), off_switch_label, FALSE, FALSE, 0);

    app->kbd_off_switch = gtk_switch_new();
    gtk_switch_set_state(GTK_SWITCH(app->kbd_off_switch), FALSE);
    g_signal_connect(app->kbd_off_switch, "state-set", G_CALLBACK(on_kbd_off_switch_toggled), app);
    gtk_box_pack_start(GTK_BOX(off_switch_hbox), app->kbd_off_switch, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(off_switch_vbox), off_switch_hbox, FALSE, FALSE, 0);

    GtkWidget *off_switch_note = gtk_label_new("When ON, overrides all profile settings until disabled.");
    gtk_widget_set_name(off_switch_note, "kbd-off-switch-note");
    gtk_label_set_xalign(GTK_LABEL(off_switch_note), 0.0);
    gtk_box_pack_start(GTK_BOX(off_switch_vbox), off_switch_note, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(off_switch_frame), off_switch_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), off_switch_frame, FALSE, FALSE, 0);

    /* --- Preset Colors section --- */
    GtkWidget *preset_frame = gtk_frame_new("Preset Colors");
    gtk_container_set_border_width(GTK_CONTAINER(preset_frame), 6);
    GtkWidget *preset_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    GtkWidget *preset_label = gtk_label_new("Quick color for entire keyboard:");
    gtk_label_set_xalign(GTK_LABEL(preset_label), 0.0);
    gtk_box_pack_start(GTK_BOX(preset_vbox), preset_label, FALSE, FALSE, 0);

    GtkWidget *preset_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    /* Define preset colors: {label, R, G, B} */
    const char *preset_names[] = {"White", "Red", "Green", "Blue", "Yellow", "Cyan", "Magenta", "Orange", "Purple"};
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

    gtk_box_pack_start(GTK_BOX(preset_vbox), preset_hbox, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(preset_frame), preset_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), preset_frame, FALSE, FALSE, 0);

    /* --- Visual Zone Editor section --- */
    GtkWidget *zone_frame = gtk_frame_new("Visual Zone Editor");
    gtk_container_set_border_width(GTK_CONTAINER(zone_frame), 6);
    GtkWidget *zone_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    /* Zone grid (zone name/color entries) */
    app->kbd_zone_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(app->kbd_zone_grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(app->kbd_zone_grid), 4);

    GtkWidget *zone_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(zone_scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(zone_scrolled), 50);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(zone_scrolled), 50);
    gtk_container_add(GTK_CONTAINER(zone_scrolled), app->kbd_zone_grid);
    gtk_box_pack_start(GTK_BOX(zone_vbox), zone_scrolled, FALSE, FALSE, 0);

    /* LED Wizard controls */
    GtkWidget *wizard_btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->kbd_led_wizard_btn = gtk_button_new_with_label("LED Wizard");
    g_signal_connect(app->kbd_led_wizard_btn, "clicked", G_CALLBACK(on_led_wizard_start_clicked), app);
    app->kbd_led_wizard_skip_btn = gtk_button_new_with_label("Skip");
    g_signal_connect(app->kbd_led_wizard_skip_btn, "clicked", G_CALLBACK(on_led_wizard_skip_clicked), app);
    gtk_widget_set_sensitive(app->kbd_led_wizard_skip_btn, FALSE);
    app->kbd_led_wizard_cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(app->kbd_led_wizard_cancel_btn, "clicked", G_CALLBACK(on_led_wizard_cancel_clicked), app);
    gtk_widget_set_sensitive(app->kbd_led_wizard_cancel_btn, FALSE);
    app->kbd_led_wizard_save_btn = gtk_button_new_with_label("Save & Finish");
    g_signal_connect(app->kbd_led_wizard_save_btn, "clicked", G_CALLBACK(on_led_wizard_save_finish_clicked), app);
    gtk_widget_set_sensitive(app->kbd_led_wizard_save_btn, FALSE);
    gtk_box_pack_start(GTK_BOX(wizard_btn_row), app->kbd_led_wizard_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wizard_btn_row), app->kbd_led_wizard_skip_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wizard_btn_row), app->kbd_led_wizard_save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wizard_btn_row), app->kbd_led_wizard_cancel_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(zone_vbox), wizard_btn_row, FALSE, FALSE, 0);
    
    /* LED Wizard status label */
    app->kbd_led_wizard_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->kbd_led_wizard_label), 0.0);
    gtk_widget_set_name(app->kbd_led_wizard_label, "kbd-wizard-label");
    gtk_box_pack_start(GTK_BOX(zone_vbox), app->kbd_led_wizard_label, FALSE, FALSE, 0);
    
    /* Keyboard schematic canvas */
    app->kbd_keyboard_canvas = gtk_drawing_area_new();
    gtk_widget_set_events(app->kbd_keyboard_canvas, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(app->kbd_keyboard_canvas, "draw", G_CALLBACK(kbd_canvas_draw), app);
    g_signal_connect(app->kbd_keyboard_canvas, "button-press-event", G_CALLBACK(kbd_canvas_button_press), app);
    gtk_box_pack_start(GTK_BOX(zone_vbox), app->kbd_keyboard_canvas, TRUE, TRUE, 0);

    /* Load keyboard image */
    app->kbd_keyboard_image = cairo_image_surface_create_from_png("/usr/share/doc/dudes-control-center/images/keyboard_dark.png");
    if (cairo_surface_status(app->kbd_keyboard_image) != CAIRO_STATUS_SUCCESS) {
        g_print("[GUI] WARNING: Could not load keyboard image: %s\n", 
                cairo_status_to_string(cairo_surface_status(app->kbd_keyboard_image)));
        cairo_surface_destroy(app->kbd_keyboard_image);
        app->kbd_keyboard_image = NULL;
        gtk_widget_set_size_request(app->kbd_keyboard_canvas, -1, 200); /* fallback height */
    }

    /* Add zone / Apply buttons row */
    GtkWidget *zone_btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->kbd_add_zone_btn = gtk_button_new_with_label("Add Zone");
    app->kbd_apply_zones_btn = gtk_button_new_with_label("Apply Zones");
    gtk_box_pack_start(GTK_BOX(zone_btn_row), app->kbd_add_zone_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(zone_btn_row), app->kbd_apply_zones_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(zone_vbox), zone_btn_row, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(zone_frame), zone_vbox);
    gtk_box_pack_start(GTK_BOX(vbox), zone_frame, TRUE, TRUE, 0);

    return vbox;
}

/* Create main window */
static GtkWidget *create_main_window(AppData *app) {
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "Dudes Control Center");
    
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
    GtkWidget *page3 = create_profile_list_page(app);
    GtkWidget *page4 = create_log_page(app);
    GtkWidget *cpu_page = create_cpu_page(app);
    /* Wrap CPU page in a scrolled window so all sections are reachable */
    GtkWidget *cpu_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cpu_scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(cpu_scrolled), cpu_page);
    GtkWidget *page6 = create_about_page();
    GtkWidget *kbd_page = create_keyboard_page(app);
    /* Wrap keyboard page in a scrolled window */
    GtkWidget *kbd_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(kbd_scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(kbd_scrolled), kbd_page);

    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page1, gtk_label_new("Status"));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page2, gtk_label_new("Curve Editor"));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page3, gtk_label_new("Fan Profiles"));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), cpu_scrolled, gtk_label_new("CPU Profiles"));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), kbd_scrolled, gtk_label_new("Keyboard Backlight"));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page4, gtk_label_new("Logs"));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page6, gtk_label_new("About"));
    
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
    
    /* Fallback to common development locations */
    if (!pixbuf) {
        g_print("[GUI] Trying development icon location...\n");
        pixbuf = gdk_pixbuf_new_from_file_at_size("/home/dude/Code/ai projects/Dudes Control Center/DudesControlCenter.png", 48, 48, NULL);
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
    GtkWidget *show_item = gtk_menu_item_new_with_label("Show Application");
    g_signal_connect(show_item, "activate", G_CALLBACK(on_tray_show_app), app);
    gtk_container_add(GTK_CONTAINER(menu), show_item);
    gtk_widget_show(show_item);
    
    /* Separator */
    GtkWidget *separator1 = gtk_separator_menu_item_new();
    gtk_container_add(GTK_CONTAINER(menu), separator1);
    gtk_widget_show(separator1);
    
    /* CPU Profiles submenu (alphabetical: C before F) */
    if (app->cpu_connected) {
        GtkWidget *cpu_profiles_menuitem = gtk_menu_item_new_with_label("CPU Profiles");
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
                    snprintf(label, sizeof(label), "%s (Active)", clean_name);
                } else {
                    snprintf(label, sizeof(label), "%s", clean_name);
                }
                g_free(raw_name);
            } else {
                snprintf(label, sizeof(label), "Profile %d", i);
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
    {
        GtkWidget *profiles_menuitem = gtk_menu_item_new_with_label("Fan Profiles");
        GtkWidget *profiles_submenu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(profiles_menuitem), profiles_submenu);
        gtk_container_add(GTK_CONTAINER(menu), profiles_menuitem);
        gtk_widget_show(profiles_menuitem);
        
        GtkWidget *first_profile_radio = NULL;
        for (int i = 0; i < app->num_profiles; i++) {
            char label[128];
            if (i == app->active_profile_idx) {
                snprintf(label, sizeof(label), "%s (Active)", app->profile_names[i]);
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
    
    /* Separator */
    GtkWidget *separator2 = gtk_separator_menu_item_new();
    gtk_container_add(GTK_CONTAINER(menu), separator2);
    gtk_widget_show(separator2);
    
    /* Version label (greyed out) - wrapped in menu item */
    {
        char version_label[256];
        snprintf(version_label, sizeof(version_label), "Version %s", APP_VERSION);
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
    GtkWidget *exit_item = gtk_menu_item_new_with_label("Exit");
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
            
            /* If we're on the Profiles tab (page 2), defer refresh until widgets are realized.
             * gtk_widget_show_all() triggers realization asynchronously. */
            gint current_page = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
            if (current_page == 2) {
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
            
            /* If we're on the Profiles tab (page 2), defer refresh until widgets are realized.
             * gtk_widget_show_all() triggers realization asynchronously. */
            gint current_page = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
            if (current_page == 2) {
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
            show_status(app, g_strdup_printf("Profile '%s' activated", app->profile_names[selected_idx]));
            
            /* Update active profile index */
            app->active_profile_idx = selected_idx;
            app->pending_active_profile_idx = selected_idx;
            
            /* Refresh profile list to update indicators on Page 3 */
            refresh_profile_list(app);
            
            /* Update tray menu to reflect new active profile */
            update_tray_menu(app);
        } else {
            show_status(app, "Failed to activate profile");
        }
    }
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
            show_status(app, g_strdup_printf("CPU profile '%s' activated", profile_name ? profile_name : "Unknown"));
            
            /* Update active index */
            app->cpu_active_custom_profile_idx = selected_idx;
            
            /* Reload profile list to update markers */
            load_cpu_custom_profiles(app);
            
            /* Persist the active_profile_index */
            gui_save_profiles_to_disk(app);
            
            /* Update tray menu to reflect new active profile */
            update_tray_menu(app);
        } else {
            show_status(app, "Failed to activate CPU profile");
        }
    }
    
    if (profile_name) g_free(profile_name);
}

/* Save window position to config.json */
static void save_window_position(AppData *app) {
    const char *config_dir = g_get_user_config_dir();
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/dudes-control-center/config.json", config_dir);
    
    cJSON *json = cJSON_CreateObject();
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
static gboolean refresh_timer_callback(gpointer user_data) {
    AppData *app = (AppData *)user_data;
    char ts[32];
    get_timestamp(ts, sizeof(ts));
    
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
            
            /* Check if process is still running */
            if (kill(old_pid, 0) == 0) {
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
    /* Initialize logging first (NULL = use default user directory) */
    if (log_init("GUI", LOG_LEVEL_INFO, NULL) != 0) {
        fprintf(stderr, "Failed to initialize logging system\n");
    }
    
    LOG_INFO("Application starting...");
    
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
    
    gtk_init(&argc, &argv);
    
    g_print("[GUI] GTK initialized successfully\n");
    g_print("[GUI] DISPLAY=%s\n", g_getenv("DISPLAY"));
    g_print("[GUI] XDG_RUNTIME_DIR=%s\n", g_getenv("XDG_RUNTIME_DIR"));
    
    AppData app = {0};
    
    /* Create main window */
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

    /* Start CPU refresh timer (every 5 seconds) if connected */
    if (app.cpu_connected) {
        app.cpu_refresh_timer_id = g_timeout_add_seconds(5, cpu_refresh_timer_callback, &app);
        g_print("[GUI] CPU refresh timer started (5s interval)\n");

        /* Load governors into combo box */
        load_cpu_governors(&app);

        /* Send an immediate STATUS query so cpu_core_count is available before the user switches tabs */
        {
            const char *immediate_status = send_cpu_command(&app, "STATUS");
            if (immediate_status != NULL && strncmp(immediate_status, "ERROR", 5) != 0) {
                parse_cpu_status(&app, immediate_status);
            }
        }

        /* Load current hard limit from daemon */
        {
            const char *hard_resp = send_cpu_command(&app, "GET_HARD_LIMIT");
            if (hard_resp && strncmp(hard_resp, "OK", 2) == 0) {
                const char *eq = strstr(hard_resp, "HARD_LIMIT=");
                if (eq) {
                    uint32_t hz = (uint32_t)strtoul(eq + 11, NULL, 10);
                    double ghz = (double)hz / 1000000000.0;
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.cpu_hard_limit_spinner), ghz);
                    g_print("[GUI] Loaded hard limit from daemon: %.2f GHz (%u Hz)\n", ghz, hz);
                    /* Spinner upper bounds are updated in build_cpu_core_freq_grid() */
                }
            }
        }
    }

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
