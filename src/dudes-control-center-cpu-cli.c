/*
 * dudes-control-center-cpu-cli.c
 * Command-line interface for controlling CPU frequency profiles
 *
 * Usage:
 *   ./dudes-control-center-cpu-cli                    - Show CPU status (default)
 *   ./dudes-control-center-cpu-cli --list-profiles    - List custom CPU profiles
 *   ./dudes-control-center-cpu-cli --apply-profile 0  - Apply custom profile by index
 *   ./dudes-control-center-cpu-cli --governor perf    - Set governor
 *   ./dudes-control-center-cpu-cli --hard-limit 3.8   - Set hard limit (GHz)
 *   ./dudes-control-center-cpu-cli --hard-limit-get   - Get hard limit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>

#define CPU_SOCKET_PATH "/run/dudes-control-center-cpu.sock"
#define CPU_BUFFER_SIZE 4096
#define PROGRAM_NAME "dudes-control-center-cpu-cli"

/* --- Daemon socket communication --- */

static int connect_to_cpu_daemon(void) {
    struct stat st;
    if (stat(CPU_SOCKET_PATH, &st) != 0) {
        fprintf(stderr, "Error: CPU daemon socket not found at %s. Is the daemon running?\n", CPU_SOCKET_PATH);
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to create socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CPU_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: Failed to connect to CPU daemon: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static const char *send_command(int fd, const char *cmd, char *response, size_t resp_size) {
    if (fd < 0) return NULL;

    /* Send command */
    ssize_t sent = write(fd, cmd, strlen(cmd));
    if (sent < 0) {
        fprintf(stderr, "Error: Failed to send command: %s\n", strerror(errno));
        return NULL;
    }

    /* Read response */
    ssize_t received = read(fd, response, resp_size - 1);
    if (received < 0) {
        fprintf(stderr, "Error: Failed to read response: %s\n", strerror(errno));
        return NULL;
    }

    response[received] = '\0';
    return response;
}

/* --- Mode implementations --- */

static int run_default_mode(int fd) {
    char response[CPU_BUFFER_SIZE];
    const char *resp = send_command(fd, "STATUS", response, sizeof(response));
    if (!resp) return EXIT_FAILURE;

    /* Parse STATUS response */
    printf("=== Dudes Control Center - CPU Status ===\n");

    /* Extract key fields from the STATUS response */
    const char *tmp = strstr(resp, "NUM_CPUS=");
    if (tmp) {
        int num_cpus = atoi(tmp + 9);
        printf("CPU Cores: %d\n", num_cpus);
    }

    tmp = strstr(resp, "CPU_TEMP=");
    if (tmp) {
        int temp = atoi(tmp + 9);
        printf("CPU Temperature: %d°C\n", temp);
    }

    tmp = strstr(resp, "CPU_LOAD=");
    if (tmp) {
        double load = atof(tmp + 9);
        printf("CPU Load: %.1f%%\n", load);
    }

    tmp = strstr(resp, "ON_AC=");
    if (tmp) {
        int on_ac = atoi(tmp + 6);
        printf("Power: %s\n", on_ac ? "AC" : "Battery");
    }

    tmp = strstr(resp, "ACTIVE_GOVERNOR=");
    if (tmp) {
        printf("Active Profile: %s\n", tmp + 16);
    }

    tmp = strstr(resp, "MIN_FREQ=");
    if (tmp) {
        printf("Min Frequency: %s\n", tmp + 9);
    }

    tmp = strstr(resp, "MAX_FREQ=");
    if (tmp) {
        printf("Max Frequency: %s\n", tmp + 9);
    }

    return EXIT_SUCCESS;
}

static int run_list_profiles(int fd) {
    char response[CPU_BUFFER_SIZE];
    const char *resp = send_command(fd, "LIST_CUSTOM_PROFILES", response, sizeof(response));
    if (!resp) return EXIT_FAILURE;

    printf("=== Custom CPU Profiles ===\n");
    printf("%s", resp);
    return EXIT_SUCCESS;
}

static int run_apply_profile(int fd, int idx) {
    char cmd[64];
    char response[CPU_BUFFER_SIZE];

    snprintf(cmd, sizeof(cmd), "APPLY_CUSTOM_PROFILE|%d", idx);
    const char *resp = send_command(fd, cmd, response, sizeof(response));
    if (!resp) return EXIT_FAILURE;

    printf("%s", resp);
    return EXIT_SUCCESS;
}

static int run_set_governor(int fd, const char *governor) {
    char cmd[128];
    char response[CPU_BUFFER_SIZE];

    snprintf(cmd, sizeof(cmd), "SET_GOVERNOR|%s", governor);
    const char *resp = send_command(fd, cmd, response, sizeof(response));
    if (!resp) return EXIT_FAILURE;

    printf("%s", resp);
    return EXIT_SUCCESS;
}

static int run_set_hard_limit(int fd, double ghz) {
    uint32_t hz = (uint32_t)(ghz * 1000000000.0);
    char cmd[64];
    char response[CPU_BUFFER_SIZE];

    snprintf(cmd, sizeof(cmd), "SET_HARD_LIMIT|%u", hz);
    const char *resp = send_command(fd, cmd, response, sizeof(response));
    if (!resp) return EXIT_FAILURE;

    printf("%s", resp);
    return EXIT_SUCCESS;
}

static int run_get_hard_limit(int fd) {
    char response[CPU_BUFFER_SIZE];
    const char *resp = send_command(fd, "GET_HARD_LIMIT", response, sizeof(response));
    if (!resp) return EXIT_FAILURE;

    printf("%s", resp);
    return EXIT_SUCCESS;
}

static void print_usage(void) {
    printf("Usage: %s [OPTIONS]\n", PROGRAM_NAME);
    printf("\n");
    printf("Dudes Control Center - CLI interface for CPU frequency control\n");
    printf("\n");
    printf("Options:\n");
    printf("  (none)               Show CPU status (default)\n");
    printf("  --list-profiles      List custom CPU frequency profiles\n");
    printf("  --apply-profile <N>  Apply custom profile by index\n");
    printf("  --governor <name>    Set governor (performance, schedutil, powersave, etc.)\n");
    printf("  --hard-limit <GHz>   Set hard frequency limit in GHz (e.g. 3.8)\n");
    printf("  --hard-limit-get     Show current hard frequency limit\n");
    printf("  --help               Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                     # Show current CPU status\n", PROGRAM_NAME);
    printf("  %s --list-profiles     # List all custom profiles\n", PROGRAM_NAME);
    printf("  %s --apply-profile 0   # Apply first custom profile\n", PROGRAM_NAME);
    printf("  %s --governor perf     # Set performance governor\n", PROGRAM_NAME);
    printf("  %s --hard-limit 3.8    # Set hard limit to 3.8 GHz\n", PROGRAM_NAME);
    printf("  %s --hard-limit-get    # Show current hard limit\n", PROGRAM_NAME);
}

int main(int argc, char *argv[]) {
    /* Check for help first (no connection needed) */
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                print_usage();
                return EXIT_SUCCESS;
            }
        }
    }

    /* Connect to CPU daemon */
    int fd = connect_to_cpu_daemon();
    if (fd < 0) {
        return EXIT_FAILURE;
    }

    /* Default mode - show status */
    if (argc == 1) {
        int result = run_default_mode(fd);
        close(fd);
        return result;
    }

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list-profiles") == 0) {
            int result = run_list_profiles(fd);
            close(fd);
            return result;
        }

        if (strcmp(argv[i], "--apply-profile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --apply-profile requires an index argument\n");
                print_usage();
                close(fd);
                return EXIT_FAILURE;
            }
            int idx = atoi(argv[++i]);
            int result = run_apply_profile(fd, idx);
            close(fd);
            return result;
        }

        if (strcmp(argv[i], "--governor") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --governor requires a name argument\n");
                print_usage();
                close(fd);
                return EXIT_FAILURE;
            }
            int result = run_set_governor(fd, argv[++i]);
            close(fd);
            return result;
        }

        if (strcmp(argv[i], "--hard-limit") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --hard-limit requires a GHz value argument\n");
                print_usage();
                close(fd);
                return EXIT_FAILURE;
            }
            double ghz = atof(argv[++i]);
            if (ghz <= 0) {
                fprintf(stderr, "Error: Hard limit must be a positive GHz value\n");
                close(fd);
                return EXIT_FAILURE;
            }
            int result = run_set_hard_limit(fd, ghz);
            close(fd);
            return result;
        }

        if (strcmp(argv[i], "--hard-limit-get") == 0) {
            int result = run_get_hard_limit(fd);
            close(fd);
            return result;
        }

        fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
        print_usage();
        close(fd);
        return EXIT_FAILURE;
    }

    close(fd);
    return EXIT_SUCCESS;
}
