#ifndef DEC_CLIENT_INTEGRATIONS_H
#define DEC_CLIENT_INTEGRATIONS_H 1

#include <stddef.h> /* size_t */

void ensure_client_integrations(void);

/* Register a probe the client-setup path calls once to decide whether the
 * sub-agent ban should be materialized: returns 1 if usable delegates exist, 0
 * if none, or -1 if unknown (server unreachable). CORE cannot make a /v1 call
 * (it links into DB-free clients AND the server), so the CLI injects the real
 * probe (agent.list -> any_delegate_available). Unset -> treated as unknown. */
void client_integrations_set_delegate_probe(int (*probe)(void));

/* Choose the client path to persist into the user's global config. |installed|
 * is the installed client (NULL/empty when absent), |exe| the running binary.
 * Prefers the install so a binary run from a throwaway build tree cannot write
 * its own soon-to-vanish path into config that outlives it. Returns 0 on
 * success. Exposed for tests. */
int client_integrations_pick_bin_path(const char *installed, const char *exe, char *out,
                                      size_t cap);

/* Enable (enable=1) or disable (enable=0) routing Claude Code through aimee's
 * Anthropic Messages ingress, by writing/removing ANTHROPIC_BASE_URL +
 * ANTHROPIC_AUTH_TOKEN in ~/.claude/settings.json. Opt-in only — enabling
 * reroutes the operator's live Claude Code to aimee's primary model. Returns 0
 * on success, -1 on error. */
int claude_code_proxy_configure(const char *server_url, const char *token, int enable);

void ensure_codex_project_trusted(const char *codex_home, const char *project_root);
void ensure_codex_current_project_trusted(const char *codex_home);

#endif /* DEC_CLIENT_INTEGRATIONS_H */
