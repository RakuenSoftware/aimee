#ifndef WEBUSER_RUNTIME_H
#define WEBUSER_RUNTIME_H 1

#include <stddef.h>

/* webuser_runtime — per-webuser ephemeral runtime dir for credential-injection
 * sockets (webchat-git WP-L). It is the home for WP-C's per-principal GIT_ASKPASS
 * socket and in-memory ssh-agent socket.
 *
 * HARD INVARIANT (fail-closed): the runtime base MUST be a tmpfs/ramfs mount, so
 * a socket — or any transient state a consumer puts here — can never spill to
 * persistent disk. If the base is not tmpfs, dir resolution REFUSES (returns -1)
 * rather than fall back to disk; the caller must then refuse the credential
 * operation. Per-user dirs are 0700 (OS-enforced cross-user isolation, matching
 * the WP-A workspace model + WP-I per-UID code-server).
 *
 * Base: $AIMEE_RUNTIME_DIR (absolute) else $XDG_RUNTIME_DIR/aimee else
 * /dev/shm/aimee — then /webusers/<name>. */

/* 1 iff `path` resides on a tmpfs or ramfs mount (statfs f_type), 0 if it is on
 * any other filesystem, -1 if it cannot be stat'd. Pure-ish (a syscall). */
int webuser_runtime_is_tmpfs(const char *path);

/* Resolve + create the per-`webuser:` 0700 runtime dir into out[cap]. FAIL-CLOSED:
 * returns -1 (and creates nothing under it) if the runtime base is not tmpfs, or
 * on a malformed/non-webuser principal or filesystem error. Returns 0 on success. */
int webuser_runtime_dir(const char *principal, char *out, size_t cap);

/* Recursively remove the principal's runtime dir (logout / session close).
 * Idempotent; best-effort. */
void webuser_runtime_cleanup(const char *principal);

#endif /* WEBUSER_RUNTIME_H */
