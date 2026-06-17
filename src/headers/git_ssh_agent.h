#ifndef GIT_SSH_AGENT_H
#define GIT_SSH_AGENT_H 1

#include <stddef.h>

/* git_ssh_agent — a per-webuser in-memory ssh-agent for git over SSH
 * (webchat-git WP-C2). The user's SSH private key is read from the sealed vault
 * and loaded into the agent via a memfd — it NEVER touches the filesystem. The
 * agent socket lives in the WP-L tmpfs runtime dir (0700); git/ssh use it via
 * SSH_AUTH_SOCK, so the private key never appears on disk or on a command line.
 * The agent (and any git/ssh child) runs with RLIMIT_CORE=0 + PR_SET_DUMPABLE=0
 * so a core dump can't capture the key. */

/* Ensure `principal`'s agent is running with their vaulted SSH key loaded, and
 * write its socket path to out[cap]. Returns:
 *   1  -> agent ready (out = SSH_AUTH_SOCK path);
 *   0  -> the principal has no vaulted SSH key (out empty; use HTTPS/ambient);
 *  -1  -> error (out empty) — e.g. an encrypted key, or the tmpfs gate failed. */
int git_ssh_agent_ensure(const char *principal, char *out, size_t cap);

/* Stop `principal`'s agent (kill it, remove its socket + pidfile). Idempotent. */
void git_ssh_agent_stop(const char *principal);

#endif /* GIT_SSH_AGENT_H */
