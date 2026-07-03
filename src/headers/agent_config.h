#ifndef DEC_AGENT_CONFIG_H
#define DEC_AGENT_CONFIG_H 1

#include "agent_types.h"
#include "config.h"
#include "vault_principal.h" /* VAULT_PRINCIPAL_MAX for the per-turn vault principal */
#include <stdatomic.h>       /* atomic_int for the per-turn cancel flag */

int agent_load_config(agent_config_t *cfg);
int agent_save_config(const agent_config_t *cfg);

/* A valid agent/model slug: 1–48 chars, starting alphanumeric, then alphanumeric
 * or . _ - . Agent names surface as model ids in /v1/models, so this keeps junk
 * (over-long or non-identifier) names out of agents.json and the model list.
 * Returns 1 if valid, 0 otherwise. */
int agent_name_valid(const char *name);
const char *agent_config_path(void);
agent_t *agent_route(agent_config_t *cfg, const char *role);
agent_t *agent_route_at_tier(agent_config_t *cfg, const char *role, int tier);
agent_t *agent_route_with_caps(agent_config_t *cfg, const char *role, const config_t *sys_cfg,
                               unsigned required_caps, int min_context);
agent_t *agent_find(agent_config_t *cfg, const char *name);
int agent_is_available_for_routing(const agent_t *agent);

/* True if at least one configured agent is enabled AND routable as a delegate
 * right now (loads agents.json). Gates sub-agent interception — only redirect to
 * delegates when usable delegates exist. */
int agent_any_delegate_available(void);

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
int agent_supports_persona(const agent_t *agent, const char *persona);
int agent_is_exec_role(const agent_t *agent, const char *role);
void agent_expand_env(const char *src, char *dst, size_t dst_len);
int agent_resolve_auth(const agent_t *agent, char *buf, size_t buf_len);
/* An explicit, actionable reason the LAST agent_resolve_auth call failed (e.g.
 * codex REAUTH_REQUIRED), or NULL. Lets the delegate/chat error path surface a
 * remedy instead of a generic provider 401 (D6). Thread-local to the turn. */
const char *agent_request_auth_error(void);
int agent_has_resolvable_credentials(const agent_t *agent);
void agent_build_extra_headers(const agent_t *agent, char *buf, size_t buf_len);

/* Per-turn Codex OAuth creds supplied by the thin client (its ~/.codex/auth.json
 * is the live, refreshed source; the server has no such file). Set at the start
 * of a chat/delegate turn and cleared at the end. When set, agent_resolve_auth
 * uses `token` for a codex-oauth agent in preference to the server's file, and
 * agent_build_extra_headers injects ChatGPT-Account-ID from `account_id`. Pass
 * NULL/empty to clear. Thread-local (each turn runs on its own worker thread). */
void agent_set_request_codex_creds(const char *token, const char *account_id);

/* 1 when a per-turn Codex OAuth token is currently bound. Set by the vault
 * delegate path (delegate_credential_retry, from the vault); the legacy client
 * push was retired in P4b. */
int agent_request_codex_token_present(void);

/* Per-turn session id, set at the start of a chat/delegate turn from the request
 * and carried in the creds snapshot so a fan-out worker inherits the originating
 * turn's session identity. Credentials no longer ride this — the vault is the
 * single source (the session-scoped client keyring was retired in P4b).
 * Thread-local. */
void agent_set_request_session(const char *session_id);

/* The attested vault principal (WP-C) for the in-flight chat turn, thread-local.
 * The chat worker sets it from its compute_ctx around the agent loop and clears
 * it after, so a delegate the chat spawns in-process (decoupled from the
 * originating connection) can still reach the same user's vault:
 * create_compute_ctx falls back to this when its conn carries no principal. Set
 * strictly for the agent-loop duration; empty otherwise. NEVER a client value. */
void agent_set_request_vault_principal(const char *principal);
const char *agent_get_request_vault_principal(void);

/* Per-turn cancellation (server-owned turn lifecycle). The chat worker binds a
 * pointer to its turn-registry cancel flag around the in-process agent loop via
 * agent_set_request_cancel(&entry->cancel) and clears it (NULL) after. The
 * agent loop polls agent_request_cancelled() at safe points to abort a detached
 * turn whose session was closed / the server is shutting down. Thread-local. */
void agent_set_request_cancel(atomic_int *flag);
int agent_request_cancelled(void);

/* Snapshot of the per-turn, thread-local credential context (session id + Codex
 * creds + WP-C vault principal). agent_set_request_session /
 * agent_set_request_codex_creds / agent_set_request_vault_principal bind these
 * on the dispatching thread, but a parallel fan-out (agent_run_parallel) runs
 * each agent on a fresh worker thread that does NOT inherit thread-locals — so
 * the dispatcher snapshots its context and each worker restores it, or the
 * panel runs keyless. The vault principal rides along so a fan-out delegate
 * reaches the originating user's vault just like a same-thread delegate. */
typedef struct
{
   char session_id[128];
   char codex_token[MAX_API_KEY_LEN];
   char codex_account_id[128];
   char vault_principal[VAULT_PRINCIPAL_MAX];
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
