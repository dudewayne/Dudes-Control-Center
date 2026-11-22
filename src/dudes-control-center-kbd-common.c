/*
 * dudes-control-center-kbd-common.c
 * Common implementation for Keyboard Backlight Control in Dudes Control Center
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>

#include "dudes-control-center-kbd-common.h"
#include "dudes-control-center-log.h"
#include "dudes-control-center-secure.h"
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
/* old (directory order) LED index -> new (name order) index, from scan_leds() */
static int g_led_old_to_new[KBD_MAX_LEDS];
static int g_led_numbering_name = 0;   /* profiles.json says LEDs are numbered by name */
static int g_kbd_section_seen = 0;     /* profiles.json had a keyboard_profiles section */

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
    char led_path[KBD_LED_PATH_MAX - 32];   /* room for "/multi_intensity" in .path */

    DIR *dir = opendir(KBD_LEDS_DIR);
    if (!dir) {
        LOG_ERROR("[KBD] Cannot open %s: %s", KBD_LEDS_DIR, strerror(errno));
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_devices) {
        if (strstr(entry->d_name, "kbd") ||
            strstr(entry->d_name, "keyboard") ||
            strstr(entry->d_name, "backlight")) {

            snprintf(led_path, sizeof(led_path), "%s/%s", KBD_LEDS_DIR, entry->d_name);

            char multi_intensity_path[KBD_LED_PATH_MAX + 32];
            snprintf(multi_intensity_path, sizeof(multi_intensity_path), "%s/multi_intensity", led_path);

            struct stat st;
            if (stat(multi_intensity_path, &st) == 0 && S_ISREG(st.st_mode)) {
                strncpy(devices[count].name, entry->d_name, KBD_LED_NAME_MAX - 1);
                devices[count].name[KBD_LED_NAME_MAX - 1] = '\0';
                snprintf(devices[count].path, sizeof(devices[count].path), "%s/multi_intensity", led_path);
                devices[count].max_brightness = 255;

                char max_bright_path[KBD_LED_PATH_MAX + 32];
                snprintf(max_bright_path, sizeof(max_bright_path), "%s/max_brightness", led_path);
                FILE *f = fopen(max_bright_path, "r");
                if (f) {
                    if (fscanf(f, "%d", &devices[count].max_brightness) != 1)
                        devices[count].max_brightness = 255;
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

    /* Number the LEDs by name, not by the (hash) order of the directory:
     * rgb:kbd_backlight -> 0, rgb:kbd_backlight_N -> N, others after them */
    int order[KBD_MAX_LEDS];
    long key[KBD_MAX_LEDS];
    for (int i = 0; i < count; i++) {
        order[i] = i;
        key[i] = 1000000L + i;                       /* unknown names keep their order, last */
        const char *k = strstr(devices[i].name, "kbd_backlight");
        if (k) {
            k += strlen("kbd_backlight");
            if (*k == '\0') key[i] = 0;
            else if (*k == '_' && k[1] >= '0' && k[1] <= '9') key[i] = strtol(k + 1, NULL, 10);
        }
    }
    for (int i = 1; i < count; i++) {                /* insertion sort, count <= 256 */
        int o = order[i], j = i - 1;
        while (j >= 0 && key[order[j]] > key[o]) { order[j + 1] = order[j]; j--; }
        order[j + 1] = o;
    }
    static KbdLedDevice sorted[KBD_MAX_LEDS];
    for (int n = 0; n < count; n++) {
        sorted[n] = devices[order[n]];
        g_led_old_to_new[order[n]] = n;
    }
    memcpy(devices, sorted, (size_t)count * sizeof(KbdLedDevice));
    return count;
}

int set_led_color(const char *led_path, uint8_t r, uint8_t g, uint8_t b, int max_brightness) {
    (void)max_brightness;   /* multi_intensity takes the raw 0-255 channel values */
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
    if (is_on) {
        if (unlink(KBD_OFF_FLAG_FILE) != 0 && errno != ENOENT)
            LOG_WARN("[KBD] Cannot remove %s: %s", KBD_OFF_FLAG_FILE, strerror(errno));
    } else {
        mkdir(KBD_STATE_DIR, 0755);
        int fd = open(KBD_OFF_FLAG_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0) close(fd);
        else LOG_WARN("[KBD] Cannot write %s: %s", KBD_OFF_FLAG_FILE, strerror(errno));
    }
}

int kbd_led_off_flag_set(void) {
    struct stat st;
    return lstat(KBD_OFF_FLAG_FILE, &st) == 0 && S_ISREG(st.st_mode);
}

int kbd_led_set_brightness(uint8_t brightness) {

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

/* Append to buffer; offset never passes buf_size - 1 (snprintf may report more) */
static void zbuf_add(char *buffer, size_t buf_size, size_t *offset, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void zbuf_add(char *buffer, size_t buf_size, size_t *offset, const char *fmt, ...) {
    if (*offset >= buf_size - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buffer + *offset, buf_size - *offset, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    *offset += (size_t)n;
    if (*offset > buf_size - 1) *offset = buf_size - 1;
}

int zone_list(char *buffer, size_t buf_size) {
    if (buf_size < 64) return -1;
    size_t offset = 0;
    zbuf_add(buffer, buf_size, &offset, "ZONES_START COUNT=%d\n", g_num_zones);

    for (int i = 0; i < g_num_zones && offset + 256 < buf_size; i++) {
        zbuf_add(buffer, buf_size, &offset, "ZONE %d name=%s color=%u,%u,%u keys=%d",
                 i, g_zones[i].name, g_zones[i].r, g_zones[i].g, g_zones[i].b, g_zones[i].num_keys);
        for (int k = 0; k < g_zones[i].num_keys && offset + 16 < buf_size; k++)
            zbuf_add(buffer, buf_size, &offset, " %d", g_zones[i].key_indices[k]);
        zbuf_add(buffer, buf_size, &offset, "\n");
    }

    zbuf_add(buffer, buf_size, &offset, "ZONES_END\n");
    return 0;
}

void zone_clear_all(void) {
    g_num_zones = 0;
    memset(g_zones, 0, sizeof(g_zones));
    LOG_INFO("[KBD] Cleared all zones - ready for new profile");
}

static void zone_remove_led_everywhere(int led) {
    for (int z = 0; z < g_num_zones; z++) {
        KbdLedZone *zone = &g_zones[z];
        int w = 0;
        for (int k = 0; k < zone->num_keys; k++)
            if (zone->key_indices[k] != led) zone->key_indices[w++] = zone->key_indices[k];
        zone->num_keys = w;
    }
}

int zone_assign_leds(const int *leds, int num_leds, uint8_t r, uint8_t g, uint8_t b) {
    int all_leds[KBD_MAX_LEDS];
    if (num_leds < 0) {
        /* Whole keyboard: one base zone with every LED */
        zone_clear_all();
        num_leds = g_led_state.num_leds;
        for (int i = 0; i < num_leds; i++) all_leds[i] = i;
        leds = all_leds;
        if (zone_create("Base", all_leds, num_leds, r, g, b, 0) != 0) return -1;
        LOG_INFO("[KBD] All %d LEDs set to RGB(%u,%u,%u)", num_leds, r, g, b);
        return 0;
    }

    /* Zone named after its color, so keys of the same color share one zone */
    char name[KBD_ZONE_NAME_MAX];
    snprintf(name, sizeof name, "#%02X%02X%02X", r, g, b);

    int valid = 0;
    for (int i = 0; i < num_leds; i++) {
        if (leds[i] < 0 || leds[i] >= g_led_state.num_leds || leds[i] >= KBD_MAX_LEDS) continue;
        zone_remove_led_everywhere(leds[i]);
        valid++;
    }
    if (valid == 0) return -1;

    int zi = -1;
    for (int z = 0; z < g_num_zones; z++)
        if (strcmp(g_zones[z].name, name) == 0) { zi = z; break; }
    if (zi < 0) {
        /* drop zones that became empty first, to make room */
        int w = 0;
        for (int z = 0; z < g_num_zones; z++)
            if (g_zones[z].num_keys > 0) g_zones[w++] = g_zones[z];
        g_num_zones = w;
        if (g_num_zones >= KBD_MAX_ZONES_PER_PROFILE) {
            LOG_ERROR("[KBD] Too many colors (max %d zones)", KBD_MAX_ZONES_PER_PROFILE);
            return -1;
        }
        zi = g_num_zones++;
        memset(&g_zones[zi], 0, sizeof g_zones[zi]);
        snprintf(g_zones[zi].name, sizeof g_zones[zi].name, "%s", name);
        g_zones[zi].r = r;
        g_zones[zi].g = g;
        g_zones[zi].b = b;
    }
    for (int i = 0; i < num_leds; i++) {
        if (leds[i] < 0 || leds[i] >= g_led_state.num_leds || leds[i] >= KBD_MAX_LEDS) continue;
        int dup = 0;
        for (int k = 0; k < g_zones[zi].num_keys; k++)
            if (g_zones[zi].key_indices[k] == leds[i]) dup = 1;
        if (!dup && g_zones[zi].num_keys < KBD_MAX_KEYS_PER_ZONE)
            g_zones[zi].key_indices[g_zones[zi].num_keys++] = leds[i];
    }

    /* remove zones left without LEDs */
    int w = 0;
    for (int z = 0; z < g_num_zones; z++)
        if (g_zones[z].num_keys > 0) g_zones[w++] = g_zones[z];
    g_num_zones = w;

    LOG_INFO("[KBD] %d LED(s) set to RGB(%u,%u,%u) (%d zones)", valid, r, g, b, g_num_zones);
    return 0;
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

    /* Persist the active profile index to profiles.json */
    kbd_profiles_save_json();

    /* Apply zones - OK if 0 zones (empty profile) */
    int result = apply_zones();
    if (result != 0 && g_num_zones == 0) {
        LOG_INFO("[KBD] Profile '%s' has no zones to apply", name);
        return 0;
    }
    return result;
}

int kbd_apply_active_profile(void) {
    if (g_active_profile_index < 0 || g_active_profile_index >= g_num_profiles) return -1;
    const KbdLedProfile *p = &g_profiles[g_active_profile_index];
    if (p->num_zones == 0) {
        LOG_INFO("[KBD] Active profile '%s' has no colors yet", p->name);
        return -1;
    }
    if (kbd_profile_load_json(p->name) != 0) return -1;
    LOG_INFO("[KBD] Restoring active profile '%s' (%d zones)", p->name, g_num_zones);
    return apply_zones();
}

/* Convert the LED wizard mapping CSV next to profiles.json */
static void kbd_migrate_mapping_csv(const char *profiles_path) {
    char csv[1024];
    snprintf(csv, sizeof csv, "%s", profiles_path);
    char *slash = strrchr(csv, '/');
    if (slash) snprintf(slash + 1, sizeof csv - (size_t)(slash + 1 - csv), "keyboard_led_mapping.csv");
    else snprintf(csv, sizeof csv, "keyboard_led_mapping.csv");

    char *data = dcc_user_file_read(csv, 256 * 1024, NULL);
    if (!data) return;                                   /* no wizard mapping yet */
    if (strstr(data, KBD_CSV_NUMBERING_MARK)) { free(data); return; }   /* already new */

    size_t cap = strlen(data) + 256, len = 0;
    char *out = malloc(cap);
    if (!out) { free(data); return; }
    len += (size_t)snprintf(out, cap, "led_id;position;led_index;%s\n", KBD_CSV_NUMBERING_MARK);
    int converted = 0;
    char *save = NULL;
    int first = 1;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (first) { first = 0; continue; }              /* old header */
        char *sep = strchr(line, ';');
        int old = atoi(line);
        if (!sep || old < 0 || old >= g_led_state.num_leds) continue;
        len += (size_t)snprintf(out + len, cap - len, "%d%s\n", g_led_old_to_new[old], sep);
        converted++;
        if (len + 64 > cap) break;
    }
    if (dcc_user_file_write(csv, out, len) == 0)
        LOG_INFO("[KBD] LED wizard mapping converted to the new LED numbering (%d LEDs): %s", converted, csv);
    else
        LOG_ERROR("[KBD] Could not convert %s: %s", csv, strerror(errno));
    free(out);
    free(data);
}

int kbd_led_migrate_numbering(void) {
#ifdef MAIN_PROFILES_PATH
    const char *path = MAIN_PROFILES_PATH;
#else
    const char *path = dcc_profiles_path();
    if (!path) return -1;
#endif
    int identity = 1;
    for (int i = 0; i < g_led_state.num_leds; i++)
        if (g_led_old_to_new[i] != i) identity = 0;

    /* The wizard mapping has its own marker (it can exist without profiles) */
    if (!identity) kbd_migrate_mapping_csv(path);

    if (!g_kbd_section_seen || g_led_numbering_name) return 0;   /* nothing saved in the old numbering */

    int changed = 0;
    if (!identity) {
        for (int p = 0; p < g_num_profiles; p++)
            for (int z = 0; z < g_profiles[p].num_zones; z++) {
                KbdLedZone *zone = &g_profiles[p].zones[z];
                for (int k = 0; k < zone->num_keys; k++) {
                    int old = zone->key_indices[k];
                    if (old >= 0 && old < g_led_state.num_leds) {
                        zone->key_indices[k] = g_led_old_to_new[old];
                        changed++;
                    }
                }
            }
    }
    g_led_numbering_name = 1;
    LOG_INFO("[KBD] Keyboard profiles converted to the new LED numbering (%d key entries)", changed);
    return kbd_profiles_save_json();
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
#ifdef MAIN_PROFILES_PATH
    const char *path = MAIN_PROFILES_PATH;
#else
    const char *path = dcc_profiles_path();
    if (!path) return -1;
#endif

    /* Read as the file's owner, never through a symlink (see secure.h) */
    cJSON *root = NULL;
    char *file_content = dcc_user_file_read(path, 1024 * 1024, NULL);
    if (file_content) {
        root = cJSON_Parse(file_content);
        free(file_content);
        if (!root) {
            /* Never replace an unparsable file: that would wipe the other sections */
            LOG_ERROR("%s: existing %s is not valid JSON - not saving", __func__, path);
            return -1;
        }
    } else if (errno != ENOENT) {
        LOG_ERROR("%s: cannot read %s - not saving", __func__, path);
        return -1;
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
    cJSON_AddStringToObject(kbd_section, "led_numbering", "name");

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

    if (dcc_user_file_write(path, json_str, strlen(json_str)) == 0) {
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

#ifdef MAIN_PROFILES_PATH
    const char *path = MAIN_PROFILES_PATH;
#else
    const char *path = dcc_profiles_path();
    if (!path) return -1;
#endif

    /* Read as the file's owner, never through a symlink (see secure.h) */
    char *file_content = dcc_user_file_read(path, 1024 * 1024, NULL);
    if (!file_content) {
        LOG_INFO("[KBD] No readable profiles file at %s", path);
        return -1;
    }

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
        g_kbd_section_seen = 1;
        cJSON *numbering = cJSON_GetObjectItem(kbd_section, "led_numbering");
        g_led_numbering_name = cJSON_IsString(numbering) && strcmp(numbering->valuestring, "name") == 0;
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
                int nz = 0;
                for (int zi = 0; zi < zone_count && nz < KBD_MAX_ZONES_PER_PROFILE; zi++) {
                    cJSON *zone_obj = cJSON_GetArrayItem(zones_array, zi);
                    if (!zone_obj) continue;
                    int z = nz++;

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
                        if (keys_count > KBD_MAX_KEYS_PER_ZONE) keys_count = KBD_MAX_KEYS_PER_ZONE;
                        for (int k = 0; k < keys_count; k++) {
                            cJSON *key_item = cJSON_GetArrayItem(keys_array, k);
                            if (key_item) {
                                p.zones[z].key_indices[k] = key_item->valueint;
                            }
                        }
                        p.zones[z].num_keys = keys_count;
                    }
                }
                p.num_zones = nz;
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
        LOG_INFO("[KBD] Saving profile '%s' without colors", name);
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

/* Rename a profile in profiles.json */
int kbd_profile_rename_json(const char *old_name, const char *new_name) {
    /* Validate new name */
    if (!new_name || strlen(new_name) == 0) {
        LOG_ERROR("[KBD] Invalid new profile name (empty)");
        return -1;
    }
    if (strchr(new_name, '\n') != NULL) {
        LOG_ERROR("[KBD] Invalid new profile name (contains newline)");
        return -1;
    }

    /* Check for duplicate name */
    for (int i = 0; i < g_num_profiles; i++) {
        if (strcmp(g_profiles[i].name, new_name) == 0) {
            LOG_ERROR("[KBD] Profile '%s' already exists", new_name);
            return -1;
        }
    }

    /* Find and rename */
    for (int i = 0; i < g_num_profiles; i++) {
        if (strcmp(g_profiles[i].name, old_name) == 0) {
            strncpy(g_profiles[i].name, new_name, KBD_PROFILE_NAME_MAX - 1);
            g_profiles[i].name[KBD_PROFILE_NAME_MAX - 1] = '\0';
            LOG_INFO("[KBD] Renamed profile '%s' to '%s'", old_name, new_name);
            return kbd_profiles_save_json();
        }
    }
    LOG_ERROR("[KBD] Profile '%s' not found for rename", old_name);
    return -1;
}

/* Create an empty profile in profiles.json */
int kbd_profile_create_json(const char *name) {
    /* Validate profile name */
    if (!name || strlen(name) == 0) {
        LOG_ERROR("[KBD] Invalid profile name (empty)");
        return -1;
    }
    if (strchr(name, '\n') != NULL) {
        LOG_ERROR("[KBD] Invalid profile name (contains newline)");
        return -1;
    }

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

    /* Margin must exceed the longest line ("PROFILE nn [*] " + 63-char name),
     * otherwise a truncated snprintf pushes offset past buf_size. */
    for (int i = 0; i < g_num_profiles && offset + 128 < (int)buf_size; i++) {
        /* Skip profiles with empty names */
        if (strlen(g_profiles[i].name) == 0) continue;
        const char *active_marker = (i == g_active_profile_index) ? "*" : " ";
        offset += snprintf(buffer + offset, buf_size - offset,
                "PROFILE %d [%s] %s\n",
                i, active_marker, g_profiles[i].name);
    }

    offset += snprintf(buffer + offset, buf_size - offset, "PROFILES_END\n");
    return 0;
}
