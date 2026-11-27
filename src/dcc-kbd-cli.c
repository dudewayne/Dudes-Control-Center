// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc-kbd-cli.c - command-line interface for the keyboard backlight (1.32.0)
 *
 * Talks to the keyboard daemon (dcc-kbd-daemon) over its socket, like
 * dcc-fan-cli and dcc-cpu-cli talk to theirs. Needs membership in the
 * dudes-control-center group (or root).
 *
 *   dcc-kbd-cli                        status
 *   dcc-kbd-cli --on | --off           backlight on / off
 *   dcc-kbd-cli --brightness <0-max>   hardware brightness
 *   dcc-kbd-cli --color <color>        whole keyboard in one color (not saved)
 *   dcc-kbd-cli --effect <effect>      whole keyboard effect (not saved, 1.35.0)
 *   dcc-kbd-cli --list-profiles        keyboard profiles, the active one marked
 *   dcc-kbd-cli --profile <name>       activate a profile
 *   dcc-kbd-cli --show-profile <name>  base color and zones of a profile
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#ifndef KBD_SOCKET_PATH
#define KBD_SOCKET_PATH "/run/dcc-kbd.sock"
#endif
#define PROGRAM_NAME "dcc-kbd-cli"
#define REPLY_MAX 16384

static void print_usage(void) {
    printf("Usage: %s [OPTIONS]\n\n", PROGRAM_NAME);
    printf("Dudes Control Center - CLI interface for the keyboard backlight\n\n");
    printf("Options:\n");
    printf("  (none)                Show the keyboard backlight status (default)\n");
    printf("  --on                  Switch the backlight on\n");
    printf("  --off                 Switch the backlight off (also after a reboot)\n");
    printf("  --brightness <level>  Set the hardware brightness (0 to the maximum, usually 10)\n");
    printf("  --color <color>       Whole keyboard in one color, not saved in a profile:\n");
    printf("                        R,G,B (0-255), #RRGGBB or white, red, green, blue,\n");
    printf("                        yellow, cyan, magenta, orange, purple\n");
    printf("  --effect <effect>     Effect for the whole keyboard, not saved in a profile:\n");
    printf("                        static, breathing[:speed], rainbow[:speed],\n");
    printf("                        wave[:speed[:right|left|down|up]]; speed 1-10 (default 5)\n");
    printf("  --list-profiles       List the keyboard profiles (* = active)\n");
    printf("  --profile <name>      Activate a keyboard profile\n");
    printf("  --show-profile <name> Show the base color and zones of a profile\n");
    printf("  --help                Show this help message\n\n");
    printf("Examples:\n");
    printf("  %s                         # Show the status\n", PROGRAM_NAME);
    printf("  %s --brightness 5          # Half brightness\n", PROGRAM_NAME);
    printf("  %s --color orange          # Whole keyboard orange\n", PROGRAM_NAME);
    printf("  %s --color 0,128,255       # Whole keyboard in this color\n", PROGRAM_NAME);
    printf("  %s --effect wave:7:left    # Rainbow wave from right to left\n", PROGRAM_NAME);
    printf("  %s --profile \"Gaming\"      # Activate the profile Gaming\n", PROGRAM_NAME);
}

static int connect_daemon(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot create a socket: %s\n", strerror(errno));
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", KBD_SOCKET_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        int e = errno;
        if (e == ENOENT || e == ECONNREFUSED)
            fprintf(stderr, "Error: the keyboard daemon is not running (%s). Start it with: "
                            "sudo systemctl start dcc-kbd-daemon\n", KBD_SOCKET_PATH);
        else if (e == EACCES)
            fprintf(stderr, "Error: no permission for %s - add your user to the group "
                            "dudes-control-center and log in again\n", KBD_SOCKET_PATH);
        else
            fprintf(stderr, "Error: cannot connect to the keyboard daemon: %s\n", strerror(e));
        close(fd);
        return -1;
    }
    return fd;
}

/* One command, one reply: the daemon ends every reply with a NUL byte */
static int ask(int fd, const char *cmd, char *reply, size_t size) {
    char line[4096];
    int n = snprintf(line, sizeof line, "%s\n", cmd);
    if (n < 0 || (size_t)n >= sizeof line) {
        fprintf(stderr, "Error: command too long\n");
        return -1;
    }
    if (send(fd, line, (size_t)n, MSG_NOSIGNAL) != n) {
        fprintf(stderr, "Error: cannot send to the keyboard daemon: %s\n", strerror(errno));
        return -1;
    }
    size_t got = 0;
    while (got < size - 1) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, 3000);
        if (r <= 0) {
            fprintf(stderr, "Error: no answer from the keyboard daemon\n");
            return -1;
        }
        ssize_t k = read(fd, reply + got, size - 1 - got);
        if (k <= 0) {
            fprintf(stderr, "Error: the keyboard daemon closed the connection\n");
            return -1;
        }
        got += (size_t)k;
        if (memchr(reply, '\0', got)) break;
    }
    reply[got] = '\0';
    if (!strncmp(reply, "ERROR", 5)) {
        fprintf(stderr, "Error: %s", reply + (reply[5] == ' ' ? 6 : 5));
        if (got && reply[strlen(reply) - 1] != '\n') fputc('\n', stderr);
        return -1;
    }
    return 0;
}

static const char *field(const char *reply, const char *key, char *out, size_t size) {
    const char *p = strstr(reply, key);
    if (!p) return NULL;
    p += strlen(key);
    size_t n = strcspn(p, "\n");
    if (n >= size) n = size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

/* "R,G,B", "#RRGGBB" or a color name -> "R,G,B" for the daemon */
static int parse_color(const char *in, char *out, size_t size) {
    static const struct { const char *name; int r, g, b; } names[] = {
        { "white", 255, 255, 255 }, { "red", 255, 0, 0 },     { "green", 0, 255, 0 },
        { "blue", 0, 0, 255 },      { "yellow", 255, 255, 0 }, { "cyan", 0, 255, 255 },
        { "magenta", 255, 0, 255 }, { "orange", 255, 165, 0 }, { "purple", 128, 0, 128 },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcasecmp(in, names[i].name)) {
            snprintf(out, size, "%d,%d,%d", names[i].r, names[i].g, names[i].b);
            return 0;
        }
    unsigned r, g, b;
    char tail;
    if (in[0] == '#' && strlen(in) == 7 && sscanf(in + 1, "%2x%2x%2x%c", &r, &g, &b, &tail) == 3) {
        snprintf(out, size, "%u,%u,%u", r, g, b);
        return 0;
    }
    int ri, gi, bi;
    if (sscanf(in, "%d,%d,%d%c", &ri, &gi, &bi, &tail) == 3 &&
        ri >= 0 && ri <= 255 && gi >= 0 && gi <= 255 && bi >= 0 && bi <= 255) {
        snprintf(out, size, "%d,%d,%d", ri, gi, bi);
        return 0;
    }
    return -1;
}

static int valid_name(const char *name) {
    return name && *name && !strstr(name, "::") && !strpbrk(name, "\n\r");
}

/* Name of the active profile from LIST_PROFILES ("PROFILE i [*] Name") */
static int active_profile(int fd, char *out, size_t size) {
    static char reply[REPLY_MAX];
    if (ask(fd, "LIST_PROFILES", reply, sizeof reply) != 0) return -1;
    const char *a = strstr(reply, "[*] ");
    if (!a) return -1;
    a += 4;
    size_t n = strcspn(a, "\n");
    if (n >= size) n = size - 1;
    memcpy(out, a, n);
    out[n] = '\0';
    return 0;
}

static int cmd_status(int fd) {
    static char reply[REPLY_MAX];
    if (ask(fd, "STATUS", reply, sizeof reply) != 0) return EXIT_FAILURE;
    char on[8] = "", leds[16] = "", br[16] = "", brmax[16] = "", prof[128] = "", fx[8] = "";
    field(reply, "IS_ON=", on, sizeof on);
    field(reply, "EFFECTS=", fx, sizeof fx);
    field(reply, "LED_COUNT=", leds, sizeof leds);
    field(reply, "BRIGHTNESS=", br, sizeof br);
    field(reply, "BRIGHTNESS_MAX=", brmax, sizeof brmax);
    char kind[16] = "", drv[64] = "";
    field(reply, "KBD_TYPE=", kind, sizeof kind);
    field(reply, "KBD_DRIVER=", drv, sizeof drv);
    printf("=== Dudes Control Center - Keyboard Backlight ===\n");
    if (kind[0]) {
        const char *k = !strcmp(kind, "per-key") ? "per-key RGB"
                      : !strcmp(kind, "rgb") ? (strcmp(leds, "1") ? "RGB zones" : "RGB, one color")
                      : !strcmp(kind, "white") ? "single color (brightness only)"
                      : "none found";
        if (drv[0]) printf("Keyboard:       %s (%s driver)\n", k, drv);
        else printf("Keyboard:       %s\n", k);
        if (!strcmp(kind, "none")) return EXIT_SUCCESS;
    }
    printf("Backlight:      %s\n", !strcmp(on, "1") ? "on" : "off");
    if (br[0] && strcmp(br, "-1")) printf("Brightness:     %s / %s\n", br, brmax[0] ? brmax : "?");
    else printf("Brightness:     unknown\n");
    printf("Active profile: %s\n", active_profile(fd, prof, sizeof prof) == 0 ? prof : "none");
    printf("LEDs:           %s\n", leds[0] ? leds : "?");
    if (fx[0]) printf("Effects:        %s\n", !strcmp(fx, "1") ? "running" : "none");
    return EXIT_SUCCESS;
}

static int cmd_list_profiles(int fd) {
    static char reply[REPLY_MAX];
    if (ask(fd, "LIST_PROFILES", reply, sizeof reply) != 0) return EXIT_FAILURE;
    int count = 0;
    for (char *line = strtok(reply, "\n"); line; line = strtok(NULL, "\n")) {
        int idx;
        if (sscanf(line, "PROFILE %d [", &idx) != 1) continue;
        const char *mark = strchr(line, '[');
        const char *name = strstr(line, "] ");
        if (!mark || !name) continue;
        printf("%c %s\n", mark[1] == '*' ? '*' : ' ', name + 2);
        count++;
    }
    if (!count) printf("No keyboard profiles yet (create them in the GUI's Keyboard Backlight tab)\n");
    return EXIT_SUCCESS;
}

static int cmd_show_profile(int fd, const char *name) {
    if (!valid_name(name)) {
        fprintf(stderr, "Error: invalid profile name\n");
        return EXIT_FAILURE;
    }
    static char reply[REPLY_MAX];
    char cmd[512];
    snprintf(cmd, sizeof cmd, "GET_PROFILE::%s", name);
    if (ask(fd, cmd, reply, sizeof reply) != 0) return EXIT_FAILURE;
    printf("Profile: %s\n", name);
    int base = 0, zones = 0;
    for (char *line = strtok(reply, "\n"); line; line = strtok(NULL, "\n")) {
        unsigned r, g, b;
        char fx[48] = "";
        const char *ef = strstr(line, " effect=");
        if (ef) snprintf(fx, sizeof fx, ", effect %.*s", (int)strcspn(ef + 8, " "), ef + 8);
        if (sscanf(line, "BASE color=%u,%u,%u", &r, &g, &b) == 3) {
            printf("Base color: %u,%u,%u (#%02X%02X%02X)%s - keys in no zone\n", r, g, b, r, g, b, fx);
            base = 1;
        } else if (!strncmp(line, "ZONE ", 5)) {
            const char *nm = strstr(line, " name="), *c = nm ? strstr(nm, " color=") : NULL;
            const char *k = c ? strstr(c, " keys=") : NULL;
            int n = 0;
            if (nm && c && k && sscanf(c + 7, "%u,%u,%u", &r, &g, &b) == 3 && sscanf(k + 6, "%d", &n) == 1) {
                if (!zones) printf("Zones:\n");
                printf("  %.*s: %u,%u,%u (#%02X%02X%02X)%s, %d LED%s\n", (int)(c - (nm + 6)), nm + 6,
                       r, g, b, r, g, b, fx, n, n == 1 ? "" : "s");
                zones++;
            }
        }
    }
    if (!base) printf("Base color: none (keys in no zone are off)\n");
    if (!zones) printf("Zones: none\n");
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        print_usage();
        return EXIT_SUCCESS;
    }
    /* check the arguments before connecting */
    const char *opt = argc > 1 ? argv[1] : NULL;
    const char *val = argc > 2 ? argv[2] : NULL;
    int needs_val = opt && (!strcmp(opt, "--brightness") || !strcmp(opt, "--color") || !strcmp(opt, "--effect") ||
                            !strcmp(opt, "--profile") || !strcmp(opt, "--show-profile"));
    int known = !opt || needs_val || !strcmp(opt, "--on") || !strcmp(opt, "--off") || !strcmp(opt, "--list-profiles");
    if (!known) {
        fprintf(stderr, "Error: unknown option '%s'\n\n", opt);
        print_usage();
        return EXIT_FAILURE;
    }
    if (needs_val && !val) {
        fprintf(stderr, "Error: %s needs a value (see --help)\n", opt);
        return EXIT_FAILURE;
    }
    if (argc > (needs_val ? 3 : 2)) {
        fprintf(stderr, "Error: one option at a time (see --help)\n");
        return EXIT_FAILURE;
    }
    char color[32] = "";
    if (opt && !strcmp(opt, "--color") && parse_color(val, color, sizeof color) != 0) {
        fprintf(stderr, "Error: invalid color '%s' - use R,G,B (0-255), #RRGGBB or a color name (see --help)\n", val);
        return EXIT_FAILURE;
    }
    if (opt && !strcmp(opt, "--effect")) {
        /* the daemon checks the names; here only that it stays one argument */
        int ok = val[0] && !strstr(val, "::") && strlen(val) < 40;
        for (const char *q = val; *q && ok; q++) ok = isalnum((unsigned char)*q) || *q == ':';
        if (!ok) {
            fprintf(stderr, "Error: invalid effect '%s' (see --help)\n", val);
            return EXIT_FAILURE;
        }
    }
    char *end = NULL;
    long level = 0;
    if (opt && !strcmp(opt, "--brightness")) {
        level = strtol(val, &end, 10);
        if (end == val || *end || level < 0) {
            fprintf(stderr, "Error: invalid brightness '%s' - use a number from 0 to the maximum\n", val);
            return EXIT_FAILURE;
        }
    }
    if (opt && (!strcmp(opt, "--profile") || !strcmp(opt, "--show-profile")) && !valid_name(val)) {
        fprintf(stderr, "Error: invalid profile name\n");
        return EXIT_FAILURE;
    }

    int fd = connect_daemon();
    if (fd < 0) return EXIT_FAILURE;
    int rc = EXIT_SUCCESS;
    static char reply[REPLY_MAX];
    char cmd[512];

    /* colors, effects and profiles need a color keyboard (1.44.0) */
    if (opt && (!strcmp(opt, "--color") || !strcmp(opt, "--effect") || !strcmp(opt, "--profile"))) {
        char kind[16] = "";
        if (ask(fd, "STATUS", reply, sizeof reply) == 0) field(reply, "KBD_TYPE=", kind, sizeof kind);
        if (!strcmp(kind, "white") || !strcmp(kind, "none")) {
            fprintf(stderr, "Error: %s\n", !strcmp(kind, "white")
                    ? "this keyboard backlight has a single color - use --on, --off or --brightness"
                    : "no keyboard backlight found");
            close(fd);
            return EXIT_FAILURE;
        }
    }

    if (!opt) {
        rc = cmd_status(fd);
    } else if (!strcmp(opt, "--on")) {
        rc = ask(fd, "TURN_ON", reply, sizeof reply) == 0 ? (printf("Backlight on\n"), EXIT_SUCCESS) : EXIT_FAILURE;
    } else if (!strcmp(opt, "--off")) {
        rc = ask(fd, "TURN_OFF", reply, sizeof reply) == 0 ? (printf("Backlight off\n"), EXIT_SUCCESS) : EXIT_FAILURE;
    } else if (!strcmp(opt, "--brightness")) {
        snprintf(cmd, sizeof cmd, "SET_BRIGHTNESS %ld", level);
        if (ask(fd, cmd, reply, sizeof reply) == 0) {
            char st[REPLY_MAX];
            char br[16] = "", mx[16] = "";
            if (ask(fd, "STATUS", st, sizeof st) == 0) {
                field(st, "BRIGHTNESS=", br, sizeof br);
                field(st, "BRIGHTNESS_MAX=", mx, sizeof mx);
            }
            printf("Brightness %s / %s\n", br[0] ? br : "?", mx[0] ? mx : "?");
        } else {
            rc = EXIT_FAILURE;
        }
    } else if (!strcmp(opt, "--color")) {
        snprintf(cmd, sizeof cmd, "SET_KEYS_COLOR::%s::all", color);
        rc = ask(fd, cmd, reply, sizeof reply) == 0
             ? (printf("Whole keyboard set to %s (not saved; activating a profile brings its colors back)\n", color),
                EXIT_SUCCESS)
             : EXIT_FAILURE;
    } else if (!strcmp(opt, "--effect")) {
        snprintf(cmd, sizeof cmd, "SET_EFFECT::%s", val);
        if (ask(fd, cmd, reply, sizeof reply) == 0) {
            const char *name = strncmp(reply, "OK Effect ", 10) == 0 ? reply + 10 : val;
            printf("Whole keyboard: effect %.*s (not saved; activating a profile brings its colors back)\n",
                   (int)strcspn(name, "\n"), name);
        } else {
            rc = EXIT_FAILURE;
        }
    } else if (!strcmp(opt, "--list-profiles")) {
        rc = cmd_list_profiles(fd);
    } else if (!strcmp(opt, "--profile")) {
        snprintf(cmd, sizeof cmd, "LOAD_PROFILE::%s", val);
        rc = ask(fd, cmd, reply, sizeof reply) == 0 ? (printf("Profile '%s' activated\n", val), EXIT_SUCCESS)
                                                    : EXIT_FAILURE;
    } else if (!strcmp(opt, "--show-profile")) {
        rc = cmd_show_profile(fd, val);
    }
    close(fd);
    return rc;
}
