/*
 * dudes-control-center-kbd-backlight-daemon.c
 * Keyboard Backlight Control Daemon for Dudes Control Center
 * Runs as a systemd service and provides IPC via Unix socket
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#include "dudes-control-center-kbd-common.h"
#include "dudes-control-center-log.h"

#define PROGRAM_NAME "dudes-control-center-kbd-backlight-daemon"

/* Global state */
static volatile sig_atomic_t g_running = 1;
static int server_fd = -1;
static int g_client_fd = -1;

/* Signal handlers */
static void on_sigint(int sig) {
    (void)sig;
    LOG_INFO("[KBD_DAEMON] Received SIGINT, shutting down...");
    g_running = 0;
}

static void on_sighup(int sig) {
    (void)sig;
    LOG_INFO("[KBD_DAEMON] Received SIGHUP");
}

/* Parse command from client */
static int handle_command(const char *cmd, char *response, size_t resp_size) {
    char cmd_copy[KBD_MAX_CMD_LEN];
    strncpy(cmd_copy, cmd, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';

    /* Remove trailing newline */
    size_t len = strlen(cmd_copy);
    if (len > 0 && cmd_copy[len - 1] == '\n') {
        cmd_copy[len - 1] = '\0';
    }

    /* Parse command using :: as delimiter to support zone names with spaces */
    char action[32] = "";
    char arg1[256] = "";
    char arg2[256] = "";
    char arg3[64] = "";
    char arg4[64] = "";

    char *cmd_dup = strdup(cmd_copy);
    char *pos = cmd_dup;

    char *sep = strstr(pos, "::");
    if (sep) {
        /* Command uses :: delimiters */
        size_t len = sep - pos;
        strncpy(action, pos, len < sizeof(action)-1 ? len : sizeof(action)-1);
        action[sizeof(action)-1] = '\0';
        pos = sep + 2;

        sep = strstr(pos, "::");
        if (sep) {
            len = sep - pos;
            strncpy(arg1, pos, len < sizeof(arg1)-1 ? len : sizeof(arg1)-1);
            arg1[sizeof(arg1)-1] = '\0';
            pos = sep + 2;
        } else {
            if (*pos != '\0') {
                strncpy(arg1, pos, sizeof(arg1)-1);
                arg1[sizeof(arg1)-1] = '\0';
            }
        }

        sep = strstr(pos, "::");
        if (sep) {
            len = sep - pos;
            strncpy(arg2, pos, len < sizeof(arg2)-1 ? len : sizeof(arg2)-1);
            arg2[sizeof(arg2)-1] = '\0';
            pos = sep + 2;
        } else {
            if (*pos != '\0') {
                strncpy(arg2, pos, sizeof(arg2)-1);
                arg2[sizeof(arg2)-1] = '\0';
            }
        }

        if (*pos != '\0') {
            strncpy(arg3, pos, sizeof(arg3)-1);
            arg3[sizeof(arg3)-1] = '\0';
        }
    } else {
        /* Space-separated args */
        sep = strchr(pos, ' ');
        if (sep) {
            size_t len = sep - pos;
            strncpy(action, pos, len < sizeof(action)-1 ? len : sizeof(action)-1);
            action[sizeof(action)-1] = '\0';
            pos = sep + 1;

            sep = strchr(pos, ' ');
            if (sep) {
                len = sep - pos;
                strncpy(arg1, pos, len < sizeof(arg1)-1 ? len : sizeof(arg1)-1);
                arg1[sizeof(arg1)-1] = '\0';
                pos = sep + 1;

                sep = strchr(pos, ' ');
                if (sep) {
                    len = sep - pos;
                    strncpy(arg2, pos, len < sizeof(arg2)-1 ? len : sizeof(arg2)-1);
                    arg2[sizeof(arg2)-1] = '\0';
                    pos = sep + 1;

                    sep = strchr(pos, ' ');
                    if (sep) {
                        len = sep - pos;
                        strncpy(arg3, pos, len < sizeof(arg3)-1 ? len : sizeof(arg3)-1);
                        arg3[sizeof(arg3)-1] = '\0';
                        pos = sep + 1;

                        if (*pos != '\0') {
                            strncpy(arg4, pos, sizeof(arg4)-1);
                            arg4[sizeof(arg4)-1] = '\0';
                        }
                    } else {
                        if (*pos != '\0') {
                            strncpy(arg3, pos, sizeof(arg3)-1);
                            arg3[sizeof(arg3)-1] = '\0';
                        }
                    }
                } else {
                    if (*pos != '\0') {
                        strncpy(arg2, pos, sizeof(arg2)-1);
                        arg2[sizeof(arg2)-1] = '\0';
                    }
                }
            } else {
                if (*pos != '\0') {
                    strncpy(arg1, pos, sizeof(arg1)-1);
                    arg1[sizeof(arg1)-1] = '\0';
                }
            }
        } else {
            strncpy(action, pos, sizeof(action)-1);
            action[sizeof(action)-1] = '\0';
        }
    }

    free(cmd_dup);

    LOG_DEBUG("[KBD_DAEMON] Received command: '%s' args='%s' '%s' '%s' '%s'",
              action, arg1, arg2, arg3, arg4);

    const KbdLedState *state = kbd_led_get_state();

    /* Command: STATUS */
    if (strcmp(action, "STATUS") == 0) {
        int led_count = get_led_count();
        snprintf(response, resp_size,
            "STATUS OK\n"
            "LED_COUNT=%d\n"
            "GLOBAL_R=%u\n"
            "GLOBAL_G=%u\n"
            "GLOBAL_B=%u\n"
            "MODE=%d\n"
            "IS_ON=%d\n",
            led_count,
            state->global_r,
            state->global_g,
            state->global_b,
            state->mode,
            state->is_on);
        return 0;
    }

    /* Command: SET_COLOR <R> <G> <B> */
    if (strcmp(action, "SET_COLOR") == 0) {
        uint8_t r = (uint8_t)atoi(arg1);
        uint8_t g = (uint8_t)atoi(arg2);
        uint8_t b = (uint8_t)atoi(arg3);

        LOG_INFO("[KBD_DAEMON] SET_COLOR: RGB(%u, %u, %u)", r, g, b);

        if (set_all_leds_color(r, g, b) == 0) {
            kbd_led_set_power((r != 0 || g != 0 || b != 0));
            kbd_led_set_mode(KBD_LED_MODE_STATIC);
            snprintf(response, resp_size, "OK Color set to %u,%u,%u\n", r, g, b);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to set color\n");
            return -1;
        }
    }

    /* Command: SET_ALL_LEDS <R> <G> <B> */
    if (strcmp(action, "SET_ALL_LEDS") == 0) {
        uint8_t r = (uint8_t)atoi(arg1);
        uint8_t g = (uint8_t)atoi(arg2);
        uint8_t b = (uint8_t)atoi(arg3);

        LOG_INFO("[KBD_DAEMON] SET_ALL_LEDS: RGB(%u, %u, %u)", r, g, b);

        if (set_all_leds_with_brightness(r, g, b) == 0) {
            kbd_led_set_power((r != 0 || g != 0 || b != 0));
            kbd_led_set_mode(KBD_LED_MODE_STATIC);
            snprintf(response, resp_size, "OK All LEDs set to %u,%u,%u\n", r, g, b);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to set LED colors\n");
            return -1;
        }
    }

    /* Command: SET_ALL_ZONES_COLOR <R> <G> <B> */
    if (strcmp(action, "SET_ALL_ZONES_COLOR") == 0) {
        uint8_t r = (uint8_t)atoi(arg1);
        uint8_t g = (uint8_t)atoi(arg2);
        uint8_t b = (uint8_t)atoi(arg3);

        LOG_INFO("[KBD_DAEMON] SET_ALL_ZONES_COLOR: RGB(%u, %u, %u)", r, g, b);

        if (set_all_zones_color(r, g, b) == 0) {
            kbd_led_set_power((r != 0 || g != 0 || b != 0));
            kbd_led_set_mode(KBD_LED_MODE_STATIC);
            snprintf(response, resp_size, "OK All zones set to %u,%u,%u\n", r, g, b);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to set zone colors\n");
            return -1;
        }
    }

    /* Command: SET_COLOR_RGB <R,G,B or #RRGGBB> */
    if (strcmp(action, "SET_COLOR_RGB") == 0) {
        uint8_t r, g, b;
        if (parse_rgb(arg1, &r, &g, &b) != 0) {
            snprintf(response, resp_size, "ERROR Invalid color format: %s\n", arg1);
            return -1;
        }

        if (set_all_leds_color(r, g, b) == 0) {
            kbd_led_set_power((r != 0 || g != 0 || b != 0));
            kbd_led_set_mode(KBD_LED_MODE_STATIC);
            snprintf(response, resp_size, "OK Color set to %s\n", arg1);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to set color\n");
            return -1;
        }
    }

    /* Command: TURN_OFF */
    if (strcmp(action, "TURN_OFF") == 0) {
        if (turn_off_all_leds() == 0) {
            kbd_led_set_power(0);
            kbd_led_set_mode(KBD_LED_MODE_OFF);
            snprintf(response, resp_size, "OK All LEDs turned off\n");
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to turn off LEDs\n");
            return -1;
        }
    }

    /* Command: LIST_LEDS */
    if (strcmp(action, "LIST_LEDS") == 0) {
        int led_count = get_led_count();
        snprintf(response, resp_size, "LED_LIST_START COUNT=%d\n", led_count);

        int offset = strlen(response);
        for (int i = 0; i < led_count && offset < KBD_MAX_RESPONSE_LEN - 128; i++) {
            int written = snprintf(response + offset, KBD_MAX_RESPONSE_LEN - offset,
                                   "LED %d %s %d\n", i, state->leds[i].name, state->leds[i].max_brightness);
            if (written < 0 || written >= KBD_MAX_RESPONSE_LEN - offset) break;
            offset += written;
        }

        strcat(response, "LED_LIST_END\n");
        return 0;
    }

    /* Command: GET_CURRENT_COLOR */
    if (strcmp(action, "GET_CURRENT_COLOR") == 0) {
        snprintf(response, resp_size, "CURRENT_COLOR R=%u G=%u B=%u\n",
            state->global_r, state->global_g, state->global_b);
        return 0;
    }

    /* Command: GET_BRIGHTNESS */
    if (strcmp(action, "GET_BRIGHTNESS") == 0) {
        uint8_t brightness = kbd_led_get_brightness();
        snprintf(response, resp_size, "BRIGHTNESS %u\n", brightness);
        return 0;
    }

    /* Command: SET_BRIGHTNESS <value> */
    if (strcmp(action, "SET_BRIGHTNESS") == 0) {
        uint8_t brightness = (uint8_t)atoi(arg1);
        LOG_INFO("[KBD_DAEMON] SET_BRIGHTNESS: %u", brightness);

        if (kbd_led_set_brightness(brightness) == 0) {
            snprintf(response, resp_size, "OK Brightness set to %u\n", brightness);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to set brightness\n");
            return -1;
        }
    }

    /* Command: PING */
    if (strcmp(action, "PING") == 0) {
        snprintf(response, resp_size, "PONG\n");
        return 0;
    }

    /* Command: TEST_LED <led_id> */
    if (strcmp(action, "TEST_LED") == 0) {
        int led_id = atoi(arg1);
        LOG_INFO("[KBD_DAEMON] TEST_LED: LED %d", led_id);

        if (!state) {
            snprintf(response, resp_size, "ERROR LED subsystem not initialized\n");
            return -1;
        }

        int led_count = get_led_count();
        if (led_id < 0 || led_id >= led_count) {
            snprintf(response, resp_size, "ERROR Invalid LED ID: %d (must be 0-%d)\n", led_id, led_count - 1);
            return -1;
        }

        if (set_led_color(state->leds[led_id].path, 0, 255, 255, state->leds[led_id].max_brightness) == 0) {
            snprintf(response, resp_size, "OK LED %d set to cyan\n", led_id);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to set LED %d\n", led_id);
            return -1;
        }
    }

    /* Command: SET_LED <led_id> <R> <G> <B> */
    if (strcmp(action, "SET_LED") == 0) {
        int led_id = atoi(arg1);
        uint8_t r = (uint8_t)atoi(arg2);
        uint8_t g = (uint8_t)atoi(arg3);
        uint8_t b = (uint8_t)atoi(arg4);

        LOG_INFO("[KBD_DAEMON] SET_LED: LED %d RGB(%u, %u, %u)", led_id, r, g, b);

        if (!state) {
            snprintf(response, resp_size, "ERROR LED subsystem not initialized\n");
            return -1;
        }

        int led_count = get_led_count();
        if (led_id < 0 || led_id >= led_count) {
            snprintf(response, resp_size, "ERROR Invalid LED ID: %d (must be 0-%d)\n", led_id, led_count - 1);
            return -1;
        }

        if (set_led_color(state->leds[led_id].path, r, g, b, state->leds[led_id].max_brightness) == 0) {
            snprintf(response, resp_size, "OK LED %d set to RGB(%u,%u,%u)\n", led_id, r, g, b);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to set LED %d\n", led_id);
            return -1;
        }
    }

    /* Command: CLEAR_LEDS */
    if (strcmp(action, "CLEAR_LEDS") == 0) {
        LOG_INFO("[KBD_DAEMON] CLEAR_LEDS");
        if (set_all_leds_color(0, 0, 0) == 0) {
            snprintf(response, resp_size, "OK All LEDs turned off\n");
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to clear LEDs\n");
            return -1;
        }
    }

    /* === ZONE COMMANDS === */

    /* Command: CREATE_ZONE <name> <key_indices_csv> <R,G,B> [is_preset] */
    if (strcmp(action, "CREATE_ZONE") == 0) {
        int key_indices[256];
        int num_keys = 0;
        int is_preset = 0;

        if (arg4[0] != '\0') {
            is_preset = atoi(arg4);
        }

        char *key_copy = strdup(arg2);
        char *token = strtok(key_copy, ",");
        while (token && num_keys < 256) {
            key_indices[num_keys++] = atoi(token);
            token = strtok(NULL, ",");
        }
        free(key_copy);

        LOG_INFO("[KBD_DAEMON] CREATE_ZONE: '%s' with %d keys (is_preset=%d)", arg1, num_keys, is_preset);

        if (num_keys == 0) {
            snprintf(response, resp_size, "ERROR No keys specified for zone\n");
            return -1;
        }

        uint8_t r, g, b;
        if (parse_rgb(arg3, &r, &g, &b) != 0) {
            snprintf(response, resp_size, "ERROR Invalid color format: %s\n", arg3);
            return -1;
        }

        if (zone_create(arg1, key_indices, num_keys, r, g, b, is_preset) == 0) {
            snprintf(response, resp_size, "OK Zone created: %s\n", arg1);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to create zone\n");
            return -1;
        }
    }

    /* Command: DELETE_ZONE <name> */
    if (strcmp(action, "DELETE_ZONE") == 0) {
        if (zone_delete(arg1) == 0) {
            snprintf(response, resp_size, "OK Zone deleted: %s\n", arg1);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Zone not found: %s\n", arg1);
            return -1;
        }
    }

    /* Command: RENAME_ZONE <old_name> <new_name> */
    if (strcmp(action, "RENAME_ZONE") == 0) {
        if (arg2[0] == '\0') {
            snprintf(response, resp_size, "ERROR New name required\n");
            return -1;
        }
        LOG_INFO("[KBD_DAEMON] RENAME_ZONE: '%s' -> '%s'", arg1, arg2);
        if (zone_rename(arg1, arg2) == 0) {
            snprintf(response, resp_size, "OK Zone renamed: %s -> %s\n", arg1, arg2);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to rename zone\n");
            return -1;
        }
    }

    /* Command: SET_ZONE_COLOR <name> <R,G,B> */
    if (strcmp(action, "SET_ZONE_COLOR") == 0) {
        uint8_t r, g, b;
        if (parse_rgb(arg3, &r, &g, &b) != 0) {
            snprintf(response, resp_size, "ERROR Invalid color format: %s\n", arg3);
            return -1;
        }

        if (zone_set_color(arg1, r, g, b) == 0) {
            snprintf(response, resp_size, "OK Zone color set: %s -> RGB(%d,%d,%d)\n", arg1, r, g, b);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Zone not found: %s\n", arg1);
            return -1;
        }
    }

    /* Command: ADD_KEY_TO_ZONE <zone_name> <key_index> */
    if (strcmp(action, "ADD_KEY_TO_ZONE") == 0) {
        int key_idx = atoi(arg2);
        if (zone_add_key(arg1, key_idx) == 0) {
            snprintf(response, resp_size, "OK Key %d added to zone: %s\n", key_idx, arg1);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to add key to zone\n");
            return -1;
        }
    }

    /* Command: REMOVE_KEY_FROM_ZONE <zone_name> <key_index> */
    if (strcmp(action, "REMOVE_KEY_FROM_ZONE") == 0) {
        int key_idx = atoi(arg2);
        if (zone_remove_key(arg1, key_idx) == 0) {
            snprintf(response, resp_size, "OK Key %d removed from zone: %s\n", key_idx, arg1);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to remove key from zone\n");
            return -1;
        }
    }

    /* Command: LIST_ZONES */
    if (strcmp(action, "LIST_ZONES") == 0) {
        char zones_buf[KBD_MAX_RESPONSE_LEN];
        zone_list(zones_buf, sizeof(zones_buf));
        snprintf(response, resp_size, "%s", zones_buf);
        return 0;
    }

    /* Command: CLEAR_ZONES */
    if (strcmp(action, "CLEAR_ZONES") == 0) {
        zone_clear_all();
        snprintf(response, resp_size, "OK All zones cleared\n");
        return 0;
    }

    /* === PROFILE COMMANDS === */

    /* Command: SAVE_PROFILE <name> */
    if (strcmp(action, "SAVE_PROFILE") == 0) {
        LOG_INFO("[KBD_DAEMON] SAVE_PROFILE: '%s'", arg1);
        if (kbd_profile_save_json(arg1) == 0) {
            snprintf(response, resp_size, "OK Profile saved: %s\n", arg1);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to save profile\n");
            return -1;
        }
    }

    /* Command: LOAD_PROFILE <name> */
    if (strcmp(action, "LOAD_PROFILE") == 0) {
        LOG_INFO("[KBD_DAEMON] LOAD_PROFILE: '%s'", arg1);
        if (profile_apply(arg1) == 0) {
            snprintf(response, resp_size, "OK Profile loaded: %s\n", arg1);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Profile not found: %s\n", arg1);
            return -1;
        }
    }

    /* Command: DELETE_PROFILE <name> */
    if (strcmp(action, "DELETE_PROFILE") == 0) {
        LOG_INFO("[KBD_DAEMON] DELETE_PROFILE: '%s'", arg1);
        if (kbd_profile_delete_json(arg1) == 0) {
            snprintf(response, resp_size, "OK Profile deleted: %s\n", arg1);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Profile not found: %s\n", arg1);
            return -1;
        }
    }

    /* Command: RENAME_PROFILE <old_name> <new_name> */
    if (strcmp(action, "RENAME_PROFILE") == 0) {
        LOG_INFO("[KBD_DAEMON] RENAME_PROFILE: '%s' -> '%s'", arg1, arg2);
        if (kbd_profile_rename_json(arg1, arg2) == 0) {
            snprintf(response, resp_size, "OK Profile renamed: %s to %s\n", arg1, arg2);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Profile not found or duplicate name: %s\n", arg1);
            return -1;
        }
    }

    /* Command: CREATE_PROFILE <name> */
    if (strcmp(action, "CREATE_PROFILE") == 0) {
        LOG_INFO("[KBD_DAEMON] CREATE_PROFILE: '%s'", arg1);
        if (kbd_profile_create_json(arg1) == 0) {
            snprintf(response, resp_size, "OK Profile created: %s\n", arg1);
            return 0;
        } else {
            snprintf(response, resp_size, "ERROR Failed to create profile\n");
            return -1;
        }
    }

    /* Command: LIST_PROFILES */
    if (strcmp(action, "LIST_PROFILES") == 0) {
        char profiles_buf[KBD_MAX_RESPONSE_LEN];
        kbd_profile_list_json(profiles_buf, sizeof(profiles_buf));
        snprintf(response, resp_size, "%s", profiles_buf);
        return 0;
    }

    /* Command: GET_ZONES */
    if (strcmp(action, "GET_ZONES") == 0) {
        char zones_buf[KBD_MAX_RESPONSE_LEN];
        zone_list(zones_buf, sizeof(zones_buf));
        snprintf(response, resp_size, "%s", zones_buf);
        return 0;
    }

    /* Command: APPLY_ZONES */
    if (strcmp(action, "APPLY_ZONES") == 0) {
        apply_zones();
        snprintf(response, resp_size, "OK Zones applied\n");
        return 0;
    }

    /* Unknown command */
    snprintf(response, resp_size, "ERROR Unknown command: %s\n", action);
    return -1;
}

/* Handle new client connection (non-blocking, persistent) */
static void handle_client_nonblocking(int client_fd) {
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    /* Close previous client if any */
    if (g_client_fd >= 0) {
        close(g_client_fd);
    }

    g_client_fd = client_fd;
    LOG_INFO("[KBD_DAEMON] Client connected (non-blocking)");
}

/* Process pending client data (call from main loop) */
static void process_client(void) {
    if (g_client_fd < 0) return;

    char buffer[KBD_BUFFER_SIZE] = {0};
    char response[KBD_MAX_RESPONSE_LEN] = {0};

    memset(buffer, 0, sizeof(buffer));
    ssize_t bytes_read = read(g_client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read == 0) {
        LOG_INFO("[KBD_DAEMON] Client disconnected");
        close(g_client_fd);
        g_client_fd = -1;
        return;
    } else if (bytes_read < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("[KBD_DAEMON] Client read error: %s", strerror(errno));
            close(g_client_fd);
            g_client_fd = -1;
        }
        return; /* No data available, that's OK */
    }

    buffer[bytes_read] = '\0';

    /* Process each line as a separate command */
    char *line = strtok(buffer, "\n");
    while (line != NULL && strlen(line) > 0) {
        LOG_DEBUG("[KBD_DAEMON] Received command: '%s'", line);

        response[0] = '\0';
        handle_command(line, response, sizeof(response));

        LOG_DEBUG("[KBD_DAEMON] Sending response: '%s'", response);

        size_t total_len = strlen(response);
        size_t total_sent = 0;
        while (total_sent < total_len) {
            ssize_t bytes_written = write(g_client_fd, response + total_sent, total_len - total_sent);
            if (bytes_written < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(1000);
                    continue;
                }
                LOG_ERROR("[KBD_DAEMON] Error writing to client: %s", strerror(errno));
                close(g_client_fd);
                g_client_fd = -1;
                return;
            }
            total_sent += bytes_written;
        }

        line = strtok(NULL, "\n");
    }
}

/* Initialize Unix socket */
static int init_socket(void) {
    struct sockaddr_un addr;
    int sock_fd;

    unlink(KBD_SOCKET_PATH);

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LOG_ERROR("[KBD_DAEMON] Failed to create socket: %s", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, KBD_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("[KBD_DAEMON] Failed to bind socket: %s", strerror(errno));
        close(sock_fd);
        return -1;
    }

    chmod(KBD_SOCKET_PATH, 0777);

    if (listen(sock_fd, SOMAXCONN) < 0) {
        LOG_ERROR("[KBD_DAEMON] Failed to listen on socket: %s", strerror(errno));
        close(sock_fd);
        return -1;
    }

    LOG_INFO("[KBD_DAEMON] Socket created at %s", KBD_SOCKET_PATH);
    return sock_fd;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Initialize logging */
    if (log_init("KBD_BACKLIGHT_DAEMON", LOG_LEVEL_INFO, "/var/log/dudes-control-center") != 0) {
        fprintf(stderr, "[KBD_DAEMON] Failed to initialize logging\n");
        return 1;
    }

    LOG_INFO("[KBD_DAEMON] Starting %s...", PROGRAM_NAME);

    /* Initialize LED subsystem */
    if (kbd_led_init() != 0) {
        LOG_ERROR("[KBD_DAEMON] Failed to initialize LED subsystem");
        return 1;
    }

    /* Load saved profiles from profiles.json */
    LOG_INFO("[KBD_DAEMON] Loading saved profiles...");
    kbd_profiles_load_json();
    LOG_INFO("[KBD_DAEMON] Loaded %d keyboard profiles", g_num_profiles);

    /* Set up signal handlers */
    signal(SIGINT, on_sigint);
    signal(SIGHUP, on_sighup);
    signal(SIGTERM, on_sigint);

    /* Initialize socket */
    server_fd = init_socket();
    if (server_fd < 0) {
        LOG_ERROR("[KBD_DAEMON] Failed to initialize socket");
        kbd_led_cleanup();
        return 1;
    }

    LOG_INFO("[KBD_DAEMON] Daemon started successfully");

    /* Main loop */
    while (g_running) {
        fd_set read_fds;
        struct timeval timeout;
        int max_fd = server_fd;

        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);

        /* Also monitor client socket for incoming data */
        if (g_client_fd >= 0) {
            FD_SET(g_client_fd, &read_fds);
            if (g_client_fd > max_fd)
                max_fd = g_client_fd;
        }

        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (activity < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("[KBD_DAEMON] select() failed: %s", strerror(errno));
            break;
        }

        if (FD_ISSET(server_fd, &read_fds)) {
            struct sockaddr_un client_addr;
            socklen_t client_len = sizeof(client_addr);

            int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                if (errno == EINTR || errno == EWOULDBLOCK) continue;
                LOG_ERROR("[KBD_DAEMON] accept() failed: %s", strerror(errno));
                continue;
            }

            handle_client_nonblocking(client_fd);
        }

        /* Process client data if there's activity on the client socket */
        if (g_client_fd >= 0 && FD_ISSET(g_client_fd, &read_fds)) {
            process_client();
        }
    }

    /* Cleanup */
    LOG_INFO("[KBD_DAEMON] Shutting down...");

    if (server_fd >= 0) {
        close(server_fd);
        unlink(KBD_SOCKET_PATH);
    }

    if (g_client_fd >= 0) {
        close(g_client_fd);
        g_client_fd = -1;
    }

    kbd_led_cleanup();

    LOG_INFO("[KBD_DAEMON] Daemon stopped");
    log_cleanup();
    return 0;
}
