/* cmd_self_update.h: thin-client self-update.
 *
 * The thin client talks to exactly one aimee-server; when that server moves
 * ahead in version, the client can drift. These helpers let the client observe
 * the server version, decide whether it is behind, and (on request) fetch the
 * matching release binary from GitHub and atomically swap itself. */
#ifndef DEC_CMD_SELF_UPDATE_H
#define DEC_CMD_SELF_UPDATE_H 1

#include <stddef.h>

/* Compare two version strings by (major, minor, patch). A leading 'v' and any
 * "-<suffix>" (e.g. git-describe's "-31-gabc123") are ignored; missing
 * components count as 0. Returns <0 if a<b, 0 if equal, >0 if a>b. */
int aimee_version_compare(const char *a, const char *b);

/* True if `s` is a plausible/safe version string: non-empty and composed only
 * of [0-9A-Za-z._-] (so it is safe to interpolate into a release URL/command). */
int aimee_version_is_safe(const char *s);

/* True if `s` is a comparable semantic version (starts with a numeric component,
 * after an optional leading 'v'). A deliberately non-semver dev/branch build
 * version (e.g. "testing-<sha>") returns 0 -- ordering against it is meaningless. */
int aimee_version_is_semver(const char *s);

/* Release asset base name for the current platform (e.g. "aimee-linux-x86_64",
 * "aimee-linux-arm64", "aimee-macos-universal"). Returns a pointer to static
 * storage, or NULL on an unsupported platform. */
const char *aimee_self_update_asset(void);

/* GET /v1/version from the configured aimee-server and write its version string
 * to `out`. Best-effort, short timeout. Returns 0 on success, -1 otherwise
 * (no remote endpoint, transport failure, or malformed response). */
int aimee_fetch_server_version(char *out, size_t cap);

/* If a remote (thin-client) server is configured AND reports a version newer
 * than this client, write a one-line human notice to `out` and return 1.
 * Returns 0 (and leaves `out` empty) when up to date, not a thin client, or the
 * check could not be completed. Never blocks longer than a few seconds. */
int aimee_self_update_notice(char *out, size_t cap);

/* `aimee self-update [--check] [--version vX.Y.Z] [--yes] [--require-verify]`.
 * Returns a process exit code. --check reports drift without downloading. */
int cmd_self_update(int argc, char **argv);

/* When aimee.yaml sets `update_mode: apply` and the server is a strictly newer
 * semver, spawn a detached, SHA-256-verified self-update (rate-limited to once
 * per hour). No-op otherwise. Non-blocking; safe to call from SessionStart. */
void aimee_self_update_apply_async(void);

#endif /* DEC_CMD_SELF_UPDATE_H */
