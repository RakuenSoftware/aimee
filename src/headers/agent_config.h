#ifndef DEC_AGENT_CONFIG_H
#define DEC_AGENT_CONFIG_H 1

#include "agent_types.h"
#include "config.h"

int agent_load_config(agent_config_t *cfg);
int agent_save_config(const agent_config_t *cfg);
const char *agent_config_path(void);
agent_t *agent_route(agent_config_t *cfg, const char *role);
agent_t *agent_route_at_tier(agent_config_t *cfg, const char *role, int tier);
agent_t *agent_route_with_caps(agent_config_t *cfg, const char *role, const config_t *sys_cfg,
                               unsigned required_caps, int min_context);
agent_t *agent_find(agent_config_t *cfg, const char *name);
int agent_is_available_for_routing(const agent_t *agent);

/* Optional route-time health filter. When a predicate is registered, routing
 * (agent_route / agent_route_at_tier / delegate fallback — everything that
 * goes through agent_is_available_for_routing) treats an agent the predicate
 * reports unavailable as if it were disabled, so new work is never dispatched
 * to it. The predicate returns nonzero to EXCLUDE the named agent. NULL (the
 * default) disables filtering. The server registers a predicate that excludes
 * agents whose provider health the catalog has marked DOWN; the CLI / test /
 * bench builds leave it NULL and keep the prior behaviour, so agent_config.o
 * gains no link dependency on provider_catalog. */
void agent_set_route_health_filter(int (*fn)(const char *agent_name));

int agent_has_role(const agent_t *agent, const char *role);
int agent_is_exec_role(const agent_t *agent, const char *role);
void agent_expand_env(const char *src, char *dst, size_t dst_len);
int agent_resolve_auth(const agent_t *agent, char *buf, size_t buf_len);
int agent_has_resolvable_credentials(const agent_t *agent);
void agent_build_extra_headers(const agent_t *agent, char *buf, size_t buf_len);

/* Per-turn Codex OAuth creds supplied by the thin client (its ~/.codex/auth.json
 * is the live, refreshed source; the server has no such file). Set at the start
 * of a chat/delegate turn and cleared at the end. When set, agent_resolve_auth
 * uses `token` for a codex-oauth agent in preference to the server's file, and
 * agent_build_extra_headers injects ChatGPT-Account-ID from `account_id`. Pass
 * NULL/empty to clear. Thread-local (each turn runs on its own worker thread). */
void agent_set_request_codex_creds(const char *token, const char *account_id);

/* Per-turn session id for the session-scoped credential keyring lookup (see
 * session_credentials.h). Set at the start of a chat/delegate turn from the
 * request, cleared (NULL/empty) otherwise. When set, agent_resolve_auth prefers
 * a client-pushed session key for the agent over the server-stored api_key/env.
 * Thread-local. */
void agent_set_request_session(const char *session_id);

/* Snapshot of the per-turn, thread-local credential context (session id + Codex
 * creds). agent_set_request_session / agent_set_request_codex_creds bind these
 * on the dispatching thread, but a parallel fan-out (agent_run_parallel) runs
 * each agent on a fresh worker thread that does NOT inherit thread-locals — so
 * the dispatcher snapshots its context and each worker restores it, or the
 * panel runs keyless. */
typedef struct
{
   char session_id[128];
   char codex_token[MAX_API_KEY_LEN];
   char codex_account_id[128];
} agent_request_creds_t;

void agent_request_creds_snapshot(agent_request_creds_t *out);
void agent_request_creds_restore(const agent_request_creds_t *creds);

/* True if `agent` is the Claude CLI (`claude` / `claude-code`) run via tmux or
 * the provider-CLI binary — authenticated by the interactive `claude` login, not
 * an API key. Used to keep Claude-via-CLI primary-only by default (gated as a
 * delegate behind config.claude_cli_delegate_enabled). All other agents,
 * including other CLI agents (Codex CLI, gemini-cli) and API-key/HTTP agents,
 * return 0. */
int agent_is_claude_cli(const agent_t *agent);

#endif /* DEC_AGENT_CONFIG_H */
