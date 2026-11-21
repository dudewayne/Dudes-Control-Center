/*
 * dudes-control-center-secure.h
 * Security helpers shared by the root daemons:
 *   - authorization of socket clients (SO_PEERCRED + group membership)
 *   - safe reading/writing of files inside a user's home directory
 */

#ifndef DUDES_CONTROL_CENTER_SECURE_H
#define DUDES_CONTROL_CENTER_SECURE_H

#include <stddef.h>

/* Members of this group (and root) may send commands to the daemons.
 * Membership is read from the group database on every connection,
 * so adding a user takes effect immediately (no re-login needed). */
#ifndef DCC_AUTH_GROUP
#define DCC_AUTH_GROUP "dudes-control-center"
#endif

/* Check whether the peer of a connected AF_UNIX socket may use the daemon.
 * Returns 1 if authorized, 0 if not. Logs the decision for denied peers. */
int dcc_peer_authorized(int client_fd);

/* Send a "permission denied" error to the client and close the socket. */
void dcc_reject_client(int client_fd);

/* Ignore SIGPIPE so a client that disconnects early cannot kill the daemon. */
void dcc_ignore_sigpipe(void);

/*
 * Files in a user's home directory are written by root daemons.
 * To stop symlink/hardlink tricks from redirecting root's writes (or reads)
 * to system files, all I/O is done with the file-system identity
 * (setfsuid/setfsgid) of the user that owns the file's directory,
 * never follows a symlink in the last path component, and writes are atomic
 * (temp file + fsync + rename).
 */

/* Read a whole regular file. Returns a malloc'd NUL-terminated buffer
 * (caller frees) or NULL on error / if larger than max_size.
 * *out_len (optional) receives the length. */
char *dcc_user_file_read(const char *path, size_t max_size, size_t *out_len);

/* Atomically replace path with data. Returns 0 on success, -1 on error. */
int dcc_user_file_write(const char *path, const char *data, size_t len);

#endif /* DUDES_CONTROL_CENTER_SECURE_H */
