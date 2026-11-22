// SPDX-License-Identifier: GPL-2.0-only
/*
 * test_secure.c - socket peer authorization and symlink-safe user-file I/O
 * (dcc-secure.c, added in v1.16.1).
 *
 * Usage: test_secure <scratch-dir> <allow|deny>
 *   Built twice by "make test": once with DCC_AUTH_GROUP = the current user's
 *   group (expects "allow") and once with a group that does not exist
 *   (expects "deny").
 */
#include "dcc-secure.h"
#include "dcc-log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("PASS  %s\n", m); else { printf("FAIL  %s\n", m); fails++; } } while (0)

static void write_plain(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static void read_plain(const char *path, char *buf, size_t n) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f) { if (!fgets(buf, (int)n, f)) buf[0] = '\0'; fclose(f); }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <scratch-dir> <allow|deny>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    int expect_allow = strcmp(argv[2], "allow") == 0;
    char p[1100], victim[1100], cfg[1024], link_dir[1100], q[1200], buf[64];

    /* --- peer authorization --- */
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int ok = dcc_peer_authorized(sv[0]);
    if (expect_allow) {
        CHECK(ok == 1, "member of DCC_AUTH_GROUP is authorized");
        close(sv[0]);
    } else {
        CHECK(ok == 0, "user outside DCC_AUTH_GROUP is rejected");
        dcc_reject_client(sv[0]);
        char msg[160] = {0};
        ssize_t n = read(sv[1], msg, sizeof msg - 1);
        CHECK(n > 0 && strstr(msg, "ERROR Permission denied"), "rejected client receives an error message");
    }
    close(sv[1]);

    /* --- SIGPIPE must not kill the process --- */
    dcc_ignore_sigpipe();
    int pp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, pp);
    close(pp[1]);
    CHECK(write(pp[0], "x", 1) == -1 && errno == EPIPE, "write to a closed peer returns EPIPE instead of killing us");
    close(pp[0]);

    if (!expect_allow) {   /* file tests only need to run once */
        printf("\n%d failure(s)\n", fails);
        return fails != 0;
    }

    /* --- safe user-file I/O --- */
    snprintf(cfg, sizeof cfg, "%s/cfg", dir);
    mkdir(cfg, 0755);
    snprintf(p, sizeof p, "%s/profiles.json", cfg);
    unlink(p);

    CHECK(dcc_user_file_write(p, "{\"a\":1}", 7) == 0, "atomic write succeeds");
    size_t n = 0;
    char *r = dcc_user_file_read(p, 1 << 20, &n);
    CHECK(r && strcmp(r, "{\"a\":1}") == 0 && n == 7, "read returns the written content");
    free(r);

    /* file replaced by a symlink to a "victim" file */
    snprintf(victim, sizeof victim, "%s/victim", dir);
    write_plain(victim, "VICTIM");
    unlink(p);
    if (symlink(victim, p) != 0) perror("symlink");
    r = dcc_user_file_read(p, 1 << 20, NULL);
    CHECK(r == NULL && errno == ELOOP, "read refuses to follow a symlink");
    free(r);
    CHECK(dcc_user_file_write(p, "NEW", 3) == 0, "write replaces the symlink itself");
    read_plain(victim, buf, sizeof buf);
    CHECK(strcmp(buf, "VICTIM") == 0, "symlink target is left untouched");
    struct stat st;
    CHECK(lstat(p, &st) == 0 && S_ISREG(st.st_mode), "profiles.json is a regular file again");

    /* config directory itself a symlink */
    snprintf(link_dir, sizeof link_dir, "%s/cfglink", dir);
    unlink(link_dir);
    if (symlink("cfg", link_dir) != 0) perror("symlink");
    snprintf(q, sizeof q, "%s/profiles.json", link_dir);
    CHECK(dcc_user_file_write(q, "X", 1) == -1, "write through a symlinked directory is refused");

    /* missing file -> ENOENT, so callers can tell "missing" from "broken" */
    snprintf(q, sizeof q, "%s/missing.json", cfg);
    r = dcc_user_file_read(q, 1 << 20, NULL);
    CHECK(r == NULL && errno == ENOENT, "missing file reports ENOENT");

    /* oversized file is refused */
    write_plain(p, "0123456789");
    r = dcc_user_file_read(p, 5, NULL);
    CHECK(r == NULL, "file larger than the limit is refused");

    printf("\n%d failure(s)\n", fails);
    return fails != 0;
}
