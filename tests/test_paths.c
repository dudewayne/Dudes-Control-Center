/*
 * test_paths.c - which user's profiles.json the root daemons use
 * (dcc_profiles_path in dcc-secure.c, v1.18.1).
 *
 * Usage: test_paths   (run in an empty directory; built with
 *        DCC_USER_FILE="user", i.e. a file in the current directory)
 */
#include "dcc-secure.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("PASS  %s\n", m); else { printf("FAIL  %s\n", m); fails++; } } while (0)

static void write_user(const char *text) {
    FILE *f = fopen("user", "w");
    if (f) { fputs(text, f); fclose(f); }
}

int main(void) {
    struct passwd *me = getpwuid(getuid());
    if (!me) { printf("FAIL  no passwd entry for the test user\n"); return 1; }
    char my_path[1024];
    snprintf(my_path, sizeof my_path, "%s/.config/dudes-control-center/profiles.json", me->pw_dir);
    const char *p;

    setenv("DCC_PROFILES_PATH", "/srv/test/profiles.json", 1);
    p = dcc_profiles_path();
    CHECK(p && strcmp(p, "/srv/test/profiles.json") == 0, "P: DCC_PROFILES_PATH (absolute) wins");
    setenv("DCC_PROFILES_PATH", "relative/profiles.json", 1);
    write_user(me->pw_name);
    p = dcc_profiles_path();
    CHECK(p && strcmp(p, my_path) == 0, "P: relative DCC_PROFILES_PATH ignored");
    unsetenv("DCC_PROFILES_PATH");

    write_user(me->pw_name);
    p = dcc_profiles_path();
    CHECK(p && strcmp(p, my_path) == 0, "P: user from the install-time user file -> that user's home");
    write_user("  \n");
    p = dcc_profiles_path();
    CHECK(!p || strstr(p, "/.config/dudes-control-center/profiles.json"), "P: empty user file -> fallback, no crash");

    write_user("root\n");
    p = dcc_profiles_path();
    CHECK(!p || strncmp(p, "/root/", 6) != 0, "P: root in the user file is refused");
    write_user("dcc-no-such-user-xyz\n");
    p = dcc_profiles_path();
    CHECK(!p || strstr(p, "dcc-no-such-user-xyz") == NULL, "P: unknown user in the user file -> fallback");

    unlink("user");
    p = dcc_profiles_path();
    struct passwd *u1000 = getpwuid(1000);
    if (u1000) {
        CHECK(p && strstr(p, "/.config/dudes-control-center/profiles.json"),
              "P: no user file -> group member or uid 1000");
    } else {
        CHECK(1, "P: no user file -> no uid 1000 on this system (skipped)");
    }
    CHECK(!p || strstr(p, "/home/dude/") == NULL || strcmp(me->pw_name, "dude") == 0 || (u1000 && strcmp(u1000->pw_name, "dude") == 0),
          "P: no hard-coded /home/dude");

    printf("\n%d failure(s)\n", fails);
    return fails != 0;
}
