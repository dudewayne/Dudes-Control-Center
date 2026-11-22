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
} KbdLedZone;

/* --- Profile structure --- */
typedef struct {
    char name[KBD_PROFILE_NAME_MAX];
    KbdLedZone zones[KBD_MAX_ZONES_PER_PROFILE];
    int num_zones;
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
int kbd_led_set_brightness(uint8_t brightness);
uint8_t kbd_led_get_brightness(void);

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
void kbd_set_active_profile_index(int idx); /* Set active keyboard profile index */

/* Exposed global counters for logging */
extern int g_num_zones;
extern int g_num_profiles;

#endif /* DUDES_CONTROL_CENTER_KBD_COMMON_H */
