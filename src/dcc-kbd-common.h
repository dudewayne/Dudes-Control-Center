// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc-kbd-common.h
 * Common header for Keyboard Backlight Control in Dudes Control Center
 */

#ifndef DUDES_CONTROL_CENTER_KBD_COMMON_H
#define DUDES_CONTROL_CENTER_KBD_COMMON_H

#include <stdint.h>

/* --- Socket configuration --- */
#ifndef KBD_SOCKET_PATH
#define KBD_SOCKET_PATH "/run/dcc-kbd.sock"
#endif
#ifndef KBD_LEDS_DIR
#define KBD_LEDS_DIR "/sys/class/leds"
#endif
/* Remembers "backlight switched off" across daemon restarts and reboots */
#ifndef KBD_STATE_DIR
#define KBD_STATE_DIR "/var/lib/dudes-control-center"
#endif
#define KBD_OFF_FLAG_FILE KBD_STATE_DIR "/kbd_backlight_off"
#define KBD_BRIGHTNESS_FILE KBD_STATE_DIR "/kbd_brightness"
#ifndef KBD_LOG_DIR
#define KBD_LOG_DIR "/var/log/dudes-control-center"
#endif
#define KBD_BUFFER_SIZE 8192
/* Long enough for a list of every LED index (SET_KEYS_COLOR, CREATE_ZONE) */
#define KBD_MAX_CMD_LEN 4096
#define KBD_MAX_RESPONSE_LEN 8192
#define KBD_MAX_CLIENTS 10

/* --- LED configuration --- */
#define KBD_MAX_LEDS 256
#define KBD_LED_NAME_MAX 128
#define KBD_LED_PATH_MAX 512

/* --- Zone and Profile configuration --- */
#define KBD_MAX_ZONES_PER_PROFILE 32
#define KBD_MAX_PROFILES 64
#define KBD_ZONE_NAME_MAX 64
#define KBD_PROFILE_NAME_MAX 64
#define KBD_MAX_KEYS_PER_ZONE 256

/* --- Color modes --- */
typedef enum {
    KBD_LED_MODE_OFF = 0,
    KBD_LED_MODE_STATIC = 1,
    KBD_LED_MODE_BREATHING = 2,
    KBD_LED_MODE_RAINBOW = 3,
    KBD_LED_MODE_WAVE = 4
} KbdLedMode;

/* --- Animated effects (1.35.0) ---
 * One effect per zone and for the base color, stored with the profile.
 * Text form: "static", "breathing:<speed>", "rainbow:<speed>",
 * "wave:<speed>:<right|left|down|up>"; speed 1 (slow) .. 10 (fast).
 * Breathing fades the region's color; Rainbow cycles all its keys through
 * the colors together; Wave runs a rainbow across the keyboard (LED n sits
 * at row n / 20, column n % 20 of the controller's matrix, Esc = 0). */
typedef enum { KBD_FX_STATIC = 0, KBD_FX_BREATHING, KBD_FX_RAINBOW, KBD_FX_WAVE, KBD_FX_COUNT } KbdFxType;
typedef enum { KBD_FX_DIR_RIGHT = 0, KBD_FX_DIR_LEFT, KBD_FX_DIR_DOWN, KBD_FX_DIR_UP, KBD_FX_DIR_COUNT } KbdFxDir;
typedef struct {
    uint8_t type;       /* KbdFxType */
    uint8_t speed;      /* 1..10 (0 = default) */
    uint8_t dir;        /* KbdFxDir, Wave only */
} KbdEffect;
#define KBD_FX_SPEED_DEFAULT 5
#define KBD_FX_SPEED_MAX 10
#define KBD_MATRIX_COLUMNS 20
#define KBD_MATRIX_ROWS 6
#define KBD_FX_FRAME_SEC 0.05   /* aim for 20 frames per second */

/* --- LED device structure --- */
typedef struct {
    char name[KBD_LED_NAME_MAX];
    char path[KBD_LED_PATH_MAX];
    uint8_t r, g, b;          /* Current color */
    int max_brightness;       /* Maximum brightness value */
} KbdLedDevice;

/* --- Global LED state --- */
typedef struct {
    KbdLedDevice leds[KBD_MAX_LEDS];
    int num_leds;
    uint8_t global_r, global_g, global_b;  /* Current global color */
    KbdLedMode mode;                    /* Current lighting mode */
    int is_on;                          /* Overall power state */
    uint8_t brightness;                 /* Global brightness level (0-255) */
} KbdLedState;

/* --- Zone structure --- */
typedef struct {
    char name[KBD_ZONE_NAME_MAX];
    uint8_t r, g, b;                    /* Zone color */
    int key_indices[KBD_MAX_KEYS_PER_ZONE]; /* LED indices in this zone */
    int num_keys;
    int is_preset;                      /* 1 if preset zone (don't save), 0 if user-created */
    KbdEffect fx;                       /* animated effect (1.35.0), static = 0 */
} KbdLedZone;

/* --- Profile structure --- */
typedef struct {
    char name[KBD_PROFILE_NAME_MAX];
    KbdLedZone zones[KBD_MAX_ZONES_PER_PROFILE];
    int num_zones;
    int has_base;                       /* base color for keys in no zone (1.24.0) */
    uint8_t base_r, base_g, base_b;
    KbdEffect base_fx;                  /* effect of the base color (1.35.0) */
} KbdLedProfile;

/* --- Function prototypes for LED access --- */
int kbd_led_init(void);                           /* Initialize LED subsystem */
void kbd_led_cleanup(void);                       /* Cleanup LED subsystem */
int set_led_color(const char *led_path, uint8_t r, uint8_t g, uint8_t b, int max_brightness);
int set_all_leds_color(uint8_t r, uint8_t g, uint8_t b);
int set_all_leds_with_brightness(uint8_t r, uint8_t g, uint8_t b);
int set_all_zones_color(uint8_t r, uint8_t g, uint8_t b);
int turn_off_all_leds(void);
int get_led_count(void);
const KbdLedState* kbd_led_get_state(void);
void kbd_led_set_mode(KbdLedMode mode);
void kbd_led_set_power(int is_on);              /* also stores the off state on disk */
int kbd_led_off_flag_set(void);                 /* 1 if the backlight was switched off */
/* Keyboard brightness = the LEDs' "brightness" attribute (0..max_brightness;
 * the ite_829x driver maps it to the keyboard's hardware brightness 0-10).
 * Desktop keyboard-brightness sliders (UPower) change the same value. */
int kbd_hw_brightness_max(void);
int kbd_hw_brightness_get(void);            /* -1 if unreadable */
int kbd_hw_brightness_set(int level);       /* also remembered for the next start */
int kbd_hw_brightness_poll(void);           /* 1 if changed by someone else (remembered) */
int kbd_hw_brightness_cached(void);         /* last known level (-1 unknown), no USB read */
void kbd_hw_brightness_restore(void);       /* at start: last level, or the maximum */

/* --- Utility functions --- */
int parse_rgb(const char *str, uint8_t *r, uint8_t *g, uint8_t *b);
int clamp_u8(int v);

/* --- Zone management --- */
int zone_create(const char *name, int *key_indices, int num_keys, uint8_t r, uint8_t g, uint8_t b, int is_preset);
int zone_delete(const char *name);
int zone_rename(const char *old_name, const char *new_name);
int zone_set_color(const char *name, uint8_t r, uint8_t g, uint8_t b);
int zone_add_key(const char *name, int key_index);
int zone_remove_key(const char *name, int key_index);
const KbdLedZone* zone_get(const char *name);
int zone_list(char *buffer, size_t buf_size);
void zone_clear_all(void);
/* Per-key lighting: give the listed LEDs a color (num_leds < 0 = every LED,
 * which replaces all zones by one base zone). The LEDs leave their old zones,
 * join the zone of that color (created if needed), empty zones are removed. */
int zone_assign_leds(const int *leds, int num_leds, uint8_t r, uint8_t g, uint8_t b);

/* --- Profile management --- */
int profile_apply(const char *name);
int kbd_apply_active_profile(void);
/* LEDs are numbered by name since 1.18.6+1 (rgb:kbd_backlight = 0,
 * rgb:kbd_backlight_N = N) instead of the order /sys/class/leds lists them.
 * Converts keyboard profiles and the LED wizard mapping saved with the old
 * numbering, once. Marker: "led_numbering": "name" / CSV header. */
int kbd_led_migrate_numbering(void);
#define KBD_CSV_NUMBERING_MARK "numbering=name"   /* at daemon start: re-apply the active profile */
const KbdLedProfile* profile_get_current(void);
int apply_zones(void);
int apply_zones_with_brightness(void);

/* --- JSON Profile I/O (profiles.json integration) --- */
int kbd_profiles_load_json(void);      /* Load keyboard profiles from profiles.json */
int kbd_profiles_save_json(void);      /* Save keyboard profiles to profiles.json */
int kbd_profile_save_json(const char *name);  /* Save current profile to profiles.json */
int kbd_profile_load_json(const char *name);  /* Load a profile from profiles.json */
int kbd_profile_create_json(const char *name);  /* Create an empty profile in profiles.json */
int kbd_profile_delete_json(const char *name);  /* Delete a profile from profiles.json */
int kbd_profile_rename_json(const char *old_name, const char *new_name);  /* Rename a profile */
int kbd_profile_list_json(char *buffer, size_t buf_size); /* List profiles from profiles.json */
int kbd_get_active_profile_index(void); /* Get active keyboard profile index */
/* Base color + zones editing (1.24.0) */
void kbd_base_set(int has_base, uint8_t r, uint8_t g, uint8_t b);
int kbd_has_colors(void);                 /* live state has a base color or zones */
int kbd_active_profile_valid(void);       /* there is an active profile */
int kbd_show_colors(void);                /* write the live colors (all off if none) */
int kbd_preview_set(const char *base, char *zones);   /* replace the live colors */
int kbd_profile_describe(const char *name, char *buffer, size_t buf_size);
int kbd_restore_active(int light_on);     /* live colors = active profile */
void kbd_set_active_profile_index(int idx); /* Set active keyboard profile index */

/* --- Effects (1.35.0) --- */
const char *kbd_fx_type_name(int type);
const char *kbd_fx_dir_name(int dir);
int kbd_fx_parse(const char *text, KbdEffect *fx);           /* 0 or -1 */
void kbd_fx_format(const KbdEffect *fx, char *out, size_t size);
void kbd_base_fx_set(const KbdEffect *fx);                    /* NULL = static */
int kbd_fx_set_all(const KbdEffect *fx);   /* live: every zone and the base color */
int kbd_fx_running(void);                  /* an effect is shown right now */
void kbd_fx_frame(void);                   /* draw one frame (changed keys only) */

/* Exposed global counters for logging */
extern int g_num_zones;
extern int g_num_profiles;

#endif /* DUDES_CONTROL_CENTER_KBD_COMMON_H */
