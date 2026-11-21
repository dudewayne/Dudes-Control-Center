/*
 * dudes-control-center-kbd-common.c
 * Common implementation for Keyboard Backlight Control in Dudes Control Center
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include "dudes-control-center-kbd-common.h"
#include "dudes-control-center-log.h"
#include "dudes-control-center-common.h"

/* ============================================================================
 * GLOBAL STATE
 * ============================================================================ */

static KbdLedState g_led_state;
static KbdLedZone g_zones[KBD_MAX_ZONES_PER_PROFILE];
static KbdLedProfile g_profiles[KBD_MAX_PROFILES];

int g_num_zones = 0;
int g_num_profiles = 0;
static int g_active_profile_index = 0;

/* ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================ */

int clamp_u8(int v) {
    return (v < 0) ? 0 : ((v > 255) ? 255 : v);
}

int parse_rgb(const char *str, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (!str || !r || !g || !b) return -1;

    /* Try R,G,B format */
    if (strchr(str, ',')) {
        int rr, gg, bb;
        if (sscanf(str, "%d,%d,%d", &rr, &gg, &bb) == 3) {
            *r = (uint8_t)clamp_u8(rr);
            *g = (uint8_t)clamp_u8(gg);
            *b = (uint8_t)clamp_u8(bb);
            return 0;
        }
    }

    /* Try #RRGGBB format */
    if (str[0] == '#') {
        unsigned int rr, gg, bb;
        if (sscanf(str + 1, "%2x%2x%2x", &rr, &gg, &bb) == 3) {
            *r = (uint8_t)rr;
            *g = (uint8_t)gg;
            *b = (uint8_t)bb;
            return 0;
        }
    }

    return -1;
}

/* ============================================================================
 * LED SUBSYSTEM
 * ============================================================================ */

static int scan_leds(KbdLedDevice *devices, int max_devices) {
    int count = 0;
    char led_path[256];

    DIR *dir = opendir("/sys/class/leds");
    if (!dir) {
        LOG_ERROR("[KBD] Cannot open /sys/class/leds: %s", strerror(errno));
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_devices) {
        if (strstr(entry->d_name, "kbd") ||
            strstr(entry->d_name, "keyboard") ||
            strstr(entry->d_name, "backlight")) {

            snprintf(led_path, sizeof(led_path), "/sys/class/leds/%s", entry->d_name);

            char multi_intensity_path[512];
            snprintf(multi_intensity_path, sizeof(multi_intensity_path), "%s/multi_intensity", led_path);

            struct stat st;
            if (stat(multi_intensity_path, &st) == 0 && S_ISREG(st.st_mode)) {
                strncpy(devices[count].name, entry->d_name, KBD_LED_NAME_MAX - 1);
                devices[count].name[KBD_LED_NAME_MAX - 1] = '\0';
                snprintf(devices[count].path, sizeof(devices[count].path), "%s/multi_intensity", led_path);
                devices[count].max_brightness = 255;

                char max_bright_path[512];
                snprintf(max_bright_path, sizeof(max_bright_path), "%s/max_brightness", led_path);
                FILE *f = fopen(max_bright_path, "r");
                if (f) {
                    fscanf(f, "%d", &devices[count].max_brightness);
                    fclose(f);
                }

                devices[count].r = 0;
                devices[count].g = 0;
                devices[count].b = 0;

                LOG_INFO("[KBD] Found LED: %s at %s (max_brightness=%d)",
                         devices[count].name, devices[count].path, devices[count].max_brightness);

                count++;
            }
        }
    }

    closedir(dir);
    return count;
}

int set_led_color(const char *led_path, uint8_t r, uint8_t g, uint8_t b, int max_brightness) {
    FILE *f = fopen(led_path, "w");
    if (!f) {
        LOG_ERROR("[KBD] Cannot open LED %s for writing: %s", led_path, strerror(errno));
        return -1;
    }

    int written = fprintf(f, "%d %d %d\n", r, g, b);
    fclose(f);

    if (written > 0) {
        LOG_DEBUG("[KBD] Set LED %s to RGB(%d, %d, %d)", led_path, r, g, b);
    }

    return (written > 0) ? 0 : -1;
}

int set_all_leds_color(uint8_t r, uint8_t g, uint8_t b) {
    g_led_state.global_r = r;
    g_led_state.global_g = g;
    g_led_state.global_b = b;

    int success_count = 0;
    for (int i = 0; i < g_led_state.num_leds; i++) {
        if (set_led_color(g_led_state.leds[i].path, r, g, b, g_led_state.leds[i].max_brightness) == 0) {
            g_led_state.leds[i].r = r;
            g_led_state.leds[i].g = g;
            g_led_state.leds[i].b = b;
            success_count++;
        }
    }

    return (success_count > 0) ? 0 : -1;
}

int set_all_leds_with_brightness(uint8_t r, uint8_t g, uint8_t b) {
    g_led_state.global_r = r;
    g_led_state.global_g = g;
    g_led_state.global_b = b;

    LOG_INFO("[KBD] Set all %d LEDs to RGB(%u, %u, %u) with brightness %u",
             g_led_state.num_leds, r, g, b, g_led_state.brightness);

    uint8_t scaled_r = (r * g_led_state.brightness) / 255;
    uint8_t scaled_g = (g * g_led_state.brightness) / 255;
    uint8_t scaled_b = (b * g_led_state.brightness) / 255;

    int success_count = 0;
    for (int i = 0; i < g_led_state.num_leds; i++) {
        if (set_led_color(g_led_state.leds[i].path, scaled_r, scaled_g, scaled_b, g_led_state.leds[i].max_brightness) == 0) {
            g_led_state.leds[i].r = scaled_r;
            g_led_state.leds[i].g = scaled_g;
            g_led_state.leds[i].b = scaled_b;
            success_count++;
        }
    }

    return (success_count > 0) ? 0 : -1;
}

int set_all_zones_color(uint8_t r, uint8_t g, uint8_t b) {
    g_led_state.global_r = r;
    g_led_state.global_g = g;
    g_led_state.global_b = b;

    for (int z = 0; z < g_num_zones; z++) {
        g_zones[z].r = r;
        g_zones[z].g = g;
        g_zones[z].b = b;
    }

    LOG_INFO("[KBD] Set all %d zones to RGB(%u, %u, %u)", g_num_zones, r, g, b);
    return apply_zones_with_brightness();
}

int turn_off_all_leds(void) {
    return set_all_leds_color(0, 0, 0);
}

int get_led_count(void) {
    return g_led_state.num_leds;
}

const KbdLedState* kbd_led_get_state(void) {
    return &g_led_state;
}

void kbd_led_set_mode(KbdLedMode mode) {
    g_led_state.mode = mode;
}

void kbd_led_set_power(int is_on) {
    g_led_state.is_on = is_on;
}

int kbd_led_set_brightness(uint8_t brightness) {
    if (brightness > 255) {
        brightness = 255;
    }

    g_led_state.brightness = brightness;
    LOG_INFO("[KBD] Brightness set to %d/255 (%.0f%%)", brightness, (brightness * 100.0) / 255.0);
    apply_zones_with_brightness();
    return 0;
}

uint8_t kbd_led_get_brightness(void) {
    return g_led_state.brightness;
}

int kbd_led_init(void) {
    LOG_INFO("[KBD] Initializing LED subsystem...");

    memset(&g_led_state, 0, sizeof(g_led_state));
    g_led_state.num_leds = scan_leds(g_led_state.leds, KBD_MAX_LEDS);

    if (g_led_state.num_leds == 0) {
        LOG_WARN("[KBD] No LEDs found! Driver may not be loaded.");
        LOG_WARN("[KBD] Try loading the driver with: sudo modprobe ite_829x");
    } else {
        LOG_INFO("[KBD] Found %d LED(s)", g_led_state.num_leds);
    }

    g_led_state.mode = KBD_LED_MODE_OFF;
    g_led_state.is_on = 0;
    g_led_state.brightness = 255;

    return (g_led_state.num_leds > 0) ? 0 : -1;
}

void kbd_led_cleanup(void) {
    LOG_INFO("[KBD] Cleaning up LED subsystem...");
    turn_off_all_leds();
}

/* ============================================================================
 * ZONE MANAGEMENT
 * ============================================================================ */

int zone_create(const char *name, int *key_indices, int num_keys, uint8_t r, uint8_t g, uint8_t b, int is_preset) {
    for (int i = 0; i < g_num_zones; i++) {
        if (strcmp(g_zones[i].name, name) == 0) {
            LOG_INFO("[KBD] Zone '%s' already exists, updating instead of creating duplicate", name);
            g_zones[i].r = r;
            g_zones[i].g = g;
            g_zones[i].b = b;
            g_zones[i].num_keys = num_keys;
            g_zones[i].is_preset = is_preset;
            for (int j = 0; j < num_keys && j < KBD_MAX_KEYS_PER_ZONE; j++) {
                g_zones[i].key_indices[j] = key_indices[j];
            }
            for (int j = num_keys; j < KBD_MAX_KEYS_PER_ZONE; j++) {
                g_zones[i].key_indices[j] = -1;
            }
            LOG_INFO("[KBD] Updated zone '%s' with %d keys", name, num_keys);
            return 0;
        }
    }

    if (g_num_zones >= KBD_MAX_ZONES_PER_PROFILE) {
        LOG_ERROR("[KBD] Too many zones (max %d)", KBD_MAX_ZONES_PER_PROFILE);
        return -1;
    }

    KbdLedZone *zone = &g_zones[g_num_zones++];
    strncpy(zone->name, name, KBD_ZONE_NAME_MAX - 1);
    zone->name[KBD_ZONE_NAME_MAX - 1] = '\0';
    zone->r = r;
    zone->g = g;
    zone->b = b;
    zone->num_keys = num_keys;
    zone->is_preset = is_preset;

    for (int i = 0; i < num_keys && i < KBD_MAX_KEYS_PER_ZONE; i++) {
        zone->key_indices[i] = key_indices[i];
    }

    LOG_INFO("[KBD] Created zone '%s' with %d keys (is_preset=%d)", name, num_keys, is_preset);
    return 0;
}

int zone_delete(const char *name) {
    for (int i = 0; i < g_num_zones; i++) {
        if (strcmp(g_zones[i].name, name) == 0) {
            memmove(&g_zones[i], &g_zones[i + 1],
                    (g_num_zones - i - 1) * sizeof(KbdLedZone));
            g_num_zones--;
            LOG_INFO("[KBD] Deleted zone '%s'", name);
            return 0;
        }
    }
    return -1;
}

int zone_rename(const char *old_name, const char *new_name) {
    for (int i = 0; i < g_num_zones; i++) {
        if (strcmp(g_zones[i].name, old_name) == 0) {
            for (int j = 0; j < g_num_zones; j++) {
                if (i != j && strcmp(g_zones[j].name, new_name) == 0) {
                    LOG_ERROR("[KBD] Cannot rename: zone '%s' already exists", new_name);
                    return -1;
                }
            }
            strncpy(g_zones[i].name, new_name, KBD_ZONE_NAME_MAX - 1);
            g_zones[i].name[KBD_ZONE_NAME_MAX - 1] = '\0';
            LOG_INFO("[KBD] Renamed zone '%s' to '%s'", old_name, new_name);
            return 0;
        }
    }
    LOG_ERROR("[KBD] Zone not found: %s", old_name);
    return -1;
}

int zone_set_color(const char *name, uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < g_num_zones; i++) {
        if (strcmp(g_zones[i].name, name) == 0) {
            g_zones[i].r = r;
            g_zones[i].g = g;
            g_zones[i].b = b;
            LOG_INFO("[KBD] Set zone '%s' color to RGB(%d,%d,%d)", name, r, g, b);
            return 0;
        }
    }
    return -1;
}

int zone_add_key(const char *name, int key_index) {
    for (int i = 0; i < g_num_zones; i++) {
        if (strcmp(g_zones[i].name, name) == 0) {
            if (g_zones[i].num_keys < KBD_MAX_KEYS_PER_ZONE) {
                g_zones[i].key_indices[g_zones[i].num_keys++] = key_index;
                return 0;
            }
        }
    }
    return -1;
}

int zone_remove_key(const char *name, int key_index) {
    for (int i = 0; i < g_num_zones; i++) {
        if (strcmp(g_zones[i].name, name) == 0) {
            for (int j = 0; j < g_zones[i].num_keys; j++) {
                if (g_zones[i].key_indices[j] == key_index) {
                    memmove(&g_zones[i].key_indices[j], &g_zones[i].key_indices[j + 1],
                            (g_zones[i].num_keys - j - 1) * sizeof(int));
                    g_zones[i].num_keys--;
                    return 0;
                }
            }
        }
    }
    return -1;
}

const KbdLedZone* zone_get(const char *name) {
    for (int i = 0; i < g_num_zones; i++) {
        if (strcmp(g_zones[i].name, name) == 0) {
            return &g_zones[i];
        }
    }
    return NULL;
}

int zone_list(char *buffer, size_t buf_size) {
    int offset = 0;
    offset += snprintf(buffer + offset, buf_size - offset, "ZONES_START COUNT=%d\n", g_num_zones);

    for (int i = 0; i < g_num_zones && offset < buf_size - 256; i++) {
        offset += snprintf(buffer + offset, buf_size - offset,
                "ZONE %d name=%s color=%u,%u,%u keys=%d",
                i, g_zones[i].name,
                g_zones[i].r, g_zones[i].g, g_zones[i].b,
                g_zones[i].num_keys);

        for (int k = 0; k < g_zones[i].num_keys && offset < buf_size - 16; k++) {
            offset += snprintf(buffer + offset, buf_size - offset, " %d", g_zones[i].key_indices[k]);
        }
        offset += snprintf(buffer + offset, buf_size - offset, "\n");
    }

    offset += snprintf(buffer + offset, buf_size - offset, "ZONES_END\n");
    return 0;
}

void zone_clear_all(void) {
    g_num_zones = 0;
    memset(g_zones, 0, sizeof(g_zones));
    LOG_INFO("[KBD] Cleared all zones - ready for new profile");
}

/* ============================================================================
 * PROFILE MANAGEMENT
 * ============================================================================ */

int profile_apply(const char *name) {
    LOG_INFO("[KBD] Applying profile '%s'...", name);

    if (kbd_profile_load_json(name) != 0) {
        LOG_ERROR("[KBD] Failed to load profile '%s'", name);
        return -1;
    }

    return apply_zones();
}

const KbdLedProfile* profile_get_current(void) {
    if (g_num_profiles > 0) {
        return &g_profiles[g_num_profiles - 1];
    }
    return NULL;
}

static void apply_brightness_scale(uint8_t *r, uint8_t *g, uint8_t *b, uint8_t brightness) {
    if (brightness == 0) {
        *r = *g = *b = 0;
    } else if (brightness < 255) {
        *r = (*r * brightness) / 255;
        *g = (*g * brightness) / 255;
        *b = (*b * brightness) / 255;
    }
}

int apply_zones_with_brightness(void) {
    if (g_num_zones == 0) {
        LOG_ERROR("[KBD] No zones to apply");
        return -1;
    }

    uint8_t led_colors[KBD_MAX_LEDS][3] = {{0}};

    for (int z = 0; z < g_num_zones; z++) {
        KbdLedZone *zone = &g_zones[z];

        for (int k = 0; k < zone->num_keys; k++) {
            int led_idx = zone->key_indices[k];
            if (led_idx >= 0 && led_idx < g_led_state.num_leds) {
                led_colors[led_idx][0] = zone->r;
                led_colors[led_idx][1] = zone->g;
                led_colors[led_idx][2] = zone->b;
            }
        }
    }

    int success_count = 0;
    for (int i = 0; i < g_led_state.num_leds; i++) {
        uint8_t r = led_colors[i][0];
        uint8_t g = led_colors[i][1];
        uint8_t b = led_colors[i][2];

        apply_brightness_scale(&r, &g, &b, g_led_state.brightness);

        if (set_led_color(g_led_state.leds[i].path, r, g, b, g_led_state.leds[i].max_brightness) == 0) {
            success_count++;
        }
    }

    LOG_INFO("[KBD] Applied %d zones to %d LEDs (brightness: %d/255)", g_num_zones, success_count, g_led_state.brightness);
    return (success_count > 0) ? 0 : -1;
}

int apply_zones(void) {
    return apply_zones_with_brightness();
}

/* ============================================================================
 * JSON Profile I/O (profiles.json integration)
 * ============================================================================ */

#include <cjson/cJSON.h>

/* Helper: build a cJSON object for a single keyboard profile */
static cJSON* kbd_profile_to_json(const KbdLedProfile *p) {
    cJSON *profile_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(profile_obj, "name", p->name);

    /* Zones array */
    cJSON *zones_array = cJSON_CreateArray();
    for (int i = 0; i < p->num_zones; i++) {
        cJSON *zone_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(zone_obj, "name", p->zones[i].name);
        cJSON_AddNumberToObject(zone_obj, "r", p->zones[i].r);
        cJSON_AddNumberToObject(zone_obj, "g", p->zones[i].g);
        cJSON_AddNumberToObject(zone_obj, "b", p->zones[i].b);

        cJSON *keys_array = cJSON_CreateArray();
        for (int k = 0; k < p->zones[i].num_keys; k++) {
            cJSON_AddItemToArray(keys_array, cJSON_CreateNumber(p->zones[i].key_indices[k]));
        }
        cJSON_AddItemToObject(zone_obj, "keys", keys_array);
        cJSON_AddItemToArray(zones_array, zone_obj);
    }
    cJSON_AddItemToObject(profile_obj, "zones", zones_array);

    return profile_obj;
}

/* Get active keyboard profile index */
int kbd_get_active_profile_index(void) {
    return g_active_profile_index;
}

/* Set active keyboard profile index */
void kbd_set_active_profile_index(int idx) {
    g_active_profile_index = idx;
}

/* Save all keyboard profiles to profiles.json (keyboard_profiles section) */
int kbd_profiles_save_json(void) {
    const char *path = MAIN_PROFILES_PATH;

    cJSON *root = NULL;
    FILE *f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (file_size > 0 && file_size <= 1024 * 1024) {
            char *file_content = (char *)malloc(file_size + 1);
            if (file_content) {
                size_t bytes_read = fread(file_content, 1, file_size, f);
                file_content[bytes_read] = '\0';
                root = cJSON_Parse(file_content);
                free(file_content);
            }
        }
        fclose(f);
    }

    if (!root) {
        root = cJSON_CreateObject();
        if (!root) return -1;
        cJSON_AddStringToObject(root, "version", "1.0");
    }

    /* Remove existing keyboard_profiles section if present */
    cJSON_DetachItemFromObject(root, "keyboard_profiles");

    /* Build keyboard_profiles section */
    cJSON *kbd_section = cJSON_CreateObject();
    cJSON_AddNumberToObject(kbd_section, "active_profile_index", g_active_profile_index);

    cJSON *profiles_array = cJSON_CreateArray();
    for (int i = 0; i < g_num_profiles; i++) {
        cJSON *profile_obj = kbd_profile_to_json(&g_profiles[i]);
        cJSON_AddItemToArray(profiles_array, profile_obj);
    }
    cJSON_AddItemToObject(kbd_section, "profiles", profiles_array);
    cJSON_AddItemToObject(root, "keyboard_profiles", kbd_section);

    /* Write to file */
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        return -1;
    }

    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", json_str);
        fclose(f);
        LOG_INFO("[KBD] Saved %d keyboard profiles to %s (keyboard_profiles section)",
                 g_num_profiles, path);
    } else {
        LOG_ERROR("[KBD] Failed to write keyboard profiles to %s: %s", path, strerror(errno));
        cJSON_free(json_str);
        cJSON_Delete(root);
        return -1;
    }

    cJSON_free(json_str);
    cJSON_Delete(root);
    return 0;
}

/* Load all keyboard profiles from profiles.json */
int kbd_profiles_load_json(void) {
    g_num_profiles = 0;

    const char *path = MAIN_PROFILES_PATH;

    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_INFO("[KBD] No saved profiles file found at %s", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 1024 * 1024) {
        fclose(f);
        return -1;
    }

    char *file_content = (char *)malloc(file_size + 1);
    if (!file_content) {
        fclose(f);
        return -1;
    }

    size_t bytes_read = fread(file_content, 1, file_size, f);
    file_content[bytes_read] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(file_content);
    free(file_content);

    if (!root) {
        LOG_ERROR("[KBD] Failed to parse profiles JSON from %s", path);
        return -1;
    }

    cJSON *kbd_section = cJSON_GetObjectItem(root, "keyboard_profiles");
    if (!kbd_section) {
        LOG_INFO("[KBD] No keyboard_profiles section in %s", path);
        cJSON_Delete(root);
        return -1;
    }

    /* Read active index */
    if (cJSON_IsObject(kbd_section)) {
        cJSON *active_item = cJSON_GetObjectItem(kbd_section, "active_profile_index");
        if (active_item) {
            g_active_profile_index = active_item->valueint;
        }
    }

    /* Load profiles */
    cJSON *profiles_array = cJSON_GetObjectItem(kbd_section, "profiles");
    if (profiles_array && cJSON_IsArray(profiles_array)) {
        int count = cJSON_GetArraySize(profiles_array);
        for (int i = 0; i < count && i < KBD_MAX_PROFILES; i++) {
            cJSON *profile_obj = cJSON_GetArrayItem(profiles_array, i);
            if (!profile_obj) continue;

            KbdLedProfile p = {0};

            cJSON *name_item = cJSON_GetObjectItem(profile_obj, "name");
            if (name_item && name_item->valuestring) {
                strncpy(p.name, name_item->valuestring, KBD_PROFILE_NAME_MAX - 1);
            } else {
                continue;
            }

            /* Load zones */
            cJSON *zones_array = cJSON_GetObjectItem(profile_obj, "zones");
            if (zones_array && cJSON_IsArray(zones_array)) {
                int zone_count = cJSON_GetArraySize(zones_array);
                for (int z = 0; z < zone_count && z < KBD_MAX_ZONES_PER_PROFILE; z++) {
                    cJSON *zone_obj = cJSON_GetArrayItem(zones_array, z);
                    if (!zone_obj) continue;

                    cJSON *zname = cJSON_GetObjectItem(zone_obj, "name");
                    if (zname && zname->valuestring) {
                        strncpy(p.zones[z].name, zname->valuestring, KBD_ZONE_NAME_MAX - 1);
                    }

                    cJSON *r_item = cJSON_GetObjectItem(zone_obj, "r");
                    if (r_item) p.zones[z].r = (uint8_t)r_item->valueint;

                    cJSON *g_item = cJSON_GetObjectItem(zone_obj, "g");
                    if (g_item) p.zones[z].g = (uint8_t)g_item->valueint;

                    cJSON *b_item = cJSON_GetObjectItem(zone_obj, "b");
                    if (b_item) p.zones[z].b = (uint8_t)b_item->valueint;

                    cJSON *keys_array = cJSON_GetObjectItem(zone_obj, "keys");
                    if (keys_array && cJSON_IsArray(keys_array)) {
                        int keys_count = cJSON_GetArraySize(keys_array);
                        for (int k = 0; k < keys_count && k < KBD_MAX_KEYS_PER_ZONE; k++) {
                            cJSON *key_item = cJSON_GetArrayItem(keys_array, k);
                            if (key_item) {
                                p.zones[z].key_indices[k] = key_item->valueint;
                            }
                        }
                        p.zones[z].num_keys = keys_count;
                    }
                }
                p.num_zones = zone_count;
            }

            g_profiles[g_num_profiles] = p;
            g_num_profiles++;
        }
    }

    cJSON_Delete(root);
    LOG_INFO("[KBD] Loaded %d keyboard profiles from %s (keyboard_profiles section)",
             g_num_profiles, path);
    return 0;
}

/* Save current zones as a profile to profiles.json */
int kbd_profile_save_json(const char *name) {
    if (g_num_zones == 0) {
        LOG_ERROR("[KBD] No zones to save");
        return -1;
    }

    /* Check if profile with this name already exists */
    for (int i = 0; i < g_num_profiles; i++) {
        if (strcmp(g_profiles[i].name, name) == 0) {
            /* Update existing profile */
            memset(&g_profiles[i], 0, sizeof(KbdLedProfile));
            strncpy(g_profiles[i].name, name, KBD_PROFILE_NAME_MAX - 1);
            g_profiles[i].num_zones = g_num_zones;
            for (int z = 0; z < g_num_zones; z++) {
                memcpy(&g_profiles[i].zones[z], &g_zones[z], sizeof(KbdLedZone));
            }
            LOG_INFO("[KBD] Updated profile '%s' with %d zones", name, g_num_zones);
            return kbd_profiles_save_json();
        }
    }

    /* Add new profile */
    if (g_num_profiles >= KBD_MAX_PROFILES) {
        LOG_ERROR("[KBD] Too many profiles (max %d)", KBD_MAX_PROFILES);
        return -1;
    }

    KbdLedProfile *p = &g_profiles[g_num_profiles];
    strncpy(p->name, name, KBD_PROFILE_NAME_MAX - 1);
    p->num_zones = g_num_zones;
    for (int z = 0; z < g_num_zones; z++) {
        memcpy(&p->zones[z], &g_zones[z], sizeof(KbdLedZone));
    }
    g_num_profiles++;

    LOG_INFO("[KBD] Added profile '%s' with %d zones", name, g_num_zones);
    return kbd_profiles_save_json();
}

/* Load a profile by name from profiles.json into active zones */
int kbd_profile_load_json(const char *name) {
    LOG_INFO("[KBD] Loading profile '%s' - clearing existing %d zones first", name, g_num_zones);

    /* Clear all existing zones */
    zone_clear_all();

    /* Find the profile */
    for (int i = 0; i < g_num_profiles; i++) {
        if (strcmp(g_profiles[i].name, name) == 0) {
            /* Copy zones from profile */
            for (int z = 0; z < g_profiles[i].num_zones; z++) {
                memcpy(&g_zones[g_num_zones], &g_profiles[i].zones[z], sizeof(KbdLedZone));
                g_num_zones++;
            }
            g_active_profile_index = i;
            LOG_INFO("[KBD] Loaded profile '%s' with %d zones", name, g_profiles[i].num_zones);
            return 0;
        }
    }

    LOG_ERROR("[KBD] Profile '%s' not found", name);
    return -1;
}

/* Delete a profile by name from profiles.json */
int kbd_profile_delete_json(const char *name) {
    for (int i = 0; i < g_num_profiles; i++) {
        if (strcmp(g_profiles[i].name, name) == 0) {
            memmove(&g_profiles[i], &g_profiles[i + 1],
                    (g_num_profiles - i - 1) * sizeof(KbdLedProfile));
            g_num_profiles--;

            /* Adjust active index */
            if (g_active_profile_index >= g_num_profiles) {
                g_active_profile_index = g_num_profiles - 1;
            }

            LOG_INFO("[KBD] Deleted profile '%s'", name);
            return kbd_profiles_save_json();
        }
    }
    LOG_ERROR("[KBD] Profile '%s' not found for deletion", name);
    return -1;
}

/* Create an empty profile in profiles.json */
int kbd_profile_create_json(const char *name) {
    /* Check if profile already exists */
    for (int i = 0; i < g_num_profiles; i++) {
        if (strcmp(g_profiles[i].name, name) == 0) {
            LOG_ERROR("[KBD] Profile '%s' already exists", name);
            return -1;
        }
    }

    /* Check max profiles */
    if (g_num_profiles >= KBD_MAX_PROFILES) {
        LOG_ERROR("[KBD] Too many profiles (max %d)", KBD_MAX_PROFILES);
        return -1;
    }

    /* Create empty profile */
    KbdLedProfile *p = &g_profiles[g_num_profiles];
    strncpy(p->name, name, KBD_PROFILE_NAME_MAX - 1);
    p->name[KBD_PROFILE_NAME_MAX - 1] = '\0';
    p->num_zones = 0;
    g_num_profiles++;

    LOG_INFO("[KBD] Created empty profile '%s'", name);
    return kbd_profiles_save_json();
}

/* List profiles from profiles.json */
int kbd_profile_list_json(char *buffer, size_t buf_size) {
    int offset = 0;
    offset += snprintf(buffer + offset, buf_size - offset,
                       "PROFILES_START COUNT=%d\n", g_num_profiles);

    for (int i = 0; i < g_num_profiles && offset < buf_size - 64; i++) {
        const char *active_marker = (i == g_active_profile_index) ? "*" : " ";
        offset += snprintf(buffer + offset, buf_size - offset,
                "PROFILE %d [%s] %s\n",
                i, active_marker, g_profiles[i].name);
    }

    offset += snprintf(buffer + offset, buf_size - offset, "PROFILES_END\n");
    return 0;
}
