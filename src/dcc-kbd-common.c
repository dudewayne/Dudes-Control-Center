// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc-kbd-common.c
 * Common implementation for Keyboard Backlight Control in Dudes Control Center
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>

#include "dcc-kbd-common.h"
#include "dcc-log.h"
#include "dcc-secure.h"
#include "dcc-common.h"

/* ============================================================================
 * GLOBAL STATE
 * ============================================================================ */

static KbdLedState g_led_state;
static KbdLedZone g_zones[KBD_MAX_ZONES_PER_PROFILE];
static KbdLedProfile g_profiles[KBD_MAX_PROFILES];

int g_num_zones = 0;
int g_num_profiles = 0;
/* Base color of the live state: keys that are in no zone (1.24.0) */
static int g_base_set = 0;
static uint8_t g_base_r, g_base_g, g_base_b;
static KbdEffect g_base_fx;            /* effect of the base color (1.35.0) */
/* Color last written to each LED, so effect frames send only changed keys */
static uint8_t g_written[KBD_MAX_LEDS][3];
static uint8_t g_written_ok[KBD_MAX_LEDS];
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

/* Kernel driver of a LED class device ("" if unknown) */
static void led_driver_name(const char *led_dir, char *out, size_t size) {
    char link[KBD_LED_PATH_MAX + 32], target[KBD_LED_PATH_MAX];
    out[0] = '\0';
    snprintf(link, sizeof link, "%s/device/driver", led_dir);
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n <= 0) return;
    target[n] = '\0';
    const char *base = strrchr(target, '/');
    base = base ? base + 1 : target;
    size_t len = strlen(base);
    if (len >= size) len = size - 1;     /* driver names are short */
    memcpy(out, base, len);
    out[len] = '\0';
}

static int read_int_file(const char *path, int fallback) {
    FILE *f = fopen(path, "r");
    if (!f) return fallback;
    int v;
    if (fscanf(f, "%d", &v) != 1) v = fallback;
    fclose(f);
    return v;
}

/* Channel order from multi_index: 0 if red, green and blue are there (a
 * missing file means "red green blue", as ite_829x), -1 otherwise */
static int led_channels(const char *led_dir, KbdLedDevice *d) {
    char path[KBD_LED_PATH_MAX + 32], line[256];
    d->ch[0] = 0; d->ch[1] = 1; d->ch[2] = 2; d->nch = 3;
    snprintf(path, sizeof path, "%s/multi_index", led_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    d->ch[0] = d->ch[1] = d->ch[2] = -1;
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, " \t\n", &save); t && n < 16; t = strtok_r(NULL, " \t\n", &save), n++) {
        if (strcmp(t, "red") == 0) d->ch[0] = n;
        else if (strcmp(t, "green") == 0) d->ch[1] = n;
        else if (strcmp(t, "blue") == 0) d->ch[2] = n;
    }
    d->nch = n;
    return (d->ch[0] >= 0 && d->ch[1] >= 0 && d->ch[2] >= 0) ? 0 : -1;
}

/* No multicolor keyboard LED: a single-color "*::kbd_backlight" (brightness
 * only). The first by name, so the choice does not depend on readdir. */
static int scan_white_led(KbdLedDevice *d) {
    DIR *dir = opendir(KBD_LEDS_DIR);
    if (!dir) return 0;
    char best[KBD_LED_NAME_MAX] = "";
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        const char *k = strstr(e->d_name, "kbd_backlight");
        if (!k || strlen(e->d_name) >= KBD_LED_NAME_MAX) continue;
        if (k[strlen("kbd_backlight")] != '\0') continue;      /* not "..._1" */
        char path[KBD_LED_PATH_MAX + 32];
        snprintf(path, sizeof path, "%s/%s/brightness", KBD_LEDS_DIR, e->d_name);
        if (access(path, F_OK) != 0) continue;
        snprintf(path, sizeof path, "%s/%s/max_brightness", KBD_LEDS_DIR, e->d_name);
        if (read_int_file(path, 0) <= 0) continue;
        if (!best[0] || strcmp(e->d_name, best) < 0)
            memcpy(best, e->d_name, strlen(e->d_name) + 1);    /* length checked above */
    }
    closedir(dir);
    if (!best[0]) return 0;
    char led_dir[KBD_LED_PATH_MAX - 32], path[KBD_LED_PATH_MAX + 32];
    snprintf(led_dir, sizeof led_dir, "%s/%s", KBD_LEDS_DIR, best);
    memset(d, 0, sizeof *d);
    snprintf(d->name, sizeof d->name, "%s", best);
    snprintf(d->path, sizeof d->path, "%s/brightness", led_dir);
    snprintf(path, sizeof path, "%s/max_brightness", led_dir);
    d->max_brightness = read_int_file(path, 1);
    d->nch = 0;
    led_driver_name(led_dir, g_led_state.driver, sizeof g_led_state.driver);
    LOG_DEBUG("[KBD] Found single-color keyboard backlight: %s (max_brightness=%d, driver %s)",
             d->name, d->max_brightness, g_led_state.driver[0] ? g_led_state.driver : "unknown");
    return 1;
}

static int scan_leds(KbdLedDevice *devices, int max_devices) {
    int count = 0;
    char led_path[KBD_LED_PATH_MAX - 32];   /* room for "/multi_intensity" in .path */
    g_led_state.kind = KBD_KIND_NONE;
    g_led_state.driver[0] = '\0';

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
                if (led_channels(led_path, &devices[count]) != 0) {
                    LOG_INFO("[KBD] %s: no red, green and blue channels - not used as a color LED", entry->d_name);
                    continue;
                }
                strncpy(devices[count].name, entry->d_name, KBD_LED_NAME_MAX - 1);
                devices[count].name[KBD_LED_NAME_MAX - 1] = '\0';
                snprintf(devices[count].path, sizeof(devices[count].path), "%s/multi_intensity", led_path);
                devices[count].max_brightness = 255;
                devices[count].scale = 0;

                char max_bright_path[KBD_LED_PATH_MAX + 32];
                snprintf(max_bright_path, sizeof(max_bright_path), "%s/max_brightness", led_path);
                devices[count].max_brightness = read_int_file(max_bright_path, 255);

                devices[count].r = 0;
                devices[count].g = 0;
                devices[count].b = 0;
                if (!g_led_state.driver[0])
                    led_driver_name(led_path, g_led_state.driver, sizeof g_led_state.driver);

                LOG_DEBUG("[KBD] Found LED: %s at %s (max_brightness=%d)",
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

    if (count >= KBD_PER_KEY_MIN_LEDS) {
        g_led_state.kind = KBD_KIND_PER_KEY;
    } else if (count > 0) {
        /* Colors are written 0-255 like ite_829x, asus-wmi, system76 and
         * tuxedo expect; scaled to max_brightness only if the kernel
         * rejects that (see led_write_raw) */
        g_led_state.kind = KBD_KIND_RGB;
        LOG_DEBUG("[KBD] %d color LED(s) without per-key layout: %s (max_brightness=%d, driver %s)",
                 count, devices[0].name, devices[0].max_brightness,
                 g_led_state.driver[0] ? g_led_state.driver : "unknown");
    } else if (max_devices > 0 && scan_white_led(&devices[0])) {
        g_led_state.kind = KBD_KIND_WHITE;
        count = 1;
    }
    return count;
}

int kbd_led_kind(void) {
    return g_led_state.kind;
}

const char *kbd_kind_name(int kind) {
    switch (kind) {
    case KBD_KIND_PER_KEY: return "per-key";
    case KBD_KIND_RGB: return "rgb";
    case KBD_KIND_WHITE: return "white";
    default: return "none";
    }
}

const char *kbd_led_driver(void) {
    return g_led_state.driver;
}

static int led_write_values(KbdLedDevice *d, uint8_t r, uint8_t g, uint8_t b) {
    int v[16] = {0};
    int nch = d->nch < 16 ? d->nch : 16;
    const uint8_t rgb[3] = { r, g, b };
    for (int c = 0; c < 3; c++) {
        int val = rgb[c];
        if (d->scale > 0) val = (val * d->scale + 127) / 255;
        if (d->ch[c] >= 0 && d->ch[c] < nch) v[d->ch[c]] = val;
    }
    char buf[16 * 5 + 2];
    size_t len = 0;
    for (int c = 0; c < nch; c++)
        len += (size_t)snprintf(buf + len, sizeof buf - len, c ? " %d" : "%d", v[c]);
    snprintf(buf + len, sizeof buf - len, "\n");

    int fd = open(d->path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, buf, strlen(buf));
    int err = errno;
    close(fd);
    errno = err;
    return n == (ssize_t)strlen(buf) ? 0 : -1;
}

static int led_write_raw(KbdLedDevice *d, uint8_t r, uint8_t g, uint8_t b) {
    if (d->nch <= 0) return 0;     /* single-color LED: on/off goes through the brightness */
    int rc = led_write_values(d, r, g, b);
    /* a driver that only takes 0..max_brightness per color */
    if (rc != 0 && errno == EINVAL && d->scale == 0 && d->max_brightness > 0 && d->max_brightness < 255) {
        d->scale = d->max_brightness;
        LOG_INFO("[KBD] %s takes colors 0-%d only - scaling", d->name, d->scale);
        rc = led_write_values(d, r, g, b);
    }
    if (rc != 0) {
        LOG_ERROR("[KBD] Cannot write LED %s: %s", d->path, strerror(errno));
        return -1;
    }
    LOG_DEBUG("[KBD] Set LED %s to RGB(%d, %d, %d)", d->path, r, g, b);
    return 0;
}

/* Write by path (other code): the cache no longer knows the LED's color */
int set_led_color(const char *led_path, uint8_t r, uint8_t g, uint8_t b, int max_brightness) {
    (void)max_brightness;
    memset(g_written_ok, 0, sizeof g_written_ok);
    for (int i = 0; i < g_led_state.num_leds && i < KBD_MAX_LEDS; i++)
        if (strcmp(g_led_state.leds[i].path, led_path) == 0)
            return led_write_raw(&g_led_state.leds[i], r, g, b);
    return -1;
}

/* Write LED i; with !force only if its color changed since the last write */
static int led_write_idx(int i, uint8_t r, uint8_t g, uint8_t b, int force) {
    if (i < 0 || i >= g_led_state.num_leds || i >= KBD_MAX_LEDS) return -1;
    if (!force && g_written_ok[i] && g_written[i][0] == r && g_written[i][1] == g && g_written[i][2] == b)
        return 0;
    if (led_write_raw(&g_led_state.leds[i], r, g, b) != 0) {
        g_written_ok[i] = 0;
        return -1;
    }
    g_written[i][0] = r; g_written[i][1] = g; g_written[i][2] = b;
    g_written_ok[i] = 1;
    return 0;
}

int set_all_leds_color(uint8_t r, uint8_t g, uint8_t b) {
    g_led_state.global_r = r;
    g_led_state.global_g = g;
    g_led_state.global_b = b;

    int success_count = 0;
    for (int i = 0; i < g_led_state.num_leds; i++) {
        if (led_write_idx(i, r, g, b, 1) == 0) {
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
        if (led_write_idx(i, scaled_r, scaled_g, scaled_b, 1) == 0) {
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

static void white_power(int on);

void kbd_led_set_power(int is_on) {
    if (g_led_state.kind == KBD_KIND_WHITE) white_power(is_on);
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

/* --- Keyboard brightness (LED "brightness" attribute) --- */
static int g_hw_brightness_last = -1;

static void led_attr_path(int i, const char *attr, char *out, size_t size) {
    snprintf(out, size, "%s", g_led_state.leds[i].path);   /* .../<led>/multi_intensity */
    char *slash = strrchr(out, '/');
    if (slash) snprintf(slash + 1, size - (size_t)(slash + 1 - out), "%s", attr);
}

static int led_read_int(int i, const char *attr) {
    char path[KBD_LED_PATH_MAX + 32];
    led_attr_path(i, attr, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static int led_write_int(int i, const char *attr, int v) {
    char path[KBD_LED_PATH_MAX + 32];
    led_attr_path(i, attr, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int ok = fprintf(f, "%d\n", v) > 0;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

static void brightness_remember(int level) {
    /* a white keyboard is switched off with brightness 0: keep the level */
    if (level <= 0 && g_led_state.kind == KBD_KIND_WHITE) return;
    mkdir(KBD_STATE_DIR, 0755);
    int fd = open(KBD_BRIGHTNESS_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char buf[16];
    int n = snprintf(buf, sizeof buf, "%d\n", level);
    if (write(fd, buf, (size_t)n) != n) LOG_WARN("[KBD] Cannot write %s", KBD_BRIGHTNESS_FILE);
    close(fd);
}

int kbd_hw_brightness_max(void) {
    if (g_led_state.num_leds == 0) return 0;
    int m = led_read_int(0, "max_brightness");
    return m > 0 ? m : g_led_state.leds[0].max_brightness;
}

int kbd_hw_brightness_get(void) {
    return g_led_state.num_leds > 0 ? led_read_int(0, "brightness") : -1;
}

int kbd_hw_brightness_set(int level) {
    if (g_led_state.num_leds == 0) return -1;
    int max = kbd_hw_brightness_max();
    if (level < 0) level = 0;
    if (level > max) level = max;
    if (led_write_int(0, "brightness", level) != 0) {
        LOG_ERROR("[KBD] Cannot set keyboard brightness: %s", strerror(errno));
        return -1;
    }
    /* ite_829x applies one LED's brightness to the whole keyboard; other
     * drivers need every LED set */
    if (g_led_state.num_leds > 1 && led_read_int(1, "brightness") != level)
        for (int i = 1; i < g_led_state.num_leds; i++) led_write_int(i, "brightness", level);
    g_hw_brightness_last = level;
    brightness_remember(level);
    LOG_INFO("[KBD] Keyboard brightness %d/%d", level, max);
    /* white keyboard: a level above 0 is "on", 0 is "off" */
    if (g_led_state.kind == KBD_KIND_WHITE && (level > 0) != (g_led_state.is_on != 0)) {
        g_led_state.is_on = level > 0;
        kbd_led_set_power(level > 0);
    }
    return 0;
}

/* Remembered level (-1 none) */
static int brightness_remembered(void) {
    int level = -1;
    FILE *f = fopen(KBD_BRIGHTNESS_FILE, "r");
    if (f) {
        if (fscanf(f, "%d", &level) != 1) level = -1;
        fclose(f);
    }
    return level;
}

/* White keyboard on/off = brightness (the level is kept for "on") */
static void white_power(int on) {
    if (g_led_state.num_leds == 0) return;
    int cur = led_read_int(0, "brightness");
    if (!on) {
        if (cur != 0 && led_write_int(0, "brightness", 0) != 0)
            LOG_ERROR("[KBD] Cannot switch the keyboard backlight off: %s", strerror(errno));
        g_hw_brightness_last = 0;
        return;
    }
    if (cur > 0) return;
    int max = kbd_hw_brightness_max();
    int level = brightness_remembered();
    if (level <= 0 || level > max) level = max;
    if (led_write_int(0, "brightness", level) != 0)
        LOG_ERROR("[KBD] Cannot switch the keyboard backlight on: %s", strerror(errno));
    g_hw_brightness_last = level;
}

int kbd_hw_brightness_cached(void) {
    return g_hw_brightness_last;
}

int kbd_hw_brightness_poll(void) {
    int cur = kbd_hw_brightness_get();
    if (cur < 0 || cur == g_hw_brightness_last) return 0;
    if (g_hw_brightness_last >= 0)
        LOG_INFO("[KBD] Keyboard brightness changed to %d (desktop slider or other program)", cur);
    g_hw_brightness_last = cur;
    brightness_remember(cur);
    /* white keyboard: the Fn key or the desktop switched it off or on */
    if (g_led_state.kind == KBD_KIND_WHITE && (cur > 0) != (g_led_state.is_on != 0)) {
        g_led_state.is_on = cur > 0;
        if (cur > 0) unlink(KBD_OFF_FLAG_FILE);
    }
    return 1;
}

void kbd_hw_brightness_restore(void) {
    /* white keyboard switched off: stays at 0 */
    if (g_led_state.kind == KBD_KIND_WHITE && !g_led_state.is_on) return;
    int level = brightness_remembered();
    int max = kbd_hw_brightness_max();
    if (level < 0 || level > max) level = max;
    kbd_hw_brightness_set(level);
}

static const char *kind_text(int kind) {
    switch (kind) {
    case KBD_KIND_PER_KEY: return "per-key RGB";
    case KBD_KIND_RGB: return "RGB without per-key layout";
    case KBD_KIND_WHITE: return "single color, brightness only";
    default: return "none";
    }
}

int kbd_led_init(void) {
    LOG_INFO("[KBD] Initializing LED subsystem...");

    memset(&g_led_state, 0, sizeof(g_led_state));
    memset(g_written_ok, 0, sizeof g_written_ok);
    g_led_state.num_leds = scan_leds(g_led_state.leds, KBD_MAX_LEDS);

    if (g_led_state.num_leds == 0) {
        LOG_WARN("[KBD] No keyboard backlight found (no LED class device \"*kbd_backlight*\"). "
                 "On Clevo laptops: sudo modprobe ite_829x. Looking again every %d seconds.", KBD_RESCAN_SEC);
    } else {
        LOG_INFO("[KBD] Found %d LED(s) (%s, ...): %s, driver %s", g_led_state.num_leds, g_led_state.leds[0].name,
                 kind_text(g_led_state.kind), g_led_state.driver[0] ? g_led_state.driver : "unknown");
    }

    g_led_state.mode = KBD_LED_MODE_OFF;
    g_led_state.is_on = 0;
    g_led_state.brightness = 255;

    return (g_led_state.num_leds > 0) ? 0 : -1;
}

int kbd_led_rescan(void) {
    g_led_state.num_leds = scan_leds(g_led_state.leds, KBD_MAX_LEDS);
    if (g_led_state.num_leds == 0) return -1;
    memset(g_written_ok, 0, sizeof g_written_ok);
    LOG_INFO("[KBD] Keyboard backlight appeared: %d LED(s) (%s, ...), %s, driver %s", g_led_state.num_leds,
             g_led_state.leds[0].name, kind_text(g_led_state.kind), g_led_state.driver[0] ? g_led_state.driver : "unknown");
    return 0;
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

/* " effect=wave:5:right" for GET_ZONES / GET_PROFILE, empty when static */
static void fx_token(const KbdEffect *fx, char *out, size_t size) {
    if (fx->type == KBD_FX_STATIC) { out[0] = '\0'; return; }
    char t[32];
    kbd_fx_format(fx, t, sizeof t);
    snprintf(out, size, " effect=%s", t);
}

int zone_list(char *buffer, size_t buf_size) {
    if (buf_size < 64) return -1;
    size_t offset = 0;
    zbuf_add(buffer, buf_size, &offset, "ZONES_START COUNT=%d\n", g_num_zones);
    char fxt[48];
    if (g_base_set) {
        fx_token(&g_base_fx, fxt, sizeof fxt);
        zbuf_add(buffer, buf_size, &offset, "BASE color=%u,%u,%u%s\n", g_base_r, g_base_g, g_base_b, fxt);
    }

    for (int i = 0; i < g_num_zones && offset + 256 < buf_size; i++) {
        fx_token(&g_zones[i].fx, fxt, sizeof fxt);
        zbuf_add(buffer, buf_size, &offset, "ZONE %d name=%s color=%u,%u,%u%s keys=%d",
                 i, g_zones[i].name, g_zones[i].r, g_zones[i].g, g_zones[i].b, fxt, g_zones[i].num_keys);
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
    g_base_set = 0;
    memset(&g_base_fx, 0, sizeof g_base_fx);
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
    (void)all_leds;
    if (num_leds < 0) {
        /* Whole keyboard: one base color, no zones (stored as the base color
         * since 1.24.0; it used to be a zone "Base" with every LED) */
        zone_clear_all();
        kbd_base_set(1, r, g, b);
        LOG_INFO("[KBD] All %d LEDs set to RGB(%u,%u,%u)", g_led_state.num_leds, r, g, b);
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

    /* A profile without any color shows all keys off, like its preview
     * (it used to leave the previous colors on and log an error) */
    if (!kbd_has_colors()) {
        LOG_INFO("[KBD] Profile '%s' has no colors - all keys off", name);
        set_all_leds_color(0, 0, 0);
        return 0;
    }
    return apply_zones();
}

int kbd_apply_active_profile(void) {
    if (g_active_profile_index < 0 || g_active_profile_index >= g_num_profiles) return -1;
    const KbdLedProfile *p = &g_profiles[g_active_profile_index];
    if (p->num_zones == 0 && !p->has_base) {
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

/* ---------------------------------------------------------------------------
 * Effects (1.35.0)
 * ------------------------------------------------------------------------- */
static const char *const fx_type_names[KBD_FX_COUNT] = { "static", "breathing", "rainbow", "wave" };
static const char *const fx_dir_names[KBD_FX_DIR_COUNT] = { "right", "left", "down", "up" };

const char *kbd_fx_type_name(int type) {
    return type >= 0 && type < KBD_FX_COUNT ? fx_type_names[type] : "static";
}

const char *kbd_fx_dir_name(int dir) {
    return dir >= 0 && dir < KBD_FX_DIR_COUNT ? fx_dir_names[dir] : "right";
}

/* "wave:5:left" -> fx; missing parts take defaults. -1 on unknown names. */
int kbd_fx_parse(const char *text, KbdEffect *fx) {
    KbdEffect f = { KBD_FX_STATIC, KBD_FX_SPEED_DEFAULT, KBD_FX_DIR_RIGHT };
    if (!text || !*text) { *fx = f; return 0; }
    char buf[64];
    snprintf(buf, sizeof buf, "%s", text);
    char *save = NULL, *part = strtok_r(buf, ":", &save);
    int t = -1;
    for (int i = 0; part && i < KBD_FX_COUNT; i++) if (strcasecmp(part, fx_type_names[i]) == 0) t = i;
    if (t < 0) return -1;
    f.type = (uint8_t)t;
    if ((part = strtok_r(NULL, ":", &save)) != NULL) {
        char *end = NULL;
        long v = strtol(part, &end, 10);
        if (end == part || *end || v < 1 || v > KBD_FX_SPEED_MAX) return -1;
        f.speed = (uint8_t)v;
    }
    if ((part = strtok_r(NULL, ":", &save)) != NULL) {
        int d = -1;
        for (int i = 0; i < KBD_FX_DIR_COUNT; i++) if (strcasecmp(part, fx_dir_names[i]) == 0) d = i;
        if (d < 0) return -1;
        f.dir = (uint8_t)d;
    }
    *fx = f;
    return 0;
}

void kbd_fx_format(const KbdEffect *fx, char *out, size_t size) {
    int sp = fx->speed ? fx->speed : KBD_FX_SPEED_DEFAULT;
    if (fx->type == KBD_FX_WAVE)
        snprintf(out, size, "wave:%d:%s", sp, kbd_fx_dir_name(fx->dir));
    else if (fx->type == KBD_FX_STATIC || fx->type >= KBD_FX_COUNT)
        snprintf(out, size, "static");
    else
        snprintf(out, size, "%s:%d", kbd_fx_type_name(fx->type), sp);
}

void kbd_base_fx_set(const KbdEffect *fx) {
    if (fx) g_base_fx = *fx;
    else memset(&g_base_fx, 0, sizeof g_base_fx);
}

static double fx_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static double fx_frac(double x) { return x - (double)(long long)x + (x < 0 && x != (double)(long long)x ? 1.0 : 0.0); }

/* hue 0..1 at full saturation and value */
static void fx_hue(double h, uint8_t rgb[3]) {
    h = fx_frac(h) * 6.0;
    int seg = (int)h;
    double f = h - seg;
    uint8_t up = (uint8_t)(f * 255.0 + 0.5), down = (uint8_t)((1.0 - f) * 255.0 + 0.5);
    switch (seg) {
    case 0:  rgb[0] = 255;  rgb[1] = up;   rgb[2] = 0;    break;
    case 1:  rgb[0] = down; rgb[1] = 255;  rgb[2] = 0;    break;
    case 2:  rgb[0] = 0;    rgb[1] = 255;  rgb[2] = up;   break;
    case 3:  rgb[0] = 0;    rgb[1] = down; rgb[2] = 255;  break;
    case 4:  rgb[0] = up;   rgb[1] = 0;    rgb[2] = 255;  break;
    default: rgb[0] = 255;  rgb[1] = 0;    rgb[2] = down; break;
    }
}

/* Color of LED i in a region with this color and effect at time t */
static void fx_color(const KbdEffect *fx, const uint8_t col[3], int i, double t, uint8_t out[3]) {
    double sp = fx->speed ? fx->speed : KBD_FX_SPEED_DEFAULT;
    switch (fx->type) {
    case KBD_FX_BREATHING: {
        /* one breath takes 12 s at speed 1, 1.2 s at speed 10 */
        double p = fx_frac(t * sp / 12.0);
        double tri = p < 0.5 ? p * 2.0 : (1.0 - p) * 2.0;
        double lvl = tri * tri * (3.0 - 2.0 * tri);          /* smooth in and out */
        lvl = 0.06 + 0.94 * lvl;
        for (int c = 0; c < 3; c++) out[c] = (uint8_t)(col[c] * lvl + 0.5);
        break;
    }
    case KBD_FX_RAINBOW:
        /* all keys together: once round the colors in 30 s (speed 1) .. 3 s (10) */
        fx_hue(t * sp / 30.0, out);
        break;
    case KBD_FX_WAVE: {
        /* one rainbow across the keyboard, moving in the chosen direction */
        int row = i / KBD_MATRIX_COLUMNS, colm = i % KBD_MATRIX_COLUMNS;
        double pos = (fx->dir == KBD_FX_DIR_DOWN || fx->dir == KBD_FX_DIR_UP)
                     ? (double)row / KBD_MATRIX_ROWS : (double)colm / KBD_MATRIX_COLUMNS;
        double shift = t * sp / 15.0;
        int toward_end = fx->dir == KBD_FX_DIR_RIGHT || fx->dir == KBD_FX_DIR_DOWN;
        fx_hue(toward_end ? pos - shift : pos + shift, out);
        break;
    }
    default:
        memcpy(out, col, 3);
    }
}

/* Live colors of every LED at time t: base color, then the zones (a later
 * zone wins), effects applied, scaled by the software brightness */
static void kbd_compose(uint8_t out[KBD_MAX_LEDS][3], double t) {
    int n = g_led_state.num_leds < KBD_MAX_LEDS ? g_led_state.num_leds : KBD_MAX_LEDS;
    memset(out, 0, sizeof(uint8_t) * 3 * KBD_MAX_LEDS);
    if (g_base_set) {
        uint8_t base[3] = { g_base_r, g_base_g, g_base_b };
        for (int i = 0; i < n; i++) fx_color(&g_base_fx, base, i, t, out[i]);
    }
    for (int z = 0; z < g_num_zones; z++) {
        const KbdLedZone *zone = &g_zones[z];
        uint8_t zc[3] = { zone->r, zone->g, zone->b };
        for (int k = 0; k < zone->num_keys; k++) {
            int led = zone->key_indices[k];
            if (led >= 0 && led < n) fx_color(&zone->fx, zc, led, t, out[led]);
        }
    }
    for (int i = 0; i < n; i++) apply_brightness_scale(&out[i][0], &out[i][1], &out[i][2], g_led_state.brightness);
}

int kbd_fx_running(void) {
    if (!g_led_state.is_on || g_led_state.num_leds == 0) return 0;
    if (g_base_set && g_base_fx.type != KBD_FX_STATIC) return 1;
    for (int z = 0; z < g_num_zones; z++)
        if (g_zones[z].fx.type != KBD_FX_STATIC && g_zones[z].num_keys > 0) return 1;
    return 0;
}

void kbd_fx_frame(void) {
    static uint8_t colors[KBD_MAX_LEDS][3];
    kbd_compose(colors, fx_now());
    for (int i = 0; i < g_led_state.num_leds && i < KBD_MAX_LEDS; i++)
        led_write_idx(i, colors[i][0], colors[i][1], colors[i][2], 0);
}

/* Every zone and the base color get this effect (dcc-kbd-cli --effect);
 * without any color the whole keyboard becomes white first */
int kbd_fx_set_all(const KbdEffect *fx) {
    if (!g_base_set && g_num_zones == 0) kbd_base_set(1, 255, 255, 255);
    g_base_fx = *fx;
    for (int z = 0; z < g_num_zones; z++) g_zones[z].fx = *fx;
    char t[32];
    kbd_fx_format(fx, t, sizeof t);
    LOG_INFO("[KBD] Effect for the whole keyboard: %s", t);
    return apply_zones();
}

int apply_zones_with_brightness(void) {
    if (g_num_zones == 0 && !g_base_set) {
        LOG_ERROR("[KBD] No zones to apply");
        return -1;
    }

    static uint8_t led_colors[KBD_MAX_LEDS][3];
    kbd_compose(led_colors, fx_now());

    int success_count = 0;
    for (int i = 0; i < g_led_state.num_leds && i < KBD_MAX_LEDS; i++)
        if (led_write_idx(i, led_colors[i][0], led_colors[i][1], led_colors[i][2], 1) == 0) success_count++;

    LOG_INFO("[KBD] Applied %d zones to %d LEDs (brightness: %d/255)%s", g_num_zones, success_count,
             g_led_state.brightness, kbd_fx_running() ? ", with effects" : "");
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
        if (p->zones[i].fx.type != KBD_FX_STATIC) {
            char fx[32];
            kbd_fx_format(&p->zones[i].fx, fx, sizeof fx);
            cJSON_AddStringToObject(zone_obj, "effect", fx);
        }
        cJSON_AddItemToArray(zones_array, zone_obj);
    }
    cJSON_AddItemToObject(profile_obj, "zones", zones_array);
    if (p->has_base) {
        cJSON *base = cJSON_CreateObject();
        cJSON_AddNumberToObject(base, "r", p->base_r);
        cJSON_AddNumberToObject(base, "g", p->base_g);
        cJSON_AddNumberToObject(base, "b", p->base_b);
        if (p->base_fx.type != KBD_FX_STATIC) {
            char fx[32];
            kbd_fx_format(&p->base_fx, fx, sizeof fx);
            cJSON_AddStringToObject(base, "effect", fx);
        }
        cJSON_AddItemToObject(profile_obj, "base", base);
    }

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
                    cJSON *zfx = cJSON_GetObjectItem(zone_obj, "effect");
                    if (cJSON_IsString(zfx) && kbd_fx_parse(zfx->valuestring, &p.zones[z].fx) != 0) {
                        LOG_WARN("[KBD] Profile '%s': unknown effect '%s' in zone '%s'",
                                 p.name, zfx->valuestring, p.zones[z].name);
                    }
                }
                p.num_zones = nz;
            }

            cJSON *base = cJSON_GetObjectItem(profile_obj, "base");
            if (cJSON_IsObject(base)) {
                cJSON *br = cJSON_GetObjectItem(base, "r"), *bg = cJSON_GetObjectItem(base, "g"),
                      *bb = cJSON_GetObjectItem(base, "b");
                p.has_base = 1;
                p.base_r = (uint8_t)clamp_u8(cJSON_IsNumber(br) ? br->valueint : 0);
                p.base_g = (uint8_t)clamp_u8(cJSON_IsNumber(bg) ? bg->valueint : 0);
                p.base_b = (uint8_t)clamp_u8(cJSON_IsNumber(bb) ? bb->valueint : 0);
                cJSON *bfx = cJSON_GetObjectItem(base, "effect");
                if (cJSON_IsString(bfx) && kbd_fx_parse(bfx->valuestring, &p.base_fx) != 0)
                    LOG_WARN("[KBD] Profile '%s': unknown base effect '%s'", p.name, bfx->valuestring);
            } else {
                /* Before 1.24.0 "whole keyboard" was a zone named "Base" that
                 * held every LED not in another zone: that is the base color */
                for (int z = 0; z < p.num_zones; z++) {
                    if (strcmp(p.zones[z].name, "Base") != 0) continue;
                    p.has_base = 1;
                    p.base_r = p.zones[z].r;
                    p.base_g = p.zones[z].g;
                    p.base_b = p.zones[z].b;
                    memmove(&p.zones[z], &p.zones[z + 1], (size_t)(p.num_zones - z - 1) * sizeof p.zones[0]);
                    p.num_zones--;
                    LOG_INFO("[KBD] Profile '%s': zone 'Base' is now the base color", p.name);
                    break;
                }
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
            g_profiles[i].has_base = g_base_set;
            g_profiles[i].base_r = g_base_r;
            g_profiles[i].base_g = g_base_g;
            g_profiles[i].base_b = g_base_b;
            g_profiles[i].base_fx = g_base_fx;
            LOG_INFO("[KBD] Updated profile '%s' with %d zones%s", name, g_num_zones,
                     g_base_set ? " and a base color" : "");
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
    p->has_base = g_base_set;
    p->base_r = g_base_r;
    p->base_g = g_base_g;
    p->base_b = g_base_b;
    p->base_fx = g_base_fx;
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
            kbd_base_set(g_profiles[i].has_base, g_profiles[i].base_r, g_profiles[i].base_g, g_profiles[i].base_b);
            kbd_base_fx_set(&g_profiles[i].base_fx);
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

            /* Keep the same profile active: a profile before it moves it up
             * one place (it used to make the next profile active). Deleting
             * the active profile makes the one now at its place active. */
            if (i < g_active_profile_index) g_active_profile_index--;
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

/* ============================================================================
 * Base color, preview and profile description (1.24.0)
 * ============================================================================ */

void kbd_base_set(int has_base, uint8_t r, uint8_t g, uint8_t b) {
    g_base_set = has_base ? 1 : 0;
    g_base_r = r;
    g_base_g = g;
    g_base_b = b;
}

int kbd_active_profile_valid(void) {
    return g_active_profile_index >= 0 && g_active_profile_index < g_num_profiles;
}

int kbd_has_colors(void) {
    return g_num_zones > 0 || g_base_set;
}

/* Write the live colors; a profile without any color shows all keys off */
int kbd_show_colors(void) {
    if (kbd_has_colors()) return apply_zones();
    return set_all_leds_color(0, 0, 0);
}

/* "r,g,b" or "r,g,b@effect" (1.35.0) */
static int parse_color_fx(const char *text, uint8_t *r, uint8_t *g, uint8_t *b, KbdEffect *fx) {
    char buf[96];
    snprintf(buf, sizeof buf, "%s", text);
    char *at = strchr(buf, '@');
    memset(fx, 0, sizeof *fx);
    if (at) {
        *at = '\0';
        if (kbd_fx_parse(at + 1, fx) != 0) return -1;
    }
    return parse_rgb(buf, r, g, b);
}

/* Replace the live colors (the GUI's profile being edited).
 * base:  "r,g,b" or "-" (no base color); "r,g,b@wave:5:right" with an effect
 * zones: "name|r,g,b|led,led,...;name|r,g,b@effect|...", may be empty.
 * Zone names must not contain '|' or ';'. Returns -1 on a format error
 * (the live colors are then unchanged). */
int kbd_preview_set(const char *base, char *zones) {
    static KbdLedZone nz[KBD_MAX_ZONES_PER_PROFILE];
    int n = 0, has_base = 0;
    uint8_t br = 0, bg = 0, bb = 0;
    KbdEffect bfx;
    memset(&bfx, 0, sizeof bfx);
    memset(nz, 0, sizeof nz);

    if (base && *base && strcmp(base, "-") != 0) {
        if (parse_color_fx(base, &br, &bg, &bb, &bfx) != 0) return -1;
        has_base = 1;
    }
    char *save = NULL;
    for (char *z = zones ? strtok_r(zones, ";", &save) : NULL; z; z = strtok_r(NULL, ";", &save)) {
        if (n >= KBD_MAX_ZONES_PER_PROFILE) return -1;
        char *bar1 = strchr(z, '|');
        char *bar2 = bar1 ? strchr(bar1 + 1, '|') : NULL;
        if (!bar1 || !bar2 || bar1 == z) return -1;
        *bar1 = '\0';
        *bar2 = '\0';
        KbdLedZone *zone = &nz[n];
        snprintf(zone->name, sizeof zone->name, "%s", z);
        if (parse_color_fx(bar1 + 1, &zone->r, &zone->g, &zone->b, &zone->fx) != 0) return -1;
        char *ksave = NULL;
        for (char *k = strtok_r(bar2 + 1, ",", &ksave); k; k = strtok_r(NULL, ",", &ksave)) {
            char *end = NULL;
            long v = strtol(k, &end, 10);
            if (end == k || v < 0 || v >= g_led_state.num_leds || v >= KBD_MAX_LEDS) continue;
            if (zone->num_keys < KBD_MAX_KEYS_PER_ZONE) zone->key_indices[zone->num_keys++] = (int)v;
        }
        n++;
    }

    zone_clear_all();
    memcpy(g_zones, nz, sizeof(KbdLedZone) * (size_t)n);
    g_num_zones = n;
    kbd_base_set(has_base, br, bg, bb);
    kbd_base_fx_set(has_base ? &bfx : NULL);
    LOG_INFO("[KBD] Preview: %d zone(s)%s", n, has_base ? " + base color" : "");
    return 0;
}

/* A saved profile in the GET_ZONES format, so the GUI can edit it:
 * PROFILE_START name=..\nBASE color=..\nZONE i name=.. color=.. keys=N ...\nPROFILE_END */
int kbd_profile_describe(const char *name, char *buffer, size_t buf_size) {
    if (buf_size < 64) return -1;
    for (int i = 0; i < g_num_profiles; i++) {
        const KbdLedProfile *p = &g_profiles[i];
        if (strcmp(p->name, name) != 0) continue;
        size_t off = 0;
        zbuf_add(buffer, buf_size, &off, "PROFILE_START COUNT=%d name=%s\n", p->num_zones, p->name);
        char fxt[48];
        if (p->has_base) {
            fx_token(&p->base_fx, fxt, sizeof fxt);
            zbuf_add(buffer, buf_size, &off, "BASE color=%u,%u,%u%s\n", p->base_r, p->base_g, p->base_b, fxt);
        }
        for (int z = 0; z < p->num_zones && off + 256 < buf_size; z++) {
            const KbdLedZone *zone = &p->zones[z];
            fx_token(&zone->fx, fxt, sizeof fxt);
            zbuf_add(buffer, buf_size, &off, "ZONE %d name=%s color=%u,%u,%u%s keys=%d",
                     z, zone->name, zone->r, zone->g, zone->b, fxt, zone->num_keys);
            for (int k = 0; k < zone->num_keys && off + 16 < buf_size; k++)
                zbuf_add(buffer, buf_size, &off, " %d", zone->key_indices[k]);
            zbuf_add(buffer, buf_size, &off, "\n");
        }
        zbuf_add(buffer, buf_size, &off, "PROFILE_END\n");
        return 0;
    }
    return -1;
}

/* Live colors back to the active profile (the GUI stopped previewing).
 * Shown only while the light is on, unless light_on. */
int kbd_restore_active(int light_on) {
    if (g_active_profile_index >= 0 && g_active_profile_index < g_num_profiles)
        kbd_profile_load_json(g_profiles[g_active_profile_index].name);
    else
        zone_clear_all();
    if (!light_on && !g_led_state.is_on) return 0;
    /* An active profile without colors keeps the keys off (like activating
     * it); white only when there is no profile at all (1.24.1) */
    if (kbd_active_profile_valid()) return kbd_show_colors();
    return set_all_leds_color(255, 255, 255);
}
