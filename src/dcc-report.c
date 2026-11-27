// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc-report.c
 * System report (1.45.0), see dcc-report.h.
 */

#define _GNU_SOURCE
#include "dcc-report.h"
#include "dcc-common.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef REPORT_FAN_SOCKET
#define REPORT_FAN_SOCKET "/run/dcc-fan.sock"
#endif
#ifndef REPORT_CPU_SOCKET
#define REPORT_CPU_SOCKET "/run/dcc-cpu.sock"
#endif
#ifndef REPORT_KBD_SOCKET
#define REPORT_KBD_SOCKET "/run/dcc-kbd.sock"
#endif
#define REPORT_ETC_DIR "/etc/dudes-control-center"

/* --- growing text buffer --- */
typedef struct { char *s; size_t len, cap; int oom; } Buf;

static void bput(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bput(Buf *b, const char *fmt, ...) {
    if (b->oom) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->s ? b->s + b->len : NULL, b->s ? b->cap - b->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (b->s && b->len + (size_t)n < b->cap) { b->len += (size_t)n; return; }
        size_t cap = b->cap ? b->cap * 2 : 8192;
        while (cap <= b->len + (size_t)n) cap *= 2;
        char *s = realloc(b->s, cap);
        if (!s) { b->oom = 1; return; }
        b->s = s;
        b->cap = cap;
    }
}

/* --- file helpers (all under DCC_SYSROOT) --- */
static void rpath(char *out, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void rpath(char *out, size_t size, const char *fmt, ...) {
    char p[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p, sizeof p, fmt, ap);
    va_end(ap);
    snprintf(out, size, "%s%s", DCC_SYSROOT, p);
}

/* First line of a file without the newline; "" if missing */
static void rline(const char *path, char *out, size_t size) {
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)size, f)) out[strcspn(out, "\n")] = '\0';
    fclose(f);
}

static int rexists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Basename of a symlink's target ("" if none), e.g. device/driver */
static void rlink_base(const char *path, char *out, size_t size) {
    char t[1024];
    out[0] = '\0';
    ssize_t n = readlink(path, t, sizeof t - 1);
    if (n <= 0) return;
    t[n] = '\0';
    const char *b = strrchr(t, '/');
    b = b ? b + 1 : t;
    size_t l = strlen(b);
    if (l >= size) l = size - 1;
    memcpy(out, b, l);
    out[l] = '\0';
}

static int name_cmp(const void *a, const void *b) {
    return strverscmp(*(char *const *)a, *(char *const *)b);
}

/* Sorted entries of a directory (free with free_list); NULL if missing */
static char **list_dir(const char *dir, int *count) {
    *count = 0;
    DIR *d = opendir(dir);
    if (!d) return NULL;
    char **v = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            char **nv = realloc(v, (size_t)cap * sizeof *v);
            if (!nv) break;
            v = nv;
        }
        v[n] = strdup(e->d_name);
        if (v[n]) n++;
    }
    closedir(d);
    if (n) qsort(v, (size_t)n, sizeof *v, name_cmp);
    *count = n;
    return v;
}

static void free_list(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* --- daemon STATUS over its socket (1 s timeout) --- */
static int ask_daemon(const char *sock, const char *cmd, char *out, size_t size) {
    out[0] = '\0';
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { int e = errno; close(fd); errno = e; return -1; }
    char line[128];
    int n = snprintf(line, sizeof line, "%s\n", cmd);
    if (send(fd, line, (size_t)n, MSG_NOSIGNAL) != n) { close(fd); return -1; }
    size_t len = 0;
    int wait_ms = 1000;
    while (len + 1 < size) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, wait_ms) <= 0) break;
        ssize_t r = read(fd, out + len, size - 1 - len);
        if (r <= 0) break;
        len += (size_t)r;
        wait_ms = 150;          /* the rest of a multi-line answer */
    }
    out[len] = '\0';
    close(fd);
    return len > 0 ? 0 : -1;
}

/* KEY=VALUE lines of a STATUS answer. Per-core lines CPU_<n>_<KEY> are
 * summed up per KEY: one value on all cores, or how many lines there are. */
typedef struct { char key[48]; char value[64]; int count; int same; } CoreKey;

static void put_status(Buf *b, const char *title, const char *sock) {
    static char resp[16384];
    bput(b, "\n== %s ==\n", title);
    if (ask_daemon(sock, "STATUS", resp, sizeof resp) != 0) {
        bput(b, "not reachable (%s)\n", errno ? strerror(errno) : "no answer");
        return;
    }
    CoreKey keys[16];
    int nk = 0;
    char *save = NULL;
    for (char *l = strtok_r(resp, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(l, '=');
        if (!eq) continue;
        if (!strncmp(l, "CPU_", 4) && isdigit((unsigned char)l[4])) {
            const char *k = l + 4;
            while (isdigit((unsigned char)*k)) k++;
            if (*k == '_' && k < eq) {
                k++;
                size_t kl = (size_t)(eq - k);
                int i = 0;
                while (i < nk && (strlen(keys[i].key) != kl || strncmp(keys[i].key, k, kl))) i++;
                if (i == nk && nk < 16 && kl < sizeof keys[0].key) {
                    memcpy(keys[i].key, k, kl);
                    keys[i].key[kl] = '\0';
                    snprintf(keys[i].value, sizeof keys[i].value, "%s", eq + 1);
                    keys[i].count = 0;
                    keys[i].same = 1;
                    nk++;
                }
                if (i < nk) {
                    keys[i].count++;
                    if (strcmp(keys[i].value, eq + 1)) keys[i].same = 0;
                }
                continue;
            }
        }
        bput(b, "%s\n", l);
    }
    for (int i = 0; i < nk; i++) {
        if (keys[i].same) bput(b, "CPU_n_%s=%s (all %d cores)\n", keys[i].key, keys[i].value, keys[i].count);
        else bput(b, "CPU_n_%s: %d per-core values left out\n", keys[i].key, keys[i].count);
    }
}

/* --- sections --- */
static void put_system(Buf *b, const char *version) {
    char p[1100], v[256], w[256], x[256], y[256];
    bput(b, "Dudes Control Center system report\n");
    bput(b, "(no user names, host names, serial numbers or home folder paths)\n");
    bput(b, "\n== Program ==\nVersion: %s\n", version ? version : "?");

    bput(b, "\n== System ==\n");
    rpath(p, sizeof p, "/sys/class/dmi/id/sys_vendor"); rline(p, v, sizeof v);
    rpath(p, sizeof p, "/sys/class/dmi/id/product_name"); rline(p, w, sizeof w);
    rpath(p, sizeof p, "/sys/class/dmi/id/board_vendor"); rline(p, x, sizeof x);
    rpath(p, sizeof p, "/sys/class/dmi/id/board_name"); rline(p, y, sizeof y);
    bput(b, "Model: %s %s (board: %s %s)\n", v[0] ? v : "?", w[0] ? w : "?", x[0] ? x : "?", y[0] ? y : "?");
    rpath(p, sizeof p, "/sys/class/dmi/id/bios_version"); rline(p, v, sizeof v);
    rpath(p, sizeof p, "/sys/class/dmi/id/bios_date"); rline(p, w, sizeof w);
    rpath(p, sizeof p, "/sys/class/dmi/id/bios_vendor"); rline(p, x, sizeof x);
    bput(b, "BIOS: %s (%s, %s)\n", v[0] ? v : "?", w[0] ? w : "?", x[0] ? x : "?");

    v[0] = '\0';
    rpath(p, sizeof p, "/etc/os-release");
    FILE *f = fopen(p, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "PRETTY_NAME=", 12)) {
                char *s = line + 12;
                s[strcspn(s, "\n")] = '\0';
                if (*s == '"') { s++; char *q = strrchr(s, '"'); if (q) *q = '\0'; }
                size_t l = strlen(s);
                if (l >= sizeof v) l = sizeof v - 1;
                memcpy(v, s, l);
                v[l] = '\0';
            }
        fclose(f);
    }
    bput(b, "Distribution: %s\n", v[0] ? v : "?");
    struct utsname u;
    if (uname(&u) == 0) bput(b, "Kernel: %s (%s)\n", u.release, u.machine);

    rpath(p, sizeof p, "/proc/cpuinfo");
    f = fopen(p, "r");
    int threads = 0;
    v[0] = '\0';
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "processor", 9)) threads++;
            if (!v[0] && !strncmp(line, "model name", 10)) {
                char *c = strchr(line, ':');
                if (c) {
                    c++;
                    while (*c == ' ') c++;
                    c[strcspn(c, "\n")] = '\0';
                    size_t l = strlen(c);
                    if (l >= sizeof v) l = sizeof v - 1;
                    memcpy(v, c, l);
                    v[l] = '\0';
                }
            }
        }
        fclose(f);
    }
    bput(b, "CPU: %s, %d threads\n", v[0] ? v : "?", threads);
}

static void put_modules_settings(Buf *b) {
    char p[1100], v[128];
    bput(b, "\n== Kernel modules ==\n");
    const char *mods[] = { "dcc_fan", "ite_829x", "clevo_wmi", "tuxedo_keyboard", "thinkpad_acpi",
                           "dell_smm_hwmon", "asus_wmi", "k10temp", "zenpower", "coretemp",
                           "amdgpu", "nvidia", "nouveau", "i915", "xe" };
    for (size_t i = 0; i < sizeof mods / sizeof *mods; i++) {
        rpath(p, sizeof p, "/sys/module/%s", mods[i]);
        if (!rexists(p)) continue;
        rpath(p, sizeof p, "/sys/module/%s/version", mods[i]);
        rline(p, v, sizeof v);
        bput(b, "%s: loaded%s%s\n", mods[i], v[0] ? ", version " : "", v);
    }

    bput(b, "\n== Settings ==\n");
    const char *comps[] = { "fan", "cpu", "keyboard" };
    int off = 0;
    for (int i = 0; i < 3; i++) {
        rpath(p, sizeof p, "%s/disabled-%s", REPORT_ETC_DIR, comps[i]);
        if (rexists(p)) { bput(b, "%s%s", off ? ", " : "Switched off: ", comps[i]); off++; }
    }
    bput(b, "%s\n", off ? "" : "Switched off: none");
    bput(b, "Generic fan control: %s\n", hwmon_fans_allowed() ? "enabled" : "off");
    bput(b, "Direct EC access: %s\n", ec_direct_allowed() ? "enabled" : "off");
}

/* Numbered attribute files like temp3_input: how many */
static int count_attr(char **ent, int n, const char *prefix, const char *suffix) {
    int c = 0;
    size_t pl = strlen(prefix), sl = strlen(suffix);
    for (int i = 0; i < n; i++) {
        size_t l = strlen(ent[i]);
        if (l > pl + sl && !strncmp(ent[i], prefix, pl) && !strcmp(ent[i] + l - sl, suffix) &&
            isdigit((unsigned char)ent[i][pl]))
            c++;
    }
    return c;
}

static void put_hwmon(Buf *b) {
    char dir[1100], p[1200], v[128], drv[128];
    bput(b, "\n== hwmon (sensors and fans) ==\n");
    rpath(dir, sizeof dir, "/sys/class/hwmon");
    int n;
    char **hw = list_dir(dir, &n);
    if (!n) bput(b, "none\n");
    for (int i = 0; i < n; i++) {
        snprintf(p, sizeof p, "%s/%s/name", dir, hw[i]);
        rline(p, v, sizeof v);
        snprintf(p, sizeof p, "%s/%s/device/driver", dir, hw[i]);
        rlink_base(p, drv, sizeof drv);
        snprintf(p, sizeof p, "%s/%s", dir, hw[i]);
        int m;
        char **ent = list_dir(p, &m);
        bput(b, "%s: %s%s%s - temp %d, fan %d", hw[i], v[0] ? v : "?", drv[0] ? ", driver " : "", drv,
             count_attr(ent, m, "temp", "_input"), count_attr(ent, m, "fan", "_input"));
        /* pwm channels with their mode (1 manual, 2 automatic) */
        for (int k = 1; k <= 8; k++) {
            char pwm[32], en[128];
            snprintf(pwm, sizeof pwm, "pwm%d", k);
            int has = 0;
            for (int j = 0; j < m; j++) if (!strcmp(ent[j], pwm)) has = 1;
            if (!has) continue;
            snprintf(p, sizeof p, "%s/%s/pwm%d_enable", dir, hw[i], k);
            rline(p, en, sizeof en);
            bput(b, ", pwm%d (enable %s)", k, en[0] ? en : "-");
        }
        bput(b, "\n");
        free_list(ent, m);
    }
    free_list(hw, n);

    bput(b, "\n== Thermal zones ==\n");
    rpath(dir, sizeof dir, "/sys/class/thermal");
    char **tz = list_dir(dir, &n);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(tz[i], "thermal_zone", 12)) continue;
        snprintf(p, sizeof p, "%s/%s/type", dir, tz[i]);
        rline(p, v, sizeof v);
        bput(b, "%s%s: %s", shown ? ", " : "", tz[i] + 12, v[0] ? v : "?");
        shown++;
    }
    bput(b, "%s\n", shown ? "" : "none");
    free_list(tz, n);
}

static void put_leds(Buf *b) {
    char dir[1100], p[1200], v[128], mi[128], mx[32], br[32], drv[128];
    bput(b, "\n== Keyboard LEDs (/sys/class/leds) ==\n");
    rpath(dir, sizeof dir, "/sys/class/leds");
    int n, shown = 0, numbered = 0;
    char **le = list_dir(dir, &n);
    for (int i = 0; i < n; i++) {
        if (!strstr(le[i], "kbd") && !strstr(le[i], "keyboard")) continue;
        /* rgb:kbd_backlight_1..N of a per-key keyboard: only counted */
        const char *k = strstr(le[i], "kbd_backlight_");
        if (k && isdigit((unsigned char)k[14])) { numbered++; continue; }
        snprintf(p, sizeof p, "%s/%s/multi_index", dir, le[i]); rline(p, mi, sizeof mi);
        snprintf(p, sizeof p, "%s/%s/max_brightness", dir, le[i]); rline(p, mx, sizeof mx);
        snprintf(p, sizeof p, "%s/%s/brightness", dir, le[i]); rline(p, br, sizeof br);
        snprintf(p, sizeof p, "%s/%s/device/driver", dir, le[i]); rlink_base(p, drv, sizeof drv);
        snprintf(p, sizeof p, "%s/%s/multi_intensity", dir, le[i]);
        int multi = rexists(p);
        bput(b, "%s: %s, brightness %s/%s%s%s%s%s\n", le[i], multi ? "multicolor" : "single color",
             br[0] ? br : "?", mx[0] ? mx : "?", mi[0] ? ", multi_index " : "", mi,
             drv[0] ? ", driver " : "", drv);
        shown++;
    }
    if (numbered) bput(b, "plus %d numbered LEDs *kbd_backlight_N (one per key)\n", numbered);
    if (!shown && !numbered) bput(b, "none\n");
    (void)v;
    free_list(le, n);
}

static void put_platform_gpu_power(Buf *b) {
    char dir[1100], p[1200], v[256], w[512], x[64], y[64], z[64];
    bput(b, "\n== Firmware power profile ==\n");
    rpath(p, sizeof p, "/sys/firmware/acpi/platform_profile"); rline(p, v, sizeof v);
    rpath(p, sizeof p, "/sys/firmware/acpi/platform_profile_choices"); rline(p, w, sizeof w);
    if (v[0]) bput(b, "platform_profile: %s (choices: %s)\n", v, w[0] ? w : "?");
    else bput(b, "not offered\n");

    bput(b, "\n== Graphics (DRM) ==\n");
    rpath(dir, sizeof dir, "/sys/class/drm");
    int n, shown = 0;
    char **cards = list_dir(dir, &n);
    for (int i = 0; i < n; i++) {
        if (strncmp(cards[i], "card", 4) || strchr(cards[i], '-')) continue;   /* not card0-eDP-1 */
        snprintf(p, sizeof p, "%s/%s/device/driver", dir, cards[i]); rlink_base(p, v, sizeof v);
        snprintf(p, sizeof p, "%s/%s/device/vendor", dir, cards[i]); rline(p, x, sizeof x);
        snprintf(p, sizeof p, "%s/%s/device/device", dir, cards[i]); rline(p, y, sizeof y);
        snprintf(p, sizeof p, "%s/%s/device/boot_vga", dir, cards[i]); rline(p, z, sizeof z);
        snprintf(p, sizeof p, "%s/%s/device/power/runtime_status", dir, cards[i]); rline(p, w, sizeof w);
        bput(b, "%s: driver %s, id %s:%s, boot_vga %s, power %s\n", cards[i], v[0] ? v : "?",
             x[0] ? x : "?", y[0] ? y : "?", z[0] ? z : "-", w[0] ? w : "?");
        shown++;
    }
    if (!shown) bput(b, "none\n");
    free_list(cards, n);

    bput(b, "\n== Power supplies ==\n");
    rpath(dir, sizeof dir, "/sys/class/power_supply");
    char **ps = list_dir(dir, &n);
    shown = 0;
    for (int i = 0; i < n; i++) {
        snprintf(p, sizeof p, "%s/%s/type", dir, ps[i]); rline(p, v, sizeof v);
        snprintf(p, sizeof p, "%s/%s/charge_control_end_threshold", dir, ps[i]);
        int thr = rexists(p);
        bput(b, "%s: %s%s\n", ps[i], v[0] ? v : "?", thr ? ", charge_control_end_threshold" : "");
        shown++;
    }
    if (!shown) bput(b, "none\n");
    free_list(ps, n);
}

/* --- privacy --- */
static int word_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.';
}

/* Replace every occurrence of what (as a whole word if whole) by with */
static char *replace_all(char *text, const char *what, const char *with, int whole) {
    size_t wl = what ? strlen(what) : 0;
    if (!text || wl < 2) return text;
    Buf b = {0};
    const char *s = text;
    for (const char *q; (q = strstr(s, what)) != NULL; ) {
        int ok = !whole || ((q == text || !word_char(q[-1])) && !word_char(q[wl]));
        bput(&b, "%.*s%s", (int)(q - s), s, ok ? with : what);
        s = q + wl;
    }
    bput(&b, "%s", s);
    if (b.oom || !b.s) { free(b.s); return text; }
    free(text);
    return b.s;
}

char *dcc_report_scrub(char *text, const char *user, const char *home, const char *host) {
    if (home && strlen(home) > 1) text = replace_all(text, home, "<home>", 0);
    if (user && *user) {
        char h[300];
        snprintf(h, sizeof h, "/home/%s", user);
        text = replace_all(text, h, "<home>", 0);
        text = replace_all(text, user, "<user>", 1);
    }
    if (host && *host) text = replace_all(text, host, "<host>", 1);
    return text;
}

char *dcc_report_build(const char *version) {
    Buf b = {0};
    put_system(&b, version);
    put_modules_settings(&b);
    put_status(&b, "Fan daemon (STATUS)", REPORT_FAN_SOCKET);
    put_status(&b, "CPU daemon (STATUS)", REPORT_CPU_SOCKET);
    put_status(&b, "Keyboard daemon (STATUS)", REPORT_KBD_SOCKET);
    put_hwmon(&b);
    put_leds(&b);
    put_platform_gpu_power(&b);
    if (b.oom) { free(b.s); return NULL; }

    /* Who could be named in it: this user, the desktop user the daemons
     * serve (/etc/dudes-control-center/user), sudo's user; the host */
    char *text = b.s;
    struct passwd *pw = getpwuid(getuid());
    char desktop[256], p[1100], host[256] = "";
    rpath(p, sizeof p, "%s/user", REPORT_ETC_DIR);
    rline(p, desktop, sizeof desktop);
    if (gethostname(host, sizeof host) != 0) host[0] = '\0';
    host[sizeof host - 1] = '\0';
    text = dcc_report_scrub(text, pw ? pw->pw_name : NULL, getenv("HOME"), host);
    text = dcc_report_scrub(text, getenv("SUDO_USER"), NULL, NULL);
    text = dcc_report_scrub(text, desktop, NULL, NULL);
    return text;
}
