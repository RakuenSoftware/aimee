#ifndef DEC_AGENT_CONFIG_H
#define DEC_AGENT_CONFIG_H 1

#include "agent_types.h"
#include "config.h"
#include "vault_principal.h" /* VAULT_PRINCIPAL_MAX for the per-turn vault principal */
#include <stdatomic.h>       /* atomic_int for the per-turn cancel flag */

int agent_load_config(agent_config_t *cfg);
int agent_save_config(const agent_config_t *cfg);

/* As agent_save_config, but the caller has just removed an agent and an empty
 * registry is therefore the intended result. The deletion guard in
 * agent_save_config cannot tell a deliberate removal of the LAST delegate from a
 * zeroed or failed-to-load cfg, so it refuses both; removing the only configured
 * delegate failed with "could not save agents.json" until this existed. Only the
 * remove handler may use it — it is the one caller that knows. */
int agent_save_config_after_removal(const agent_config_t *cfg);

/* A valid agent/model slug: 1–48 chars, starting alphanumeric, then alphanumeric
 * or . _ - . Agent names surface as model ids in /v1/models, so this keeps junk
 * (over-long or non-identifier) names out of agents.json and the model list.
 * Returns 1 if valid, 0 otherwise. */
int agent_name_valid(const char *name);

/* Reject an endpoint that is really a mis-parsed flag (leading '-'). `agent add`'s
 * first three arguments are positional, so a flag in the endpoint slot used to be
 * stored as the address and only surfaced at `agent probe`. Narrow on purpose: a
 * scheme requirement would reject host:port forms this command has always taken. */
int agent_endpoint_valid(const char *endpoint);
const char *agent_config_path(void);
agent_t *agent_route(agent_config_t *cfg, const char *role);
agent_t *agent_route_at_tier(agent_config_t *cfg, const char *role, int tier);
/* The two system settings routing actually consults. This used to be a whole
 * config_t threaded through for exactly these two booleans -- callers had to hold
 * the 750 KB struct, and a reader could not tell which of its ~600 fields
 * mattered. Populate with agent_route_policy_current(). */
typedef struct
{
   int capability_routing; /* model_meta.capability_routing */
   int prefer_local;       /* prefer_local_agents */
} agent_route_policy_t;

/* Fill from the live config. */
void agent_route_policy_current(agent_route_policy_t *out);

agent_t *agent_route_with_caps(agent_config_t *cfg, const char *role,
                               const agent_route_policy_t *sys_cfg, unsigned required_caps,
                               int min_context);

/* Same, but for a packet of a declared SCOPE. An agent whose max_scope ceiling is
 * below the packet's scope is excluded, and unlike min_context that exclusion is
 * BINDING - escalation must never relax it, or a whole_task packet would escalate
 * into the very seat the operator declared unable to handle it. An UNSET packet
 * scope resolves to WHOLE_TASK: under uncertainty prefer the capable seat, because
 * over-selecting costs less, in tokens and wall-clock, than a misplacement.
 * agent_route_with_caps() is this with AGENT_SCOPE_UNSET. */
/* Target for a MISPLACEMENT escalation: the most capable eligible seat strictly
 * dearer than `failed_tier`. Most capable rather than one step up, because the
 * escalation allowance is spent once and over-selecting beats laddering. The
 * scope ceiling still binds. NULL when nothing dearer is eligible, in which case
 * the caller must fail for review rather than re-run the same class of seat. */
agent_t *agent_route_escalation_target(agent_config_t *cfg, const char *role, int failed_tier,
                                       unsigned required_caps, agent_scope_t scope);

agent_t *agent_route_with_caps_scoped(agent_config_t *cfg, const char *role,
                                      const agent_route_policy_t *policy, unsigned required_caps,
                                      int min_context, agent_scope_t scope);
agent_t *agent_find(agent_config_t *cfg, const char *name);

/* REGISTRY ACCESSORS: ask for the one agent you need, not the whole registry.
 *
 * The overwhelming majority of callers do the same three lines -- declare an
 * agent_config_t, agent_load_config() into it, agent_find() one agent -- and
 * agent_config_t is 350,968 bytes. Every such lookup therefore costs a stat(), a
 * 343 KB memset (which touches every page) and a 343 KB memcpy out of a cache
 * that already holds exactly the answer. There are 262 by-value declarations of
 * this struct in the tree, on paths including routing, workflows and roundtable,
 * so the copies land on request threads and churn straight into per-thread
 * malloc arenas.
 *
 * These read the cached registry IN PLACE under its existing lock and copy out a
 * single agent_t (16,720 bytes) -- ~21x less per lookup, and no memset at all.
 * The slow path (cache cold or stale) still performs one full load, because that
 * is genuinely a load; it just does not happen per request.
 *
 * Return 0 and fill `out` on success, non-zero when no such agent exists. `out`
 * is untouched on failure. */
int agent_registry_find(const char *name, agent_t *out);

/* As agent_default_primary, against the cached registry. */
int agent_registry_default_primary(agent_t *out);
/* Select the default "primary" agent for ingress paths that don't name a model:
 * an explicitly configured default when it is enabled, else the first enabled
 * agent, else NULL. Never returns a disabled agent. */
agent_t *agent_default_primary(agent_config_t *cfg);
int agent_is_available_for_routing(const agent_t *agent);

/* Optional server-owned selection provider. Eligibility, policy, and capability
 * filtering remain local; the provider selects an index from the resulting
 * equally eligible pool. `randomized` is nonzero for role/panel diversity and
 * zero for balanced ordinary routing. Once registered, provider failure fails
 * the route closed rather than falling back to an in-process selector. */
typedef int (*agent_route_selection_fn)(int randomized, uint32_t candidate_count,
                                        uint32_t *selected_index);
void agent_set_route_selection_provider(agent_route_selection_fn provider);

/* Why an agent is not a routable delegate. Mirrors the decision order of
 * agent_is_available_for_routing so callers can surface the ACTUAL reason
 * instead of a single catch-all "unavailable" string. */
typedef enum
{
   AGENT_ROUTE_OK = 0,             /* routable */
   AGENT_ROUTE_NULL,               /* NULL agent */
   AGENT_ROUTE_HEALTH_DOWN,        /* health catalog marked the provider DOWN (breaker open) */
   AGENT_ROUTE_CLIENT_ONLY_CLAUDE, /* claude CLI agent that is not server-hosted */
   AGENT_ROUTE_POLICY_EXCLUDED,    /* registered delegate-policy filter excluded it */
   AGENT_ROUTE_MISSING_COMMAND,    /* a required CLI (tmux / provider-cli) is not on PATH */
   AGENT_ROUTE_NO_CREDENTIALS,     /* an HTTP agent with no resolvable credentials */
} agent_route_block_t;

/* Same decision as agent_is_available_for_routing, but reports WHY. On a block
 * that names something specific (the missing command, or a Primary-Agent-Only
 * flag) a short phrase is written to `detail` (pass NULL/0 to skip). Returns
 * AGENT_ROUTE_OK when the agent is routable. */
agent_route_block_t agent_routing_block_reason(const agent_t *agent, char *detail,
                                               size_t detail_sz);

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

/* Optional route-time DEGRADED predicate (returns nonzero if the named agent is
 * degraded). Unlike the health filter it does NOT exclude the agent: routing
 * PREFERS a healthy peer when one can serve the role, and falls back to a
 * degraded seat only when none can. NULL (the default) disables the preference,
 * so filter-less CLI / test builds keep the prior cost-only behaviour and
 * agent_config.o gains no link dependency on the provider catalog. The server
 * registers a predicate reporting CATALOG_HEALTH_DEGRADED. */
void agent_set_route_degraded_filter(int (*fn)(const char *agent_name));

/* Optional route-time delegate-POLICY filter, same mechanism as the health
 * filter (returns nonzero to EXCLUDE the agent; NULL disables). The server
 * registers a predicate enforcing ONE invariant everywhere routing happens,
 * not just on one dispatch path:
 *   an agent flagged "Primary Agent Only" (agents.json `primary_only`) is
 *   never a delegation target — the per-agent choice that replaced the global
 *   claude_cli_delegate_enabled opt-in. There is deliberately no separate
 *   "provider-named agent is never a delegate" name match: that rule made the
 *   flag unreachable for a claude-oauth-as-primary box (the OAuth flow names
 *   the agent "claude", which then equals config.provider), so `primary_only`
 *   is now the sole per-agent gate and unchecking it opts the primary into
 *   self-delegation. Roundtable panels use explicit role-based eligibility
 *   (delegate_ensemble.c), with no identity-based exclusion of the primary.
 * The structural rule — a claude-CLI agent that is not server-hosted can never
 * execute as a delegate — is enforced unconditionally in
 * agent_is_available_for_routing, so even filter-less builds (CLI/tests) never
 * route to a client-only claude. */
void agent_set_route_policy_filter(int (*fn)(const agent_t *agent));

/* Optional route-time capacity probe: returns the number of slots CURRENTLY in
 * use by `agent_name`, or -1 when unknown/unconfigured. The server registers
 * agent_admission_agent_active; CLI and test builds leave it NULL.
 *
 * Routing PREFERS agents with a free slot (active < max_parallel) over saturated
 * ones. Without it, routing and admission disagreed about "available": routing
 * checks health, policy and structure but not capacity, so it handed out agents
 * that were already at max_parallel, and admission then rejected them with
 * AGENT_RC_AT_LIMIT. Health could not close the gap, because being at-limit is
 * deliberately NOT recorded as a provider fault (see agent_fallback.c), so a
 * saturated agent is never marked DOWN and stays selectable forever.
 *
 * This only ever REORDERS preference — if every candidate is saturated, routing
 * falls back to the full set, so a populated pool can never be filtered down to
 * "no agent available", and blocking admission still waits for a slot. An
 * unknown answer (-1, or no probe) counts as "has capacity". */
void agent_set_route_capacity_probe(int (*fn)(const char *agent_name));
/* Live delegate occupancy for `agent_name`, or -1 when unknown. Lets the served
 * agent list publish occupancy to out-of-process routers (the Go WFE). */
int agent_route_agent_active(const char *agent_name);

/* Primary-turn marker for the delegate-policy filter. The PRIMARY chat turn
 * routes the provider-named agent through the same machinery as delegation
 * (agent_run_with_tools -> agent_route), where the `primary_only` gate above
 * would exclude a primary-only agent — but a primary turn is not delegation:
 * driving claude via its CLI as the PRIMARY is the documented default. The
 * chat worker brackets the turn with set(1)/set(0) on
 * its own thread (thread-local, so concurrent delegate routing on other
 * threads stays policed); the server's policy predicate consults it. */
void agent_routing_set_primary_turn(int on);
int agent_routing_primary_turn(void);

/* Vendor identity for model-capability lookup: `catalog_provider` when set,
 * otherwise `provider`. Use this for EVERY model_capability_get() call that
 * would otherwise pass an agent's provider — `provider` names the wire shape and
 * is wrong for a third-party vendor served over another vendor's API (MiniMax,
 * Kimi over Anthropic). Never use it for auth, headers, or request building. */
const char *agent_catalog_provider(const agent_t *agent);

/* Effective per-model limits: the operator's DECLARED value when they set one
 * (tested by AGENT_DECL_*, so a declared 0 counts), else the model catalog.
 * 0 means unknown. Use these rather than reaching for model_context_window() /
 * model_max_output() directly -- the catalog half is being removed, and these
 * are where it lives. */
int agent_declared_context_window(const agent_t *agent);
int agent_declared_max_output(const agent_t *agent);

/* Registration prefix of a route-target name: everything before the first ':'.
 * Provider-general registration names its targets `<registration>:<model>`, so
 * this identifies siblings sharing credentials, endpoint and wire protocol —
 * used to prefer a same-registration peer during fallback. A legacy
 * single-model agent has no ':' and is its own registration. */
void agent_registration_prefix(const char *name, char *out, size_t out_len);

/* 1 if `a` and `b` belong to the same provider registration, and so share
 * credentials, endpoint and wire protocol. This is what fallback consults to
 * prefer a sibling model before crossing to another vendor.
 *
 * Compares the STORED registration, never a name prefix: a prefix parse groups
 * a legacy agent coincidentally named "gw:backup" with the targets of a
 * registration "gw", and flattens a registration named "gw:east" down to "gw" —
 * both group seats with unrelated endpoints and credentials.
 *
 * An agent with no registration (a legacy single-model entry, or one written by
 * hand) is its own group and has no siblings, so it never matches — including
 * against another unregistered agent.
 *
 * NOTE: after a save/load cycle `registration` is trusted operator-supplied
 * grouping metadata, not authenticated provenance. Expansion collapses the
 * operator's `models` form into individual agents, so a hand-edited agents.json
 * can assign any value. That is the same trust level as every other field in
 * that file (endpoints, credentials policy, routing eligibility). */
int agent_same_registration(const agent_t *a, const agent_t *b);

int agent_has_role(const agent_t *agent, const char *role);
int agent_supports_persona(const agent_t *agent, const char *persona);
int agent_is_exec_role(const agent_t *agent, const char *role);
void agent_expand_env(const char *src, char *dst, size_t dst_len);

/* The agent's API key AS A SECRET, resolved on demand.
 *
 * agents.json may carry either a literal key or a "$VAR" reference. The config
 * module stores whatever is on disk VERBATIM and never resolves it, so the
 * registry -- a 350,968-byte struct that is copied per lookup and cached in
 * memory for the process's lifetime -- holds a reference rather than a
 * credential. Resolution happens here, in the module that owns credentials,
 * at the moment of use.
 *
 * Returns 1 and fills `out` when a key is available, 0 otherwise (`out` is left
 * empty). Callers own the copy and should runtime_secret_wipe() it when done. */
int agent_api_key_secret(const agent_t *agent, char *out, size_t out_len);

int agent_resolve_auth(const agent_t *agent, char *buf, size_t buf_len);
/* An explicit, actionable reason the LAST agent_resolve_auth call failed (e.g.
 * codex REAUTH_REQUIRED), or NULL. Lets the delegate/chat error path surface a
 * remedy instead of a generic provider 401 (D6). Thread-local to the turn. */
const char *agent_request_auth_error(void);
int agent_has_resolvable_credentials(const agent_t *agent);

/* Does the server vault hold any credential filed under this PROVIDER's name?
 *
 * The provider-level twin of agent_has_resolvable_credentials(). It exists
 * because provider availability used to be decided solely by
 * runtime_secret_has(env_var), and that table is loaded from one agent namespace
 * ("environment") for a hardcoded list of AIMEE_* names — so a key stored the
 * way an operator stores one, `aimee vault set minimax api_key ...`, was
 * invisible and `provider list` reported [no key] over a populated vault.
 *
 * Matching is case-insensitive on the name: provider ids are lowercase literals
 * in the catalogue while a vault entry carries whatever the operator typed.
 * Returns 1 when a credential exists, 0 otherwise and on any vault error, so a
 * failure to read reads as "not configured" rather than a false positive. */
int vault_provider_has_credential(const char *provider_name);

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
 * an API key. Still used for the structural server-hosted delegate rule and by
 * the OAuth setup path; the primary-only DEFAULT for claude is now expressed as
 * the per-agent `primary_only` flag set at add time (see agent_t.primary_only).
 * All other agents, including other CLI agents (Codex CLI, gemini-cli) and
 * API-key/HTTP agents, return 0. */
int agent_is_claude_cli(const agent_t *agent);

/* Generalized role dispatch. delegate_pick_for_role returns the index of a
 * viable agent (enabled, routable, serves `role`) not named in `exclude`,
 * uniformly at random among the eligible set — or -1 if none remain. Callers
 * loop pick->run->exclude-on-failure->repick until one works. A roundtable of N
 * `review` delegates excludes those already used to get diverse reviewers. */
int delegate_pick_for_role(agent_config_t *cfg, const char *role, const char *const exclude[],
                           int nexclude);
/* Pinned-model analog of delegate_pick_for_role: return the index of the agent
 * NAMED `name` iff it is enabled, serves `role`, and is routable right now — the
 * same eligibility triple, but no random draw and NO substitution. A pinned
 * roundtable seat resolves through this; -1 means the pinned model cannot be
 * fulfilled, so the caller fails the run rather than seating a different agent. */
int agent_pick_named_for_role(agent_config_t *cfg, const char *name, const char *role);
/* Seed the picker's RNG for deterministic tests (otherwise /dev/urandom). */
void delegate_role_pick_seed(unsigned seed);

#endif /* DEC_AGENT_CONFIG_H */
