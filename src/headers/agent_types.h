#ifndef DEC_AGENT_TYPES_H
#define DEC_AGENT_TYPES_H 1

#include <limits.h>
#include <pthread.h>
#include <sys/types.h>

/* MAX_PATH_LEN. Previously reached only transitively via config.h; this header must not
 * depend on a caller including the config module first. */
#include "client_constants.h"

/* Forward declaration for cJSON (used by plan API). */
struct cJSON;

#define MAX_AGENTS               16
#define MAX_AGENT_ROLES          16
#define MAX_AGENT_PERSONAS       16
#define MAX_AGENT_NAME           64
#define MAX_ENDPOINT_LEN         512
#define MAX_MODEL_LEN            128
#define MAX_API_KEY_LEN          4096
#define MAX_AUTH_CMD_LEN         512
#define MAX_AGENT_CREDENTIALS    8
#define MAX_CRED_NAME_LEN        32
#define MAX_CRED_ENV_VAR_LEN     64
#define MAX_FALLBACK             8
#define AGENT_DEFAULT_TIMEOUT_MS 180000
/* Default per-call timeout for a REASONING model when the operator pins none.
 * Reasoning models (e.g. MiniMax-M3) emit a large hidden reasoning trace before
 * the visible answer, so a single completion routinely runs several minutes —
 * past the 3-minute standard default, which then fails as a spurious
 * "read_response failed" (HTTP -1) and burns the retry budget. A non-reasoning
 * model keeps the standard default; an explicit agents.json/--timeout value
 * always wins over either. */
#define AGENT_REASONING_TIMEOUT_MS 600000
/* Floor for the per-call HTTP timeout inside the multi-turn tool loop. Once the
 * remaining loop budget drops below this, the loop stops cleanly instead of
 * issuing a doomed short-timeout call that the provider-health tracker would
 * misread as "provider unreachable" (see posix/agent_runtime.c). */
#define AGENT_LOOP_MIN_CALL_MS 60000
/* 0 = "no explicit cap configured" — the request layer then derives the output
 * ceiling from the model registry (model_max_output) rather than a hardcoded
 * default, so every model gets its own full output budget. An agent may still
 * pin a smaller cap explicitly in agents.json / --max-tokens. */
#define AGENT_DEFAULT_MAX_TOKENS   0
#define MAX_EXEC_ROLES             8
#define MAX_EXEC_PROMPT_LEN        4096
#define AGENT_BACKEND_TMUX_CLI     "tmux-cli"
#define AGENT_BACKEND_PROVIDER_CLI "provider-cli"
#define AGENT_BACKEND_CLI_STDIO    "cli-stdio" /* legacy config alias */
#define CLI_CMD_MAX                256
#define AGENT_DEFAULT_MAX_PARALLEL 3
/* Default per-result cap (bytes) on the MODEL-VISIBLE tool output. Raised to
 * 32 KB (== AGENT_TOOL_OUTPUT_RAW_MAX) so full tool output flows to the model by
 * default; the (default-off) context-economizer losslessly compresses OLD
 * results to keep history bounded. Operators can LOWER the per-result cap via
 * the `tool_output_max_bytes` config key (resolved by agent_tool_output_cap()).
 * This constant is the resolver's fallback when the key is unset/0. */
#define AGENT_TOOL_OUTPUT_MAX (32 * 1024)
/* Raw capture safety buffer (bytes): the hard ceiling on bytes captured from a
 * child process before any model-visible truncation. NOT operator-configurable
 * and never exceeded by the resolved per-result cap. */
#define AGENT_TOOL_OUTPUT_RAW_MAX (32 * 1024)
#define AGENT_MAX_LIST_FILES      500
#define AGENT_MAX_TOOL_CALLS      16
#define AGENT_MAX_NET_HOSTS       64
#define AGENT_MAX_NETWORKS        8
#define AGENT_MAX_TUNNELS         8
#define AGENT_CONTEXT_BUDGET      16000
#define AGENT_CACHE_TTL_SECONDS   300
#define AGENT_MAX_PLAN_STEPS      32
#define AGENT_MAX_PLAN_DEPS       8
#define AGENT_MAX_CHECKPOINTS     32
#define AGENT_MAX_EVAL_TASKS      64
#define AGENT_MAX_COORD_AGENTS    4

/* Request-layer evidence gate: require provider tool selection until one tool
 * has returned usable evidence, but never force tools on a text-only final turn. */
static inline int agent_require_initial_tool_choice(int policy_enabled, int successful_tool_calls,
                                                    int tools_active)
{
   return policy_enabled && successful_tool_calls == 0 && tools_active;
}

/* Per-call HTTP timeout for one turn of the multi-turn tool loop.
 *   agent_timeout_ms  configured per-call timeout (also the per-call cap)
 *   total_timeout_ms  whole-loop budget (typically agent_timeout_ms * N)
 *   elapsed_ms        wall-clock already spent in the loop
 * Returns the timeout to use, or -1 when the remaining budget is too small for a
 * viable call (the caller should stop the loop cleanly rather than issue a
 * doomed short-timeout call that the provider-health tracker misreads as
 * "provider unreachable"). Pure function — unit-tested in test_agent.c. */
static inline int agent_loop_per_call_timeout_ms(int agent_timeout_ms, int total_timeout_ms,
                                                 int elapsed_ms)
{
   int remaining = total_timeout_ms - elapsed_ms;
   int min_call =
       agent_timeout_ms < AGENT_LOOP_MIN_CALL_MS ? agent_timeout_ms : AGENT_LOOP_MIN_CALL_MS;
   if (remaining < min_call)
      return -1;
   return agent_timeout_ms < remaining ? agent_timeout_ms : remaining;
}

/* Would this delegate be refused before its first call? A workflow stage cap may
 * be smaller than one viable call, in which case the loop stops immediately and
 * blames a budget it never got to spend. Callers preflight with this so the
 * refusal is stated honestly (and costs no model call). Pure -- unit-tested. */
static inline int agent_loop_window_too_small(int agent_timeout_ms, int total_timeout_ms)
{
   return agent_loop_per_call_timeout_ms(agent_timeout_ms, total_timeout_ms, 0) < 0;
}

/* Whole tool-loop budget for one delegate. The configured per-call timeout keeps
 * its existing four-call ceiling, while a positive request cap may only reduce
 * that budget. Workflow callers use the cap to leave time for post-delegate
 * verification before their stage deadline. */
static inline int agent_loop_total_timeout_ms(int agent_timeout_ms, int request_cap_ms)
{
   int configured =
       agent_timeout_ms > INT_MAX / 4 ? INT_MAX : (agent_timeout_ms > 0 ? agent_timeout_ms * 4 : 0);
   if (request_cap_ms > 0 && (configured <= 0 || request_cap_ms < configured))
      return request_cap_ms;
   return configured;
}

/* Apply a positive enclosing-request cap to one backend wait. Unlike the
 * configured timeout resolver above, a cap may replace an otherwise unbounded
 * CLI wait but can never lengthen a shorter configured timeout. */
static inline int agent_timeout_cap_ms(int timeout_ms, int request_cap_ms)
{
   if (request_cap_ms > 0 && (timeout_ms <= 0 || request_cap_ms < timeout_ms))
      return request_cap_ms;
   return timeout_ms;
}

/* Effective per-call timeout for a delegate run. A delegate must NEVER run with
 * a non-positive timeout: a timeout_ms <= 0 reaching the HTTP layer disables the
 * read deadline (agent_bridge conn_open sets deadline_ns = 0), so conn_read
 * loops on its per-recv timeout forever and a stalled provider hangs the worker
 * permanently — leaking its compute-pool thread + provider concurrency slot and
 * eventually wedging the whole background-delegate queue (jobs stuck "pending").
 * Resolve to the explicit request timeout if positive, else the agent's
 * configured timeout if positive, else a hard default ceiling — guaranteeing a
 * positive value so the run always returns. Pure function — unit-tested in
 * test_agent.c. */
static inline int delegate_effective_timeout_ms(int request_timeout_ms, int agent_timeout_ms)
{
   if (request_timeout_ms > 0)
      return request_timeout_ms;
   if (agent_timeout_ms > 0)
      return agent_timeout_ms;
   return AGENT_DEFAULT_TIMEOUT_MS;
}

typedef struct
{
   char name[64];
   char ip[64];
   char user[32];
   int port;
   char desc[256];
   char tunnel[64]; /* optional: name of tunnel for this host */
} agent_net_host_t;

typedef struct
{
   char name[64];
   char cidr[32];
   char desc[256];
} agent_net_def_t;

typedef enum
{
   TUNNEL_STATE_IDLE = 0,
   TUNNEL_STATE_CONNECTING,
   TUNNEL_STATE_ACTIVE,
   TUNNEL_STATE_RECONNECTING,
   TUNNEL_STATE_FAILED,
   TUNNEL_STATE_STOPPED
} agent_tunnel_state_t;

typedef struct
{
   /* Config (from JSON) */
   char name[64];
   char relay_ssh[512]; /* e.g. "ssh relay@relay.example.com" */
   char relay_key[MAX_PATH_LEN];
   char target_host[64];  /* internal target, e.g. "192.168.1.101" */
   int target_port;       /* internal target port, e.g. 22 */
   int reconnect_delay_s; /* seconds between retries, default 5 */
   int max_reconnects;    /* 0 = unlimited */

   /* Runtime state (not serialized) */
   agent_tunnel_state_t state;
   int allocated_port;
   pid_t ssh_pid;
   pthread_t monitor_thread;
   int reconnect_count;
   char error[256];
   char effective_entry[512]; /* computed: "ssh -p <port> user@relay" */
} agent_tunnel_t;

typedef struct
{
   agent_tunnel_t tunnels[AGENT_MAX_TUNNELS];
   int tunnel_count;
   pthread_mutex_t lock;
   volatile int shutdown;
} agent_tunnel_mgr_t;

typedef struct
{
   char ssh_entry[512];
   char ssh_key[MAX_PATH_LEN];
   agent_net_host_t hosts[AGENT_MAX_NET_HOSTS];
   int host_count;
   agent_net_def_t networks[AGENT_MAX_NETWORKS];
   int network_count;
   agent_tunnel_mgr_t *tunnel_mgr; /* optional, NULL when no tunnels */
} agent_network_t;

/* One entry in an agent's credential pool. The lease pool in
 * delegate_economics.c treats `credentials` as a round-robin set;
 * sibling delegates lease distinct entries when N >= 2 so a 429 on one
 * does not poison the sibling. `cooldown_until_ms` tracks per-credential
 * cooldown (set on 429, checked at acquire time). When the agent's
 * `credential_count == 0` the pool is empty and acquire/release fall
 * back to the existing single-token `api_key` field. */
typedef struct agent_credential
{
   char name[MAX_CRED_NAME_LEN];
   char api_key_env[MAX_CRED_ENV_VAR_LEN];
   long long cooldown_until_ms; /* monotonic ms; 0 = available */
} agent_credential_t;

/* Per-agent middleware configuration.
 * Zero values mean "use defaults" — the pipeline builder applies sensible
 * defaults when a field is zero (e.g., stall_threshold defaults to 3).
 * Set a field to -1 to explicitly disable that middleware. */
typedef struct agent_middleware_cfg
{
   int cost_limit;       /* max total tokens (prompt+completion); 0=disabled */
   int context_warn_pct; /* inject warning at this context usage %; 0=default(50) */
   int auto_compact_pct; /* trigger compaction at this context usage %; 0=default(80) */
   int stall_threshold;  /* consecutive tool errors before warning; 0=default(3) */
   int context_window;   /* explicit context window override; 0=auto-detect from model */
} agent_middleware_cfg_t;

/* Which per-model numbers the operator declared. See agent_t.declared.
 *
 * A bit means "the operator stated this value", NOT "the value is non-zero" --
 * that distinction is the whole point: it lets a free model declare 0 and an
 * unconfigured one stay silent, which one number cannot express on its own. */
enum
{
   AGENT_DECL_PRICE_IN = 1u << 0,
   AGENT_DECL_PRICE_OUT = 1u << 1,
   AGENT_DECL_PRICE_CACHED = 1u << 2,
   AGENT_DECL_CONTEXT_WINDOW = 1u << 3,
   AGENT_DECL_MAX_OUTPUT = 1u << 4,
};

typedef struct agent_ablation_flags
{
   int configured;        /* 0 = production/default path; treat all guardrails as enabled */
   int rescue;            /* multi-format rescue parsing */
   int respond_tool;      /* synthetic respond tool injection/strip */
   int sampling_defaults; /* per-model sampling defaults */
   int normalize;         /* robust tool-call validation/normalization */
   int retry;             /* retry/repair paths */
} agent_ablation_flags_t;

/* How large a unit of work a packet is, and the hardest an agent may be given.
 *
 * Ordinal, so a ceiling comparison is a simple `>`. UNSET means "not declared":
 * on an AGENT it means no ceiling; on a PACKET it resolves to WHOLE_TASK, because
 * under uncertainty this design OVER-SELECTS toward capability.
 *
 * Why over-select rather than lean on escalation: one capable session plus a
 * review is cheaper - and substantially faster - than a session, a review, an
 * escalation, another session and another review. Escalation pays for the failed
 * attempt AND everything after it, in tokens and in wall-clock. An escalation is
 * therefore a MISPLACEMENT INCIDENT, not a routine safety net.
 *
 * Two values only, matching the distinction observed delegate prompts actually
 * make: bounded SWE-bench style "fix this bug in this file" work versus
 * "implement the complete approved task in this worktree and verify it". */
typedef enum
{
   AGENT_SCOPE_UNSET = 0,
   AGENT_SCOPE_BOUNDED = 1,    /* a specified, self-contained change */
   AGENT_SCOPE_WHOLE_TASK = 2, /* the complete task, repo-wide verification */
} agent_scope_t;

const char *agent_scope_name(agent_scope_t s);
agent_scope_t agent_scope_from_string(const char *s);

typedef struct
{
   char name[MAX_AGENT_NAME];
   char endpoint[MAX_ENDPOINT_LEN];
   char model[MAX_MODEL_LEN];
   char api_key[MAX_API_KEY_LEN];
   /* The verbatim on-disk api_key — a "$VAR" reference (or, during boot-time
    * migration only, a legacy literal).
    * `api_key` above holds the RESOLVED value for runtime use (agent_expand_env
    * at load); this preserves the reference so agent_save_config never
    * re-serializes a resolved $VAR secret back into agents.json as plaintext.
    * Empty for agents created in-memory (e.g. `agent add $VAR`), where save may
    * retain the reference from api_key. Literal values are never serialized. */
   char api_key_disk[MAX_API_KEY_LEN];
   char auth_cmd[MAX_AUTH_CMD_LEN];
   char auth_type[16];
   char provider[16];
   /* Catalog (vendor) identity for model-capability lookup, distinct from
    * `provider` above, which names the WIRE SHAPE aimee speaks to this endpoint.
    * A third-party vendor served over another vendor's wire format (MiniMax and
    * Moonshot/Kimi both expose Anthropic-compatible endpoints) has
    * provider="anthropic" but catalog_provider="minimax"/"moonshotai". Only
    * capability lookup (model_capability_get and callers) may read this; request
    * building, auth, and headers must keep using `provider`, which selects the
    * anthropic-version header (agent_config.c), the x-api-key auth coercion, and
    * the credential env-var set. Empty means "same as provider". */
   char catalog_provider[16];
   /* 1 when catalog_provider came from agents.json rather than derivation. Only
    * an explicit value is re-serialized, so a save never freezes a derived guess
    * into config where it would outlive the derivation rules. */
   int catalog_provider_explicit;
   /* Operator reason this agent's cost_tier is exempt from the catalog-price
    * consistency check (agent_tier_lint). A subscription or flat-rate plan can
    * make per-token price the wrong basis for its tier — e.g. a ChatGPT/codex
    * OAuth seat whose marginal cost is not the published API price. Empty means
    * the check applies. A REASON is required rather than a bare boolean so an
    * exemption cannot silently hide a genuinely mis-tiered agent. */
   char tier_price_exempt[128];
   /* Operator price override, US dollars per million tokens. 0 = unset, meaning
    * "resolve from the model catalog". Set when the catalog price is not what
    * this deployment actually pays: a flat-rate or subscription seat whose
    * marginal token cost differs from the published API rate, negotiated or
    * committed-use pricing, a self-hosted model whose real cost is compute, or a
    * gateway that resells at its own margin. Both axes are independent, so a
    * deployment may override only one.
    *
    * KNOWN LIMITS:
    *  - 0 always means "unset, fall back to the catalog", so a genuinely FREE
    *    model cannot be expressed as 0. Such an agent is simply skipped by the
    *    price lint (no finding), which is the safe direction.
    *  - These fields are NOT round-trip safe across binary versions: an older
    *    binary loads agents.json, ignores these members, and drops them on its
    *    next save. Mixed-version operation against one config will silently lose
    *    pricing overrides. */
   double price_in_per_mtok;
   double price_out_per_mtok;
   double price_cached_per_mtok;
   /* Which of the numeric fields above (and max_output / middleware.context_window
    * below) the operator actually DECLARED, as AGENT_DECL_* bits.
    *
    * The fields alone cannot express this. Their documented convention is "0 =
    * unset, fall back to the catalog", which conflates a genuinely free model
    * with an undeclared one and caps a declared-as-0 value at unusable. That was
    * survivable only while a catalog sat underneath to answer; once operator
    * declaration is the authoritative source, "the operator said 0" and "the
    * operator said nothing" have to be different states.
    *
    * Set at load from KEY PRESENCE in agents.json rather than from the value, so
    * an existing config (no key) stays undeclared and nothing changes meaning
    * under an upgrade. Serialization is driven off these bits, so a declared 0
    * round-trips instead of vanishing on the next save. */
   unsigned declared;
   /* Output-token ceiling for this model, when the operator declares one.
    * Meaningful only with AGENT_DECL_MAX_OUTPUT set -- 0 is a legal declared
    * value in the same way a 0 price is. Distinct from max_tokens, which is the
    * per-REQUEST cap this deployment asks for; this is what the model will
    * actually emit. */
   int max_output;
   /* Hardest work this agent may be given. UNSET (the default, so every existing
    * config is unchanged) means no ceiling. Declared by the operator, who knows
    * their local model's limits better than any benchmark would: a small local
    * model can do bounded work but not whole-task work, and without this it would
    * win EVERY packet under cheapest-first routing. */
   agent_scope_t max_scope;
   /* The provider-general registration that GENERATED this target, when it was
    * generated. Empty for a legacy single-model agent, which is its own
    * registration. Stored rather than inferred from the name: parsing "text
    * before the first ':'" conflates a legacy agent coincidentally named
    * "gw:backup" with targets of a registration "gw", and reduces a registration
    * named "gw:east" to "gw" - grouping unrelated seats with different
    * endpoints and credentials as fallback peers. */
   char registration[MAX_AGENT_NAME];
   char roles[MAX_AGENT_ROLES][32];
   int role_count;
   /* Personas this agent may be dispatched AS (delegate identities: engineer,
    * architect, reviewer, ...). The wildcard "all" (or an empty/absent list, for
    * backward compatibility) means the agent may serve every persona. */
   char personas[MAX_AGENT_PERSONAS][32];
   int persona_count;
   int cost_tier;
   int max_tokens;
   int timeout_ms;
   /* Per-invocation whole tool-loop cap supplied by an enclosing workflow
    * deadline. Runtime-only: never loaded from or written to agents.json. */
   int tool_loop_timeout_ms_cap;
   int enabled;
   int tools_enabled;
   /* Per-invocation runtime policy (never serialized): require the first
    * tool-bearing model turn to select a tool. The tool loop clears the
    * requirement after the first call so the model can produce final text. */
   int require_initial_tool_call;
   int inject_respond_tool;
   int recommended_sampling;
   agent_ablation_flags_t ablation;
   int max_turns;
   int write_capable; /* 1 = write-capable delegate (grants the codex workspace-write
                       * sandbox; see cli_codex.c); 0 = read-only. No longer imposes a
                       * turn cap — the role's max_turns is the sole turn constraint. */
   int max_parallel;
   char exec_roles[MAX_EXEC_ROLES][32];
   int exec_role_count;
   char exec_system_prompt[MAX_EXEC_PROMPT_LEN];
   char extra_headers[256]; /* newline-separated extra HTTP headers, e.g. ChatGPT-Account-ID */
   char fallback_model[MAX_MODEL_LEN];
   /* Optional credential pool. When credential_count > 0, sibling
    * delegates lease distinct entries via delegate_economics; the
    * single-token `api_key` field above is unused for that agent. */
   agent_credential_t credentials[MAX_AGENT_CREDENTIALS];
   int credential_count;
   agent_middleware_cfg_t middleware; /* per-agent middleware pipeline config */
   /* CLI backend config (optional) */
   char backend[32];          /* "tmux-cli", "provider-cli", or "" for HTTP */
   char cli_cmd[CLI_CMD_MAX]; /* CLI command, e.g. "claude" or "gemini" */
   int cli_idle_timeout_ms;   /* explicit response timeout ms; 0/-1 = no timeout */
   int session_reuse;         /* 1 = reuse CLI session across tasks */
   int force_cli_isolation;   /* per-execution override: never reuse a CLI pane */
   int autonomous;            /* runtime global config: auto-approve provider CLI prompts */
   /* Picks the per-CLI adapter for AGENT_BACKEND_PROVIDER_CLI. Supported
    * values include "codex", "claude", "gemini", "mistral", "mistral-plan"
    * (Vibe-backed Mistral subscription-plan route), and "vibe-plan" (Vibe planning agent).
    * Empty when the backend is not provider-cli. */
   char cli_kind[16];
   /* 1 if this CLI agent was installed + OAuth'd ON the aimee-server host (via
    * `aimee agent add <vendor>-oauth`), so it runs as a real server-side delegate
    * — distinct from a client-only claude. The roundtable panel seats a
    * server-hosted, authenticated claude; a client-only one stays excluded. */
   int is_server_hosted;
   /* 1 = this agent may ONLY serve as the primary; it is never a delegation
    * target. 0 = delegate-eligible (subject to the structural rules in
    * agent_is_available_for_routing). Replaces the former global
    * config.claude_cli_delegate_enabled opt-in with a per-agent choice made at
    * add time: the Web GUI pre-checks "Primary Agent Only" for a claude-oauth
    * subscription (driving a personal Claude plan as an automated delegate may
    * breach Anthropic's terms) and leaves it off for every other agent. */
   int primary_only;
} agent_t;

typedef struct
{
   agent_t agents[MAX_AGENTS];
   int agent_count;
   char default_agent[MAX_AGENT_NAME];
   /* Preferred seat for unpinned delegate turns. This is deliberately separate
    * from default_agent: choosing a local worker must not replace the user's
    * primary chat model. If unavailable or ineligible for the requested role,
    * normal capability/cost routing remains the fallback. */
   char default_delegate[MAX_AGENT_NAME];
   char fallback_chain[MAX_FALLBACK][MAX_AGENT_NAME];
   int fallback_count;
   /* Transient per-request routing contract: an explicit agent/provider pin
    * must surface that agent's result and may never substitute a peer. */
   int route_pinned;
   /* One absolute per-request budget shared by the primary route, credential
    * retries, configured fallbacks, and same-tier fallbacks. Runtime-only: the
    * deadline is CLOCK_MONOTONIC milliseconds and is never serialized. */
   int tool_loop_timeout_ms_cap;
   int64_t tool_loop_deadline_ms;
   agent_network_t network;
   agent_tunnel_mgr_t tunnel_mgr;
   agent_ablation_flags_t ablation;
} agent_config_t;

typedef struct
{
   const char *role;
   const char *system_prompt;
   const char *user_prompt;
   int max_tokens;
   double temperature;
   /* Optional per-task participant selector. When set, this task runs on the
    * named configured agent (resolved like aux_router does), bypassing
    * role-based routing — so a fan-out (e.g. the MoA ensemble) reaches N
    * distinct agents instead of N copies of the one default agent. When NULL,
    * routing is unchanged (role-based default route). */
   const char *agent;
   /* Run this task WITH tools (default 0 = the historical plain completion, so no
    * existing fan-out changes). Set by the review panel: a panelist holding only a
    * diff cannot check whether the change is reachable, or whether a new file is
    * product — the facts that decide if a change is real live outside the diff.
    * With tools the task's role picks the toolset (`review` -> the index-only
    * `review_indexed`: code_search / find_symbol / search_memory / search_docs);
    * write tools stay off regardless. */
   int use_tools;
   /* Require at least one tool call before accepting a text-only turn. Used by
    * evidence-gated reviews; ignored when use_tools is false. */
   int require_initial_tool_call;
} agent_task_t;

typedef struct
{
   char agent_name[MAX_AGENT_NAME];
   /* The model actually served to the provider for this call. Distinct from
    * agent_name (the configured agent identity, e.g. "codex") because an agent
    * may serve a different model id (e.g. "gpt-5.4"), and a turn-0 400 may swap
    * in the agent's fallback_model. Used for cost estimation and the token_audit
    * model column; empty falls back to agent_name. */
   char model[MAX_MODEL_LEN];
   /* The model aimee SELECTED to serve (the agent's configured/fallback model),
    * set at execution entry and never overwritten by the provider-reported model.
    * Lets the audit distinguish what aimee served from what the provider echoed
    * in `model`. Empty falls back to `model` in the audit. */
   char served_model[MAX_MODEL_LEN];
   /* The model the client requested, when it differs from the served `model`
    * (ingress: e.g. Claude Code asks for a model, aimee serves its primary).
    * Empty for internal agent calls. Recorded separately in the audit. */
   char requested_model[MAX_MODEL_LEN];
   /* Provider stop/finish reason for the turn (e.g. "end_turn", "tool_use",
    * "stop", "length"), when the parser captured it. Empty otherwise. */
   char stop_reason[32];
   char *response;
   int prompt_tokens;
   int completion_tokens;
   int cache_write_tokens; /* Anthropic: cache_creation_input_tokens */
   int cache_read_tokens;  /* Anthropic: cache_read_input_tokens */
   int latency_ms;
   int success;
   char error[512];
   int turns;
   int tool_calls;
   int successful_tool_calls; /* executed calls with a non-empty, non-error result */
   int rescue_recoveries;
   int confidence;
   int abstained;
   char abstain_reason[512];
} agent_result_t;

/* Structured outcome classification for agent executions */
typedef enum
{
   OUTCOME_SUCCESS = 0,
   OUTCOME_PARTIAL,
   OUTCOME_FAILURE,
   OUTCOME_ERROR
} outcome_type_t;

typedef struct
{
   outcome_type_t outcome;
   char reason[256];
   int turns_used;
   int tools_called;
   int64_t tokens_used;
   char tool_error_pattern[128]; /* repeated tool+error key for anti-pattern extraction */
} agent_outcome_t;

typedef struct
{
   char name[MAX_AGENT_NAME];
   int total_calls;
   int total_prompt_tokens;
   int total_completion_tokens;
   int total_cache_write_tokens;
   int total_cache_read_tokens;
   double total_estimated_cost_usd;
   int avg_latency_ms;
   double success_rate;
} agent_stats_t;

/* Task type classification for context assembly */
typedef enum
{
   TASK_TYPE_GENERAL = 0,
   TASK_TYPE_BUG_FIX,
   TASK_TYPE_REFACTOR,
   TASK_TYPE_FEATURE,
   TASK_TYPE_REVIEW,
   TASK_TYPE_TEST,
   TASK_TYPE_COUNT
} task_type_t;

/* Context category weights per task type */
typedef enum
{
   CTX_WEIGHT_LOW = 10,
   CTX_WEIGHT_MED = 25,
   CTX_WEIGHT_HIGH = 40
} ctx_weight_t;

#endif /* DEC_AGENT_TYPES_H */
