#ifndef WEBUSER_RUNTIME_H
#define WEBUSER_RUNTIME_H 1

#include <stddef.h>

/* webuser_runtime — environment-wide ephemeral runtime dir for credential-injection
 * sockets (webchat-git WP-L). It is the home for WP-C's per-principal GIT_ASKPASS
 * socket and in-memory ssh-agent socket.
 *
 * HARD INVARIANT (fail-closed): the runtime base MUST be a tmpfs/ramfs mount, so
 * a socket — or any transient state a consumer puts here — can never spill to
 * persistent disk. If the base is not tmpfs, dir resolution REFUSES (returns -1)
 * rather than fall back to disk; the caller must then refuse the credential
 * operation. The shared environment dir is 0700 and owned by aimee-server.
 *
 * Base: $AIMEE_RUNTIME_DIR (absolute) else $XDG_RUNTIME_DIR/aimee else
 * /dev/shm/aimee — then /environment. */

/* 1 iff `path` resides on a tmpfs or ramfs mount (statfs f_type), 0 if it is on
 * any other filesystem, -1 if it cannot be stat'd. Pure-ish (a syscall). */
int webuser_runtime_is_tmpfs(const char *path);

/* Decide whether `name` (length `len`) is a safe single path component, writing
 * 1 or 0 to `allowed`. Returns 0 when the decision was reached.
 *
 * The rule belongs to `workspace`, which owns what a project reference may be;
 * this file used to reach into its private header for it. The seam lets core
 * inject the workspace module's own answer over the event bus instead, so the
 * rule stays in one place without this becoming a direct module-to-module call.
 * The signature matches workspace's ref validator so core can register one
 * implementation for both. */
typedef int (*webuser_name_validator_fn)(const char *name, size_t len, int *allowed);
void webuser_runtime_register_name_validator(webuser_name_validator_fn validator);

/* Validate the `webuser:` actor, then resolve/create the shared 0700 environment
 * runtime dir into out[cap]. FAIL-CLOSED:
 * returns -1 (and creates nothing under it) if the runtime base is not tmpfs, or
 * on a malformed/non-webuser principal or filesystem error. Returns 0 on success. */
int webuser_runtime_dir(const char *principal, char *out, size_t cap);

/* Remove the shared environment runtime dir (server cleanup).
 * Idempotent; best-effort. */
void webuser_runtime_cleanup(const char *principal);

#endif /* WEBUSER_RUNTIME_H */
