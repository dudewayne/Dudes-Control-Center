/*
 * dudes-control-center-secure.c
 * Security helpers shared by the root daemons (see header for details).
 */

#define _GNU_SOURCE
#include "dudes-control-center-secure.h"
#include "dudes-control-center-log.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------------ */
/* Socket client authorization                                              */
/* ------------------------------------------------------------------------ */

static int user_in_group(const struct passwd *pw, gid_t gid) {
    if (pw->pw_gid == gid) return 1;

    int ngroups = 64;
    gid_t *groups = malloc(sizeof(gid_t) * (size_t)ngroups);
    if (!groups) return 0;

    if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0) {
        /* ngroups now holds the required size */
        gid_t *bigger = realloc(groups, sizeof(gid_t) * (size_t)ngroups);
        if (!bigger) { free(groups); return 0; }
        groups = bigger;
        if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0) {
            free(groups);
            return 0;
        }
    }

    int found = 0;
    for (int i = 0; i < ngroups; i++) {
        if (groups[i] == gid) { found = 1; break; }
    }
    free(groups);
    return found;
}

int dcc_peer_authorized(int client_fd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || len != sizeof(cred)) {
        LOG_WARN("[AUTH] Cannot read peer credentials: %s - rejecting client", strerror(errno));
        return 0;
    }

    if (cred.uid == 0) return 1;

    struct group *gr = getgrnam(DCC_AUTH_GROUP);
    if (!gr) {
        LOG_WARN("[AUTH] Group '%s' does not exist - only root may connect (rejected uid=%u pid=%d)",
                 DCC_AUTH_GROUP, (unsigned)cred.uid, (int)cred.pid);
        return 0;
    }
    gid_t auth_gid = gr->gr_gid;   /* copy before the next NSS call reuses the buffer */

    struct passwd *pw = getpwuid(cred.uid);
    if (!pw) {
        LOG_WARN("[AUTH] Unknown uid %u (pid %d) - rejecting client", (unsigned)cred.uid, (int)cred.pid);
        return 0;
    }

    if (user_in_group(pw, auth_gid)) return 1;

    LOG_WARN("[AUTH] Rejected client uid=%u (%s) pid=%d: not a member of group '%s'",
             (unsigned)cred.uid, pw->pw_name, (int)cred.pid, DCC_AUTH_GROUP);
    return 0;
}

void dcc_reject_client(int client_fd) {
    static const char msg[] = "ERROR Permission denied: user is not in group '" DCC_AUTH_GROUP "'\n";
    /* Best effort; never block and never raise SIGPIPE */
    (void)!send(client_fd, msg, sizeof(msg) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
    close(client_fd);
}

void dcc_ignore_sigpipe(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
}

/* ------------------------------------------------------------------------ */
/* File I/O with the identity of the file's owner                           */
/* ------------------------------------------------------------------------ */

typedef struct {
    int switched;
} FsIdentity;

/* Find the owner of the directory that contains path.
 * The directory must be a real directory (not a symlink) owned by a non-root user. */
static int resolve_dir_owner(const char *path, uid_t *uid, gid_t *gid) {
    char dir[PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(dir)) return -1;
    memcpy(dir, path, n + 1);

    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    if (slash == dir) slash[1] = '\0'; else *slash = '\0';

    struct stat st;
    if (lstat(dir, &st) != 0) {
        int err = errno;
        LOG_WARN("[SECURE] Cannot stat directory %s: %s", dir, strerror(err));
        errno = err;
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        LOG_ERROR("[SECURE] %s is not a real directory (symlink or other) - refusing file access", dir);
        errno = EPERM;
        return -1;
    }
    if (st.st_uid == 0 && geteuid() == 0) {
        LOG_ERROR("[SECURE] %s is owned by root; expected the desktop user. "
                  "Fix with: sudo chown <user>: %s", dir, dir);
        errno = EPERM;
        return -1;
    }
    *uid = st.st_uid;
    *gid = st.st_gid;
    return 0;
}

/* Switch file-system uid/gid to the target user. While the fsuid is non-zero
 * the kernel also drops CAP_DAC_OVERRIDE & co., so every access is checked
 * exactly as if the user had made it. No-op when not running as root. */
static int fs_switch(uid_t uid, gid_t gid, FsIdentity *id) {
    id->switched = 0;
    if (geteuid() != 0) return 0;

    /* Root's supplementary groups would still apply after setfsgid.
     * The daemons never need them, so drop them once (process-wide). */
    static int groups_dropped = 0;
    if (!groups_dropped) {
        if (setgroups(0, NULL) != 0) {
            LOG_ERROR("[SECURE] setgroups(0) failed: %s", strerror(errno));
            return -1;
        }
        groups_dropped = 1;
    }

    setfsgid(gid);
    setfsuid(uid);
    /* setfsuid/setfsgid return the previous value; calling with -1 queries */
    if ((uid_t)setfsuid((uid_t)-1) != uid || (gid_t)setfsgid((gid_t)-1) != gid) {
        LOG_ERROR("[SECURE] Failed to switch file-system identity to %u:%u", (unsigned)uid, (unsigned)gid);
        setfsuid(0);
        setfsgid(0);
        return -1;
    }
    id->switched = 1;
    return 0;
}

static void fs_restore(FsIdentity *id) {
    if (!id->switched) return;
    setfsuid(0);
    setfsgid(0);
    id->switched = 0;
}

static char *read_fd_all(int fd, size_t max_size, size_t *out_len) {
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    if (st.st_size < 0 || (size_t)st.st_size > max_size) return NULL;

    size_t cap = (size_t)st.st_size + 1;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    size_t total = 0;
    while (total < cap - 1) {
        ssize_t r = read(fd, buf + total, cap - 1 - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return NULL;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    if (out_len) *out_len = total;
    return buf;
}

char *dcc_user_file_read(const char *path, size_t max_size, size_t *out_len) {
    uid_t uid; gid_t gid;
    if (!path || resolve_dir_owner(path, &uid, &gid) != 0) return NULL;

    FsIdentity id;
    if (fs_switch(uid, gid, &id) != 0) return NULL;

    char *buf = NULL;
    int err = 0;
    /* O_NONBLOCK: never hang on a FIFO planted in place of the file */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd >= 0) {
        buf = read_fd_all(fd, max_size, out_len);
        if (!buf) {
            err = EINVAL;
            LOG_WARN("[SECURE] %s is not a regular file or exceeds %zu bytes", path, max_size);
        }
        close(fd);
    } else {
        err = errno;
        if (err != ENOENT) {
            LOG_WARN("[SECURE] Cannot open %s for reading: %s", path, strerror(err));
        }
    }

    fs_restore(&id);
    errno = err;   /* lets callers tell "missing" (ENOENT) from real errors */
    return buf;
}

int dcc_user_file_write(const char *path, const char *data, size_t len) {
    uid_t uid; gid_t gid;
    if (!path || !data || resolve_dir_owner(path, &uid, &gid) != 0) return -1;

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid()) >= (int)sizeof(tmp)) return -1;

    FsIdentity id;
    if (fs_switch(uid, gid, &id) != 0) return -1;

    int ret = -1;
    mode_t mode = 0644;
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        mode = st.st_mode & 0666;   /* keep the existing permissions */
    }

    unlink(tmp);  /* leftover from a crash; unlink never follows symlinks */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) {
        LOG_ERROR("[SECURE] Cannot create %s: %s", tmp, strerror(errno));
        goto out;
    }

    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, data + written, len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("[SECURE] Write to %s failed: %s", tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            goto out;
        }
        written += (size_t)w;
    }
    fchmod(fd, mode);  /* creation mode is filtered by umask */

    if (fsync(fd) != 0 || close(fd) != 0) {
        LOG_ERROR("[SECURE] Flushing %s failed: %s", tmp, strerror(errno));
        unlink(tmp);
        goto out;
    }

    /* rename() replaces a symlink at path instead of following it */
    if (rename(tmp, path) != 0) {
        LOG_ERROR("[SECURE] Cannot replace %s: %s", path, strerror(errno));
        unlink(tmp);
        goto out;
    }
    ret = 0;

out:
    fs_restore(&id);
    return ret;
}
