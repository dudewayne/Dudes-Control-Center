/*
 * test_fan_responses.c - fan daemon response building (regression for the
 * GET_PROFILE stack overflow fixed in v1.16.1). No hardware access.
 * Build: see "make test".
 */
#define main daemon_main
#include "dcc-fan-daemon.c"
#undef main

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("PASS  %s\n", m); else { printf("FAIL  %s\n", m); fails++; } } while (0)

static int count_lines(const char *s) {
    int n = 0;
    for (; *s; s++) if (*s == '\n') n++;
    return n;
}

int main(void) {
    char resp[RESPONSE_SIZE];
    char small[200];
    char cmd[64];

    init_default_profile();

    /* 60 ADD_CURVE_POINTs (capped at 50) with very long numbers */
    for (int i = 0; i < 60; i++) {
        snprintf(cmd, sizeof cmd, "ADD_CURVE_POINT 0 %d 100\n", -1000000000 + i);
        handle_command(cmd, resp, sizeof resp);
    }
    CHECK(profiles[0].num_points == 50, "curve capped at 50 points");

    handle_command("GET_PROFILE 0\n", resp, sizeof resp);
    CHECK(count_lines(resp) == 52 && strstr(resp, "PROFILE_END\n"), "full 50-point profile fits in the response");

    /* A buffer far too small must truncate cleanly, never overflow (ASan checks this) */
    handle_command("GET_PROFILE 0\n", small, sizeof small);
    CHECK(strlen(small) < sizeof small && strstr(small, "PROFILE_END\n"), "tiny buffer truncates and keeps the end marker");

    handle_command("LIST_PROFILES\n", small, sizeof small);
    CHECK(strstr(small, "PROFILES_END\n") != NULL, "LIST_PROFILES keeps the end marker");

    /* Missing point data must not read uninitialised memory */
    handle_command("SET_CURVE_POINTS 0 3\n", resp, sizeof resp);
    CHECK(strstr(resp, "0 points") != NULL, "SET_CURVE_POINTS without data stores 0 points");

    printf("\n%d failure(s)\n", fails);
    return fails != 0;
}
