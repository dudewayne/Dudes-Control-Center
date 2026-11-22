// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc-fan-cli.c
 * Command-line interface for controlling notebook fans
 * 
 * Usage:
 *   ./dcc-fan-cli              - Show CPU/GPU temps and fan status (default)
 *   ./dcc-fan-cli --set 10     - Set fan speed to 10% (in 10% steps: 0,10,...,100; 0 = fans off)
 *   ./dcc-fan-cli --auto       - Enable automatic fan control based on temperature
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "dcc-common.h"

#define PROGRAM_NAME "dcc-fan-cli"

/* Auto mode target duty calculation (similar to fan-control) */
static int target_duty_from_temp(int temp_c) {
    const struct { int t; int p; } points[] = {
        {45, 20},
        {55, 30},
        {60, 40},
        {70, 50},
        {80, 70},
        {85, 100}
    };
    const int n = sizeof(points) / sizeof(points[0]);

    if (temp_c <= points[0].t)
        return points[0].p;
    if (temp_c >= points[n - 1].t)
        return points[n - 1].p;

    for (int i = 0; i < n - 1; i++) {
        int t1 = points[i].t, p1 = points[i].p;
        int t2 = points[i + 1].t, p2 = points[i + 1].p;
        if (temp_c >= t1 && temp_c < t2) {
            double fraction = (double)(temp_c - t1) / (double)(t2 - t1);
            double percentage = p1 + (p2 - p1) * fraction;
            return (int)(percentage + 0.5);
        }
    }

    return 100; /* fallback */
}

static void print_status(int cpu_temp, int gpu_temp, int cpu_fan_rpm, int gpu_fan_rpm) {
    char line[96];

    printf("=== Dudes Control Center Status ===\n");
    printf("CPU Temperature: %d°C\n", cpu_temp);
    if (gpu_temp >= 0) {
        printf("GPU Temperature: %d°C\n", gpu_temp);
    } else {
        printf("GPU Temperature: N/A (sensor not available)\n");
    }
    /* Percent = duty set in the EC; RPM = measured speed */
    int cd = get_cpu_fan_duty(), gd = get_gpu_fan_duty();
    format_fan_text(line, sizeof line, "CPU Fan", cpu_fan_rpm, fan_duty_plausible(cd, cpu_fan_rpm) ? cd : -1);
    printf("%s\n", line);
    format_fan_text(line, sizeof line, "GPU Fan", gpu_fan_rpm, fan_duty_plausible(gd, gpu_fan_rpm) ? gd : -1);
    printf("%s\n", line);
}

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static int run_auto_mode(void) {
    printf("Starting auto mode... Press Ctrl+C to stop\n");
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    
    /* Ring buffer for temperature averaging */
    int ring[3] = {-1, -1, -1};
    int idx = 0, filled = 0;
    
    /* Start at 20% duty */
    int current_duty = 20;
    set_fan_duty(current_duty);
    
    while (!g_stop) {
        int cpu_temp = get_cpu_temp();
        int gpu_temp = get_gpu_temp();
        
        /* Determine the hotter temperature */
        int hot_temp = cpu_temp;
        if (gpu_temp >= 0 && gpu_temp > cpu_temp) {
            hot_temp = gpu_temp;
        }
        
        /* Check for invalid CPU temp - safety measure */
        if (cpu_temp < TEMP_MIN_VALID_C || cpu_temp > TEMP_MAX_VALID_C) {
            printf("\nWARNING: Invalid CPU temperature (%d°C), setting fans to 100%%!\n", cpu_temp);
            set_fan_duty(100);
            current_duty = 100;
        } else {
            /* Update ring buffer */
            ring[idx] = hot_temp;
            idx = (idx + 1) % 3;
            if (filled < 3) filled++;
            
            /* Calculate average temperature */
            int sum = 0;
            for (int i = 0; i < filled; i++) {
                sum += ring[i];
            }
            int avg_temp = sum / filled;
            
            /* Calculate target duty */
            int target_duty = target_duty_from_temp(avg_temp);
            
            /* Smooth transitions - only increase if significantly different */
            int diff = target_duty - current_duty;
            if (diff >= 5) {
                current_duty += 5;
                set_fan_duty(current_duty);
            } else if (diff <= -1) {
                current_duty -= 1;
                set_fan_duty(current_duty);
            }
            
            /* Read current status */
            int cpu_fan_rpm = get_cpu_fan_rpm();
            int gpu_fan_rpm = get_gpu_fan_rpm();
            int fan_duty = get_fan_duty();
            
            /* Print status on single line */
            char gpu_str[16];
            if (gpu_temp >= 0) {
                snprintf(gpu_str, sizeof(gpu_str), "%3d", gpu_temp);
            } else {
                strcpy(gpu_str, "---");
            }
            printf("\rCPU: %3d°C  GPU: %s°C  CPU Fan: %5d RPM  GPU Fan: %5d RPM  Duty: %3d%%    ", cpu_temp, gpu_str, cpu_fan_rpm, gpu_fan_rpm, fan_duty);
            fflush(stdout);
        }
        
        usleep(1000 * 1000); /* 1 second interval */
    }
    
    printf("\nAuto mode stopped.\n");
    return 0;
}

static int run_set_mode(int percentage) {
    if (percentage < 0 || percentage > 100 || percentage % 10 != 0) {
        fprintf(stderr, "Error: Percentage must be in 10%% steps (10, 20, ..., 100)\n");
        return EXIT_FAILURE;
    }
    
    printf("Setting fan speed to %d%%...\n", percentage);
    if (set_fan_duty(percentage) != 0) {
        fprintf(stderr, "Error: Failed to set fan duty\n");
        return EXIT_FAILURE;
    }
    
    /* Read and print status */
    int cpu_temp = get_cpu_temp();
    int gpu_temp = get_gpu_temp();
    int cpu_fan_rpm = get_cpu_fan_rpm();
    int gpu_fan_rpm = get_gpu_fan_rpm();
    print_status(cpu_temp, gpu_temp, cpu_fan_rpm, gpu_fan_rpm);
    return EXIT_SUCCESS;
}

static int run_default_mode(void) {
    /* Read status */
    int cpu_temp = get_cpu_temp();
    int gpu_temp = get_gpu_temp();
    int cpu_fan_rpm = get_cpu_fan_rpm();
    int gpu_fan_rpm = get_gpu_fan_rpm();
    print_status(cpu_temp, gpu_temp, cpu_fan_rpm, gpu_fan_rpm);
    return EXIT_SUCCESS;
}

static void print_usage(void) {
    printf("Usage: %s [OPTIONS]\n", PROGRAM_NAME);
    printf("\n");
    printf("Dudes Control Center - CLI interface for notebook fan control\n");
    printf("\n");
    printf("Options:\n");
    printf("  (none)         Show CPU/GPU temperatures and fan status (default)\n");
    printf("  --set <pct>    Set fan speed to specified percentage (0,10,...,100; 0 = fans off)\n");
    printf("  --auto         Enable automatic fan control based on temperature\n");
    printf("  --log-level <LEVEL>  Set daemon log level (ERROR, WARN, INFO, DEBUG)\n");
    printf("  --log-level-get      Show current daemon log level\n");
    printf("  --help         Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s              # Show current status\n", PROGRAM_NAME);
    printf("  %s --set 50     # Set fan to 50%% speed\n", PROGRAM_NAME);
    printf("  %s --auto       # Enable automatic control\n", PROGRAM_NAME);
    printf("  %s --log-level DEBUG   # Enable debug logging\n", PROGRAM_NAME);
    printf("  %s --log-level-get     # Show current log level\n", PROGRAM_NAME);
}

int main(int argc, char *argv[]) {
    /* Check for help first (before EC init) */
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                print_usage();
                return EXIT_SUCCESS;
            }
        }
    }
    
    /* Handle log-level commands (no EC access needed) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log-level") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --log-level requires a level argument (ERROR, WARN, INFO, DEBUG)\n");
                print_usage();
                return EXIT_FAILURE;
            }
            char level_arg[32];
            strncpy(level_arg, argv[++i], sizeof(level_arg) - 1);
            level_arg[sizeof(level_arg) - 1] = '\0';
            
            char resp[256];
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "SET_LOG_LEVEL %s", level_arg);
            if (daemon_send_command(cmd, resp, sizeof(resp)) == 0) {
                printf("%s", resp);
            } else {
                fprintf(stderr, "Error: Failed to set log level\n");
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }
        
        if (strcmp(argv[i], "--log-level-get") == 0) {
            char resp[256];
            if (daemon_send_command("GET_LOG_LEVEL", resp, sizeof(resp)) == 0) {
                printf("%s", resp);
            } else {
                fprintf(stderr, "Error: Failed to get log level\n");
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }
    }
    
    /* Initialize EC access */
    if (ec_init() != 0) {
        fprintf(stderr, "Error: Failed to initialize EC access. Try running with sudo.\n");
        return EXIT_FAILURE;
    }
    
    /* Parse arguments */
    if (argc == 1) {
        /* Default mode - show status */
        int result = run_default_mode();
        ec_cleanup();
        return result;
    }
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--set") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --set requires a percentage argument\n");
                print_usage();
                ec_cleanup();
                return EXIT_FAILURE;
            }
            
            int pct = atoi(argv[++i]);
            int result = run_set_mode(pct);
            ec_cleanup();
            return result;
        }
        
        if (strcmp(argv[i], "--auto") == 0) {
            int result = run_auto_mode();
            ec_cleanup();
            return result;
        }
        
        fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
        print_usage();
        ec_cleanup();
        return EXIT_FAILURE;
    }
    
    ec_cleanup();
    return EXIT_SUCCESS;
}
