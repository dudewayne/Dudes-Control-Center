/*
 * test_fan_safety.c - fan daemon safety logic with mocked sensors and EC
 * (fail-safe, emergency override, profile handling, client I/O, sd_notify).
 * Build: see "make test".
 */
#define get_cpu_temp mock_cpu_temp
#define get_gpu_temp mock_gpu_temp
#define set_fan_duty mock_set_fan_duty
#define main daemon_main
#include "dudes-control-center-fan-daemon.c"
#undef main

static int m_cpu = 50, m_gpu = -1, m_fail_write = 0, m_last_write = -1, m_writes = 0;
int mock_cpu_temp(void) { return m_cpu; }
int mock_gpu_temp(void) { return m_gpu; }
int mock_set_fan_duty(int pct) { if (m_fail_write) return -1; m_last_write = pct; m_writes++; return 0; }

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) printf("PASS  %s\n", msg); else { printf("FAIL  %s (duty=%d override=%d active=%d)\n", msg, current_fan_duty, override_reason, active_profile_idx); fails++; } } while (0)

static void steps(int n) { for (int i = 0; i < n; i++) fan_control_step(); }
static char resp[RESPONSE_SIZE];
static const char *cmd(const char *c) { resp[0] = 0; handle_command(c, resp, sizeof resp); return resp; }

int main(void) {
    init_default_profile();
    auto_mode_enabled = 1; current_fan_duty = 20;

    /* 1. normal auto mode follows the curve */
    m_cpu = 70; steps(6);
    CHECK(current_fan_duty == 50, "auto mode reaches curve value at 70C (50%)");

    /* 2. sensor failure */
    m_cpu = -1; steps(1);
    CHECK(current_fan_duty == 50 && override_reason == OVERRIDE_NONE, "single bad reading holds current duty");
    steps(1);
    CHECK(current_fan_duty == 100 && override_reason == OVERRIDE_SENSOR_FAILURE, "2 bad readings -> fail-safe 100%");
    cmd("SET_MANUAL 30\n");
    CHECK(current_fan_duty == 100 && strstr(resp, "held at 100%"), "SET_MANUAL during fail-safe is held at 100%");
    auto_mode_enabled = 1;
    m_cpu = 50; steps(1);
    CHECK(override_reason == OVERRIDE_NONE && current_fan_duty < 100, "valid reading clears fail-safe and ramps down (auto)");

    /* 3. emergency override in manual mode */
    cmd("SET_MANUAL 0\n");
    CHECK(current_fan_duty == 0 && !auto_mode_enabled, "manual 0% applied");
    m_cpu = 96; steps(1);
    CHECK(current_fan_duty == 100 && override_reason == OVERRIDE_EMERGENCY_TEMP, "manual mode: 96C -> emergency 100%");
    CHECK(auto_mode_enabled && emergency_auto_count == 1,
          "emergency ends manual mode as soon as it starts (v1.18.4)");
    m_cpu = 92; steps(1);
    CHECK(current_fan_duty == 100, "hysteresis: still 100% at 92C");
    m_cpu = 89; steps(1);
    CHECK(override_reason == OVERRIDE_NONE && auto_mode_enabled && emergency_auto_count == 1,
          "below 90C after an emergency: auto mode takes over (v1.18.4)");
    CHECK(current_fan_duty > 0, "fans do not stop again after the emergency");
    cmd("SET_MANUAL 0\n");
    m_cpu = -1; steps(2);
    CHECK(current_fan_duty == 100, "manual mode: sensor failure -> 100%");
    m_cpu = 50; steps(1);
    CHECK(current_fan_duty == 0, "manual mode: recovery restores manual duty");
    m_gpu = 97; steps(1);
    CHECK(current_fan_duty == 100, "GPU at 97C also triggers emergency");
    cmd("SET_MANUAL 20\n");
    CHECK(current_fan_duty == 100 && !auto_mode_enabled, "manual set during the emergency is held at 100%");
    m_gpu = -1; steps(1);
    CHECK(auto_mode_enabled && override_reason == OVERRIDE_NONE,
          "manual set during the emergency -> auto mode when it clears");

    /* 4. SET_AUTO 0 keeps current speed */
    cmd("SET_ACTIVE_PROFILE 0\n"); m_cpu = 70; steps(6);
    int before = current_fan_duty;
    cmd("SET_AUTO 0\n"); steps(2);
    CHECK(current_fan_duty == before, "SET_AUTO 0 holds the current speed");

    /* 5. profile index handling */
    cmd("CREATE_PROFILE Quiet\n");   /* idx 1, empty */
    cmd("CREATE_PROFILE Loud\n");    /* idx 2 */
    cmd("SET_ACTIVE_PROFILE 1\n");
    CHECK(strstr(resp, "no curve points") && active_profile_idx == 0, "activating an empty profile is refused");
    cmd("SET_CURVE_POINTS 2 2 ,30,90,90,100\n");
    cmd("SET_ACTIVE_PROFILE 2\n");
    CHECK(active_profile_idx == 2, "profile with points can be activated");
    cmd("DELETE_PROFILE 1\n");
    CHECK(active_profile_idx == 1 && strcmp(profiles[active_profile_idx].name, "Loud") == 0,
          "deleting an earlier profile keeps the same active profile");
    cmd("DELETE_PROFILE 1\n");
    CHECK(active_profile_idx == 0 && strcmp(profiles[0].name, "Default") == 0,
          "deleting the active profile falls back to Default");

    /* 6. empty active curve -> built-in safe curve, not 20% */
    cmd("SET_CURVE_POINTS 0 0\n");
    CHECK(profiles[0].num_points == 0, "active profile emptied");
    CHECK(get_target_duty_from_temp(90) == 100, "empty active curve uses built-in curve (90C -> 100%)");
    init_default_profile();

    /* 7. curve evaluation does not reorder the stored points */
    cmd("SET_CURVE_POINTS 0 3 ,80,90,40,20,60,50\n");
    get_target_duty_from_temp(55);
    CHECK(profiles[0].points[0].temperature == 80 && profiles[0].points[2].temperature == 60,
          "evaluation does not sort the profile in place");
    CHECK(get_target_duty_from_temp(50) == 35, "unsorted curve still interpolates correctly (50C -> 35%)");
    init_default_profile();

    /* 8. EC write failure keeps the last known duty */
    auto_mode_enabled = 0; manual_duty = 40; current_fan_duty = 60; m_cpu = 50;
    m_fail_write = 1; steps(1);
    CHECK(current_fan_duty == 60, "failed EC write does not fake a new duty");
    m_fail_write = 0; steps(1);
    CHECK(current_fan_duty == 40, "write is retried on the next step");

    /* 9. client handling: pipelined commands and a client that stops reading */
    for (int i = 0; i < MAX_CLIENTS; i++) client_fds[i] = -1;
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
    client_fds[0] = sv[0]; num_clients = 1;
    const char *two = "GET_LOG_LEVEL\nLIST_PROFILES\n";
    (void)!write(sv[1], two, strlen(two));
    handle_client(sv[0], 0);
    char rb[4096] = {0}; ssize_t n = read(sv[1], rb, sizeof rb - 1);
    CHECK(n > 0 && strstr(rb, "LOG_LEVEL=") && strstr(rb, "PROFILES_START"), "two pipelined commands both answered");

    int small = 4096; setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    int dropped = 0;
    for (int i = 0; i < 2000 && !dropped; i++) {
        (void)!write(sv[1], "GET_PROFILE 0\n", 14);
        dropped = handle_client(sv[0], 0);
    }
    CHECK(dropped && client_fds[0] == -1, "client that never reads is dropped instead of blocking");
    close(sv[1]);

    /* 10. systemd notify message reaches NOTIFY_SOCKET */
    int ns = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    const char *name = "dcc-test-notify";
    memcpy(a.sun_path + 1, name, strlen(name));
    bind(ns, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name)));
    setenv("NOTIFY_SOCKET", "@dcc-test-notify", 1);
    sd_notify_msg("WATCHDOG=1");
    char nb[64] = {0}; recv(ns, nb, sizeof nb - 1, MSG_DONTWAIT);
    CHECK(strcmp(nb, "WATCHDOG=1") == 0, "sd_notify WATCHDOG=1 delivered");

    printf("\n%d failure(s)\n", fails);
    return fails != 0;
}
