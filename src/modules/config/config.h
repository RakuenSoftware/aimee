#ifndef DEC_CONFIG_H
#define DEC_CONFIG_H 1

#include "sandbox.h"
#include "prompts.h" /* aimee_mode_t */
#include <stdint.h>  /* int64_t (used below) — keep config.h self-contained so any
                      * includer (e.g. cli_remote.c on MinGW) compiles regardless of
                      * include order. */
#include <stdlib.h>
#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#endif

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 4096
#endif

/* Default iteration limits for chat loops (tool-call rounds per user message) */
#define CONFIG_DEFAULT_MAX_ITERATIONS          15
#define CONFIG_DEFAULT_MAX_ITERATIONS_DELEGATE 25

/* Default delegation depth/spawn limits */
#define CONFIG_DEFAULT_MAX_DELEGATION_DEPTH  3
#define CONFIG_DEFAULT_MAX_DELEGATION_SPAWNS 50

/* Server execution pool defaults */
#define CONFIG_DEFAULT_BACKGROUND_THREADS 2
#define CONFIG_DEFAULT_SESSION_THREADS    4
/* Raised from 2 -> 4 now that DB2 connections are bounded by the connection pool
 * (db2_connection_pool_size), not 1:1 with worker threads. */
#define CONFIG_DEFAULT_KB_WORKER_THREADS 4
/* DB2 connection pool: max reusable Postgres connections leased by worker
 * threads (lazy-opened on demand). Keep well under Postgres max_connections. */
#define CONFIG_DEFAULT_DB2_POOL_SIZE 16
/* Backstop ceiling on concurrent on-demand (I/O-bound) delegates. Delegates are
 * gated by the per-model concurrency limiter, not a CPU pool; this only guards
 * against pathological fan-out exhausting fds/memory. */
#define CONFIG_DEFAULT_DELEGATE_MAX_INFLIGHT           512
#define CONFIG_DEFAULT_CONCURRENCY_PREEMPT_REQUEUE_MAX 1

/* Default render backend for the #4-full computed-style oracle: curl the
 * conventional css-render sidecar (deploy/css-render, reachable as
 * `aimee-css-render:8780` on the shared container network). Inert when the
 * sidecar is down (oracle = UNAVAILABLE); set css_render_command empty to off. */
#define CONFIG_DEFAULT_CSS_RENDER_COMMAND                                                          \
   "curl -s --max-time 30 --data-binary @- http://aimee-css-render:8780/render"

/* Default TCTI for the tpm2 custody provider (vault.tpm2.tcti): the in-kernel
 * TPM resource manager device. The swtpm integration CT overrides this to
 * "swtpm:host=127.0.0.1,port=2321". */
#define CONFIG_DEFAULT_VAULT_TPM2_TCTI "device:/dev/tpmrm0"

/* Default NV index for the tpm2 anti-rollback monotonic counter (vault.tpm2.nv_index,
 * P7-tpm2b). A big-endian hex/decimal handle in the TPM2 NV space (0x01xxxxxx range).
 * Parsed by the WITH_TPM2 build via strtoul(base 0). */
#define CONFIG_DEFAULT_VAULT_TPM2_NV_INDEX "0x01500001"

/* Concurrency config: per-model and per-provider overrides */
#define CONFIG_CONCURRENCY_KEY_LEN     128
#define CONFIG_CONCURRENCY_MAX_ENTRIES 16

/* Max additional bearers (beyond the primary) a deployment may accept at once. */
/* Capacity of the ensemble reference arrays. Exported as a named constant so a
 * consumer can check its own limit against config's WITHOUT naming config_t:
 * delegate_ensemble.c used to assert on sizeof(((config_t *)0)->...), which made
 * the struct's layout part of its interface just to catch dimension drift. */
#define CONFIG_ENSEMBLE_MAX_REFS 32

#define AIMEE_API_BEARER_EXTRA_MAX 7

typedef struct
{
   char key[CONFIG_CONCURRENCY_KEY_LEN];
   int limit;
} config_concurrency_entry_t;

extern __thread int g_aimee_compute_threads_override;

static inline int aimee_default_compute_threads(void)
{
   return CONFIG_DEFAULT_BACKGROUND_THREADS;
}

static inline int aimee_default_session_threads(void)
{
   return CONFIG_DEFAULT_SESSION_THREADS;
}

static inline int aimee_resolve_compute_threads(int configured)
{
   if (g_aimee_compute_threads_override > 0)
      return g_aimee_compute_threads_override;
   const char *env = getenv("AIMEE_BACKGROUND_THREADS");
   if (!env || !*env)
      env = getenv("AIMEE_COMPUTE_THREADS");
   if (env && *env)
   {
      char *end = NULL;
      long value = strtol(env, &end, 10);
      if (end && *end == '\0' && value > 0)
         return (int)value;
   }
   return configured > 0 ? configured : aimee_default_compute_threads();
}

static inline int aimee_resolve_session_threads(int configured)
{
   const char *env = getenv("AIMEE_SESSION_THREADS");
   if (env && *env)
   {
      char *end = NULL;
      long value = strtol(env, &end, 10);
      if (end && *end == '\0' && value > 0)
         return (int)value;
   }
   return configured > 0 ? configured : aimee_default_session_threads();
}

static inline int aimee_resolve_delegate_max_inflight(int configured)
{
   const char *env = getenv("AIMEE_DELEGATE_MAX_INFLIGHT");
   if (env && *env)
   {
      char *end = NULL;
      long value = strtol(env, &end, 10);
      if (end && *end == '\0' && value > 0)
         return (int)value;
   }
   return configured > 0 ? configured : CONFIG_DEFAULT_DELEGATE_MAX_INFLIGHT;
}

static inline int aimee_resolve_db2_pool_size(int configured)
{
   const char *env = getenv("AIMEE_DB2_POOL_SIZE");
   if (env && *env)
   {
      char *end = NULL;
      long value = strtol(env, &end, 10);
      if (end && *end == '\0' && value > 0)
         return (int)value;
   }
   return configured > 0 ? configured : CONFIG_DEFAULT_DB2_POOL_SIZE;
}

#define CONFIG_MCP_MAX_CLIENTS          8
#define CONFIG_MCP_MAX_COMMAND_ARGS     16
#define CONFIG_MCP_MAX_CWD              512
#define CONFIG_MCP_OSV_MAX_ALLOW        16
#define CONFIG_COMPUTER_USE_MAX_DOMAINS 16

typedef enum
{
   CONFIG_MCP_TRANSPORT_NONE = 0,
   CONFIG_MCP_TRANSPORT_STDIO = 1,
   CONFIG_MCP_TRANSPORT_SSE = 2
} config_mcp_transport_t;

/* Which daemon hosts (runs) this MCP plugin, deciding its exposure scope:
 *   SERVER — booted by aimee-server; the plugin's tools are exposed only to that
 *            server's own sessions (the historical, default behavior).
 *   KB     — booted by aimee-kb; the plugin's tools are exposed to everything
 *            hooked up to that kb (every server + thin client), reached from a
 *            server over kb_client HTTP. */
typedef enum
{
   CONFIG_MCP_INSTALL_SERVER = 0,
   CONFIG_MCP_INSTALL_KB = 1
} config_mcp_install_t;

typedef struct
{
   char name[64];
   config_mcp_transport_t transport;
   config_mcp_install_t install; /* which daemon runs it (scope); default SERVER */
   char command[CONFIG_MCP_MAX_COMMAND_ARGS][256];
   int command_count;
   char cwd[CONFIG_MCP_MAX_CWD];
   char url[512];
   char bearer_token_env[128];
} config_mcp_client_t;

#define CONFIG_MAX_DISPOSITIONS     8
#define CONFIG_DISPOSITION_NAME_LEN 32

/* Charter: operator-authored, immutable-at-runtime identity layer.
 * Four structured string arrays (safety axioms, hard constraints,
 * values, tone boundaries) plus a drift-limit scalar for the working
 * profile. Arrays are loaded at config-parse time and are not mutated
 * by any runtime path — the only way to change the charter is to edit
 * aimee.yaml. */
#define CONFIG_CHARTER_MAX_ENTRIES 16
#define CONFIG_CHARTER_ENTRY_LEN   256

/* Working-profile injection allow list. The working-profile layer
 * commits its state from autoobserved feedback; this flag gates
 * whether those committed values reach the system prompt. Default 0
 * (off); enable per-field after validating quality. */
#define CONFIG_WORKING_PROFILE_ALLOW_MAX 8
#define CONFIG_WORKING_PROFILE_FIELD_LEN 64

typedef enum
{
   CONFIG_DISPOSITION_SOURCE_NONE = 0,
   CONFIG_DISPOSITION_SOURCE_GLOBAL,
   CONFIG_DISPOSITION_SOURCE_WORKSPACE,
   CONFIG_DISPOSITION_SOURCE_PROJECT
} config_disposition_source_t;

typedef struct
{
   char name[CONFIG_DISPOSITION_NAME_LEN];
   double value;
   config_disposition_source_t source;
} config_disposition_t;

/* One auxiliary-model task route. Named (it was an anonymous inline struct) so a
 * caller can be handed one element without holding a config_t -- see
 * config_aux_task_at. */
#define CONFIG_AUX_TASK_NAME_LEN 64
typedef struct
{
   char task[CONFIG_AUX_TASK_NAME_LEN];
   char provider[64];
   char model[128];
   int max_tokens;
} config_aux_task_t;

/* Trigger rule (from trigger_rules YAML list) */
#define TRIGGER_RULE_MAX_SOURCE   64
#define TRIGGER_RULE_MAX_EVENT    256
#define TRIGGER_RULE_MAX_SCHEDULE 64
#define TRIGGER_RULE_MAX_TEMPLATE 128
#define TRIGGER_RULE_MAX_WS       256
#define TRIGGER_RULE_MAX_MODE     32
#define TRIGGER_RULES_MAX         32

typedef struct
{
   char source[TRIGGER_RULE_MAX_SOURCE];     /* "github-webhook", "ci-webhook", "cron" */
   char event[TRIGGER_RULE_MAX_EVENT];       /* event pattern to match */
   char schedule[TRIGGER_RULE_MAX_SCHEDULE]; /* cron expression (source=cron only) */
   char pipeline_template[TRIGGER_RULE_MAX_TEMPLATE];
   char workspace[TRIGGER_RULE_MAX_WS];
   char mode[TRIGGER_RULE_MAX_MODE]; /* work-item mode the rule files: "autonomous"
                                      * (default — the run advances hands-off) or
                                      * "interactive" (parks for a human to drive in
                                      * the webchat). Empty => "autonomous". */
   double max_spend_usd;
} trigger_rule_t;

/* Cron job config (from cron_jobs YAML list). This is the typed schema for the
 * richer watchdog/llm/hybrid jobs; runtime dispatch can consume it incrementally
 * without overloading legacy trigger_rules. */
#define CRON_JOB_MAX_ID             64
#define CRON_JOB_MAX_SCHEDULE       64
#define CRON_JOB_MAX_MODE           16
#define CRON_JOB_MAX_SCRIPT         2048
#define CRON_JOB_MAX_PROMPT         4096
#define CRON_JOB_MAX_WORKDIR        256
#define CRON_JOB_MAX_CONTEXT_FROM   64
#define CRON_JOB_MAX_WHEN_CONTEXT   256
#define CRON_JOB_MAX_SKILLS         8
#define CRON_JOB_MAX_SKILL_NAME     64
#define CRON_JOB_MAX_DELIVER_TARGET 256
#define CRON_JOBS_MAX               32

typedef struct
{
   char id[CRON_JOB_MAX_ID];
   char schedule[CRON_JOB_MAX_SCHEDULE];
   char mode[CRON_JOB_MAX_MODE]; /* llm | script | hybrid */
   char script[CRON_JOB_MAX_SCRIPT];
   char prompt[CRON_JOB_MAX_PROMPT];
   char workdir[CRON_JOB_MAX_WORKDIR];
   char context_from[CRON_JOB_MAX_CONTEXT_FROM];
   char when_context_contains[CRON_JOB_MAX_WHEN_CONTEXT];
   char skills[CRON_JOB_MAX_SKILLS][CRON_JOB_MAX_SKILL_NAME];
   int skill_count;
   char deliver_target[CRON_JOB_MAX_DELIVER_TARGET];
   int deliver_only_if_changed;
   int deliver_first_run_silent;
   int pre_wake_gate;
   int enabled;
} cron_job_t;

/* Database connection settings for the explicit two-store architecture:
 * DB1 = local user store (sqlite), DB2 = shared knowledge store (postgres
 * + pgvector). See docs/STORAGE_TIERS.md. (The vector tier was folded
 * into DB2 as pgvector in #1575.) */
#define CONFIG_DB2_URL_LEN 512

typedef struct config
{
   char db1_path[MAX_PATH_LEN];
   char db2_url[CONFIG_DB2_URL_LEN];  /* DB2 connection URL */
   int db2_pool_size;                 /* DB2 connection pool cap; default 8 */
   char workspaces[64][MAX_PATH_LEN]; /* workspace root directories (absolute at runtime) */
   /* Resource provider backing each workspace, indexed alongside workspaces[]
    * ("" == the default `shared` co-located provider). Set at registration
    * (workspace-resource-plane §1); read by turn setup to bind the active
    * provider. Persisted as a {path, provider} object only when non-default. */
   char workspace_providers[64][16];
   /* For a `mirror`-provider workspace (workspace-resource-plane §3) the registry
    * also carries the client's VCS coordinates so the detached session setup can
    * drive the mirror lifecycle (seed bare mirror from remote @ head, reconstruct
    * a server-side worktree) automatically. Empty for shared/detached entries.
    * Indexed alongside workspaces[]; persisted in the {path,provider,...} object. */
   char workspace_vcs_remote[64][512];
   char workspace_vcs_head[64][65]; /* SHA-1/SHA-256 object id + NUL */
   /* Per-workspace delegate-sandbox image override ("" == none; fall through to the
    * global delegate_sandbox_image, then the backend default). Indexed alongside
    * workspaces[]; persisted in the {path,provider,...,sandbox_image} object. The
    * richer build-from-spec form lives in the repo's .aimee/project.yaml. */
   char workspace_sandbox_image[64][256];
   int workspace_count;
   char guardrail_mode[16];
   int subagent_ban_enabled; /* default ON: when set AND usable delegates are configured,
                                block the primary agent's own sub-agent tools (Task/Agent/
                                spawn_agent/RemoteTrigger) and redirect to `aimee delegate`.
                                Explicit `subagent_ban_enabled: false` allows provider-native
                                sub-agents. */
   char provider[16];
   /* Durable default persona: the persona a fresh primary session starts as, and
    * the persona draft roundtable panelists author with when none is set. Width =
    * PERSONA_NAME_MAX (persona.h). Defaults to "engineer"; settable in the GUI. */
   char default_persona[64];

   /* Claude primary CLI model override (enforced via --model flag on launch) */
   char claude_model[128]; /* e.g. "claude-opus-4-6" — empty means use CLI default */

   /* Codex primary CLI model override (enforced via per-turn model override) */
   char codex_model[128]; /* e.g. "gpt-5.4" — empty means use CLI default */

   /* Primary CLI reasoning effort override shared by providers that support it. */
   char model_reasoning_effort[32]; /* low/medium/high/xhigh; empty means provider default */

   /* OpenAI-compatible primary CLI settings */
   char openai_endpoint[512]; /* e.g. "https://api.openai.com/v1" */
   char openai_model[128];    /* e.g. "gpt-4o" */
   char openai_key_cmd[512];  /* command that prints the API key */

   /* Embedding command: piped text on stdin, returns JSON float array on stdout */
   /* Embedder. Empty embedder_url means the in-container embedder baked into
    * this image variant (bekko-a25m 384, or nomic-v2 768); a non-empty URL is an
    * operator-run endpoint, whose width may be anything up to EMBED_MAX_DIM —
    * 4000, which is the DB2 column ceiling, not 4096.
    *
    * embedder_dims is a ONE-WAY DOOR once anything has been embedded: DB2
    * records the vector-column width and refuses startup on drift. */
   char embedder_command[512];
   char embedder_model[128];
   char embedder_url[512];
   char embedder_api_key[256];
   int embedder_dims;
   char memory_weight_profile[512];
   char memory_rerank_mode[16];
   int memory_maintenance_trigger_inserts;
   int memory_maintenance_trigger_secs;
   /* Scheduled maintenance (memory.maintenance.*):
    * memory_maintenance_enabled: 0 = scheduler off (default; on-demand
    *   `aimee memory maintain` still works).  1 = maybe_run fires a
    *   cycle when the interval has elapsed.
    * memory_maintenance_interval_seconds: cadence between cycles.  0
    *   falls back to MEMORY_MAINTENANCE_DEFAULT_INTERVAL_SECS (900).
    * memory_maintenance_summarize_enabled: opt-in gate on the
    *   summarize mode (the only path that may call an LLM).  Default 0. */
   int memory_maintenance_enabled;
   int memory_maintenance_interval_seconds;
   int memory_maintenance_summarize_enabled;
   /* css_style_graph_enabled: gate (default 1, on) for the CSS migration
    * assistant's style-graph write path during indexing (WP-C). When off, the
    * indexer keeps only the legacy lexical CSS class-name scan. */
   int css_style_graph_enabled;
   /* code_cochange_git_enabled: gate (default 1, on) for mining git history at
    * `index scan` time to accumulate co_edited edges (files that change
    * together in a commit) into the entity graph that index_blast_radius
    * already reads. Incremental and idempotent via a per-project HEAD marker in
    * kb_runtime_state; bulk commits (> 25 code files) are skipped. Set false to
    * keep co_edited edges session-derived only. */
   int code_cochange_git_enabled;
   /* wfe_live_forge_enabled: gate (default 0, OFF — 2026-07-17: opt-in while the
    * autonomous pipeline is under test) for the autonomous live forge
    * (full-autonomous-development F4). When OFF the wfe forge provider is not
    * registered and every forge op fails closed, so an autonomous run can never
    * open or merge a real PR (it parks). When ON, every op still re-checks this
    * flag AND the merge-target rail: PRs open-only against the resolved trunk,
    * merges only to the unprotected autonomous base. Set true to opt in. */
   int wfe_live_forge_enabled;
   /* wfe_proposals_autoscan_enabled: gate (default 0, OFF — 2026-07-17) for the
    * proposals/watch-dir trigger source's AUTOMATIC every-tick scan that files a
    * WFE work item for every un-filed pending proposal. When OFF, proposals are
    * NEVER filed automatically; a human files them one at a time via
    * `trigger.fire source=proposals proposal=<name>`. Set true to opt in to the
    * autonomous scan once the pipeline is trusted. */
   int wfe_proposals_autoscan_enabled;
   /* client_integrations_enabled: gate (default 1, ON) for the automatic
    * registration of aimee into external AI-tool user configs — the Claude Code
    * MCP server/hooks/env/commands in ~/.claude, plus Gemini/Copilot/Codex. When
    * OFF, ensure_client_integrations() is a no-op so aimee never writes itself
    * into a tool's global config; wire aimee into a single project manually
    * instead. The env var AIMEE_NO_CLIENT_INTEGRATIONS overrides this at runtime.
    * Set false to opt out. */
   int client_integrations_enabled;
   /* audit_action_enabled: emit a per-tool-call governed-action row (kind=
    * tool_action) to audit.log from pre_tool_check. Default-ON (the
    * trajectory_export reader shipped, so the rows are consumable); audit is
    * passive, fail-open, and never changes an enforcement verdict. Set false to
    * opt out. */
   int audit_action_enabled;
   /* audit_worm_enabled: dual-write each governed-action audit row into the
    * per-service WORM store (append-only, hash-chained SQLite) alongside the
    * legacy audit.log. Default-OFF (S0 of the WORM audit-store proposal); the
    * WORM store is not yet authoritative, so a failed WORM append is recoverable
    * audit loss, never an enforcement change. Persisted as an opt-in (config.c
    * load + config_save), so an enabled deployment survives a restart; the global
    * default stays OFF. */
   int audit_worm_enabled;
   /* css_render_command: the render backend for the #4-full computed-style oracle.
    * A shell command (like embedding_command) that reads a {"html","css"} JSON
    * object on stdin and writes a computed-style snapshot JSON on stdout. It runs
    * a headless browser over UNTRUSTED markup, so it must be an isolated,
    * out-of-process backend (e.g. `curl` to a sandboxed render sidecar). Defaults
    * to the conventional css-render sidecar (CONFIG_DEFAULT_CSS_RENDER_COMMAND) so
    * render-capture works the moment the sidecar is up (on-demand); inert when it
    * is down (the oracle reports UNAVAILABLE, never a fake verdict). Set empty to
    * disable. */
   char css_render_command[512];
   /* vault.custody: which custody provider anchors the vault's server KEK (P10/P7
    * slice 3b). One of {file,mock,tpm2,pkcs11,kms}. `file` (default) self-unseals
    * from a 0600 master-key file (today's low-ops behavior). `mock` is a test/dev
    * anchor exercising the seal barrier. tpm2/pkcs11/kms are declared but not yet
    * implemented (they fail closed at kb bind). Read once at kb startup by
    * kb_vault_policy_select; the server profile always runs `file`. */
   char vault_custody[16];
   /* vault.tpm2.blob_path: filesystem path to the sealed KEK blob (TPM2B_PUBLIC +
    * TPM2B_PRIVATE, marshaled) for the tpm2 custody provider (P7-tpm2a). Empty
    * (default) resolves to <config_default_dir>/vault/tpm2-kek.blob at use. Only
    * read by the WITH_TPM2 build of vault_custody_tpm2.c; inert otherwise. */
   char vault_tpm2_blob_path[512];
   /* vault.tpm2.tcti: the tss2 TCTI connection string passed to
    * Tss2_TctiLdr_Initialize (default "device:/dev/tpmrm0"; the swtpm CT sets
    * "swtpm:host=127.0.0.1,port=2321"). Only read by the WITH_TPM2 build. */
   char vault_tpm2_tcti[128];
   /* vault.tpm2.nv_index: the TPM2 NV monotonic-counter index anchoring P7-tpm2b
    * anti-rollback (default "0x01500001"). A hex/decimal handle string parsed via
    * strtoul(base 0). Only read by the WITH_TPM2 build of vault_custody_tpm2.c. */
   char vault_tpm2_nv_index[32];
   int memory_salience_enabled;
   double memory_salience_weight;
   int memory_salience_window_size;
   int memory_surprise_enabled;
   double memory_surprise_weight;
   char memory_coref_mode[16];
   int memory_coref_window;
   int memory_cognify_async_enabled;

   /* LLM-driven cognification: extract typed triples, claims, and observations from
    * raw memory units. memory_cognify_enabled: 0 = disabled (default), 1 = enabled.
    * memory_cognify_model: model alias for the cognifier (default "haiku").
    * memory_cognify_command: external command that performs cognification (stdin: JSON
    *   {unit_id, text}, stdout: JSON cognification result). Empty = disabled. */
   int memory_cognify_enabled;
   char memory_cognify_model[64];
   char memory_cognify_command[512];

   int memory_context_budget_enabled; /* 0=top-K assembly (default), 1=token-budget assembly */
   int memory_context_budget_tokens;  /* budget in tokens; 0=use default (2048) */
   int memory_routing_enabled;        /* 1=adaptive route selection (default), 0=hybrid route mix */

   /* Context pre-injection for the model ingresses (Codex/OpenAI). When on, the
    * ingress prepends a fusion-recall <aimee-context> envelope to the request
    * system prompt so the external agent stops re-exploring the repo. Default
    * off; a per-request `x-aimee-preinject: 0` header also disables it. */
   int ingress_preinject_enabled;
   /* Task-conditioned code packet rollout: off preserves the legacy preview,
    * observe retrieves/audits without changing model-visible bytes, and on
    * replaces the legacy preview with the bounded answerable packet. E4 ships
    * observe; E6 owns any promotion to on. */
   char code_context_mode[16];
   /* ingress-compression P5 (§2.3), default off: inject the <aimee-context>
    * envelope on the Anthropic-native /v1/messages passthrough (otherwise
    * parity-skipped to preserve the client's cached prefix). Separate from the
    * OpenAI/Codex seam's ingress_preinject_enabled. */
   int ingress_preinject_anthropic_enabled;
   int ingress_preinject_assembly_budget;
   int ingress_max_raw_scans;
   /* ingress-compression P2 (§6.5 B4): max line span the code_span_get recovery
    * resolver will return in one call. Bounds the per-call recovery cost and a
    * model-supplied range. */
   int code_span_max_lines;
   /* Operator cap (bytes) on the per-result MODEL-VISIBLE tool output (read_file,
    * bash, grep, glob, git_* results). 0 = use the built-in default
    * AGENT_TOOL_OUTPUT_MAX (32768). Any positive value is clamped to
    * (0, AGENT_TOOL_OUTPUT_RAW_MAX=32768]; set it LOWER to bound the bytes a
    * single tool result contributes to the prompt + history. Resolved by
    * agent_tool_output_cap(); never exceeds the 32 KB raw capture buffer. */
   int tool_output_max_bytes;
   /* ingress-compression master gate (§6.5 B8), default off. When on, code-search
    * hits are span-enriched (the matched line is located) and the ingress envelope
    * folds code entries into recoverable references; off keeps the search query,
    * cost, and envelope byte-identical. */
   int ingress_compress_enabled;
   /* ingress-compression P1b: a code snippet is folded to a `file:line` reference
    * only when its snippet exceeds this many chars (folding a tiny snippet saves
    * nothing). Default 80. */
   int ingress_compress_min_chars;
   /* ingress-compression P3 (§2 placement invariant), default off. When on, the
    * volatile <aimee-context> envelope is appended AFTER the stable instructions
    * prefix (instead of prepended) on the OpenAI/Codex Responses path, so the
    * provider's automatic prefix cache is not invalidated each turn. */
   int ingress_cache_placement_enabled;
   /* Delegation available at all. Default 1. When 0, the delegate tools are
    * withheld from the tool surface AND the manager persona block is omitted --
    * that block exists to direct work to delegates, so it is noise without them.
    * Advertising delegation on a surface that cannot perform it costs tokens on
    * every request and was measured doing exactly nothing: zero delegate calls
    * across a whole benchmark corpus whose prompt said "ALWAYS delegate". */
   int delegates_enabled;
   /* Emit the manager persona block. Default 1. Independent off switch for
    * callers that keep delegation but want the shorter prompt. */
   int prompt_manager_block_enabled;
   /* Emit the roundtable-review mandate inside the manager block. Default 1.
    * Separable because it is obeyed and each review is a full round trip: on
    * small bounded tasks that is pure overhead, which is why benchmarks turn it
    * off while production keeps it. */
   int prompt_manager_review_enabled;
   /* Session-isolation guard (opt-in): when on, the PreToolUse attention-guard
    * fails closed on a mutating tool whose target is NOT inside an aimee-managed
    * worktree (.aimee/worktrees/...), forcing every mutating session into an
    * isolated worktree+branch off the default branch. Default 0 (off). */
   int require_session_worktree;
   /* session_worktree_base: what a NEW primary session's branch+worktree is cut from.
    * Resolution order: CONFIGURED -> remote DEFAULT -> main -> master.
    *   "remote_default" (DEFAULT) — the remote's advertised default (origin/HEAD),
    *                     fetched fresh; then main, then master.
    *   "local_default"  — the local copy of the default branch; then main, then master.
    *   "current"        — whatever the source checkout has checked out. Reachable ONLY
    *                     by explicit opt-in, never as a fallback.
    *   "<ref>"          — an explicit ref (e.g. "origin/main", a tag, a SHA); verified
    *                     to exist, and an error if it does not.
    * Each fallback prefers origin/<name> and accepts the local branch only when no
    * remote-tracking ref exists, so a repo WITH a remote never starts from a stale local.
    *
    * The current branch is deliberately NOT in the chain. It used to be the last resort,
    * which meant a new session was cut from whatever the shared checkout happened to be
    * sitting on -- silently inheriting another session's unmerged work. main and master
    * are dumb fallbacks, but they are stable.
    *
    * Env override: AIMEE_SESSION_WORKTREE_BASE. The attention guard independently
    * refuses a worktree whose recorded base is neither the default branch nor a
    * registered parent, so loosening this does not disable that check. */
   char session_worktree_base[64];
   /* External-memory guard (default on): the PreToolUse attention-guard blocks
    * agent writes to external file-based agent-memory stores
    * (~/.claude/projects/<slug>/memory/...), redirecting durable memories into
    * aimee's memory system (`aimee memory store`). Explicit false opts out. */
   int require_aimee_memory;
   /* Forge-tooling guard (default on): a delegate may not run `git` or `gh` in a
    * shell — every git/forge action goes through aimee's git_* tools and executes
    * on aimee-server, where the forge credential lives and never reaches a child's
    * environment or argv. Reads are blocked too (git_status / git_log /
    * git_diff_summary / git_pr action=view cover them), so the rule is simply "no
    * git or gh in a shell" with no verb list to drift. Explicit false opts out.
    *
    * Two layers, neither of which is a proof: the classifier
    * (wfe_shell_invokes_git) is a string match a determined agent can evade, and
    * the env-strip at spawn (delegate_child_strip_forge_creds) removes the
    * credentials we know how to name. Together they raise the cost sharply; they do
    * not constitute a credential boundary.
    *
    * SIDE EFFECT worth knowing before enabling: the env-strip also drops
    * SSH_AUTH_SOCK, so a delegate cannot use agent-backed SSH to ANY host — not
    * just the forge. It also points GIT_CONFIG_GLOBAL/SYSTEM at /dev/null, so a
    * delegate sees no global git config (including user.name/user.email; commits go
    * through git_commit on the server, which supplies its own identity). */
   int require_aimee_git;

   /* `delegate_sandbox` was removed: a delegate runs in a container or not at all,
    * so there is no longer a second execution model for it to select. The key is
    * still accepted by the schema and warned about at load, so an existing config
    * that sets it does not become an "unknown key". */

   /* Global default delegate-sandbox image ("" == the backend default, ubuntu:22.04).
    * Lowest-precedence image source: a repo's .aimee/project.yaml `sandbox` block and
    * a per-workspace `sandbox_image` override both win over this. A bare image
    * reference (used as-is); the build-from-spec form lives in .aimee/project.yaml. */
   char delegate_sandbox_image[256];

   /* Runtime package-access policy for a `--network none` delegate sandbox. aimee
    * always performs the fetch (the delegate never holds an outside socket) and logs
    * it; this selects HOW MUCH aimee will fetch on the delegate's behalf:
    *   "proxy"      (default) proxy package-manager fetches to any host
    *   "off"        no runtime proxy; build-time installs + learned pre-bake only
    *   "gated"      host-allowlisted registries; off-allowlist -> human approval
    *   "governance" allowlist from a governance provider; off-allowlist refused
    * See docs/proposals/pending/delegate-sandbox-image-customization.md. */
   char delegate_sandbox_package_access[32];

   /* delegate_sandbox_require_isolation: fail-closed guard (default 0, OFF) for the
    * `--network none` sandbox, meaningful only when delegate_sandbox is ON. aimee always
    * passes --network none, but some container runtimes (e.g. SmoothNAS/tierd, which
    * attaches a "primary" veth that cannot be disconnected) ignore it and give the
    * sandbox real egress — silently defeating the package-access proxy/allowlist. After
    * the container starts aimee asks the host daemon whether a network with an IP is
    * attached; on a breach it ALWAYS logs an error. When this flag is set, sandboxing is
    * MANDATORY: rather than fall back to un-isolated in-process host execution, aimee
    * refuses to run the delegate at all on ANY failure to achieve an isolated sandbox —
    * a detected breach, an unverifiable probe, the docker backend being unavailable, or
    * the container failing to acquire. Default OFF so a non-compliant runtime degrades
    * loudly instead of breaking every delegate; set true once the runtime honours
    * isolation. */
   int delegate_sandbox_require_isolation;

   /* delegate_sandbox_learn_packages: the "learned toolchain" (default 1, ON). When on,
    * aimee captures the apt packages a delegate installs inside its `--network none`
    * sandbox and records them per-project (git root), then pre-bakes the learned set into
    * that project's next sandbox image build — augmenting a declared .aimee/project.yaml
    * `from`+`packages` spec, or SYNTHESIZING one (FROM the resolved base + the learned
    * packages) when the project declares none. The tools are then present immediately with
    * no runtime fetch. Best-effort: a learned build that fails to build falls back to the
    * un-augmented image (the runtime package-access path still covers the delegate). Note
    * the first delegate turn after a new package is learned pays a one-time image build. */
   int delegate_sandbox_learn_packages;

   /* Gateway tool-policing (P2): when on, the gateway strips subagent-spawning
    * tools (Task/Agent/spawn_agent/RemoteTrigger) from the inbound `tools` of a
    * proxied /v1/messages or /v1/chat/completions request, so the served model is
    * never offered a way to spawn subagents. Default off. */
   int gateway_prevent_subagents;
   /* Gateway model-pin (universal-gateway P2b): when on, the proxied /v1/messages
    * served model is forced to the configured primary's model, overriding whatever
    * model the client requested. Default off (the passthrough honors the client
    * model, P1). Single-model Anthropic-compatible shims (llama.cpp/vLLM) enable
    * this so an arbitrary client model name is not forwarded and rejected upstream. */
   int gateway_pin_model;
   int typed_facts_enabled; /* typed-fact knowledge layer master gate (default off) */
   /* kb.typed_facts.* — KB-owned autonomous reconciliation knobs (proposal §7.2/§8). */
   int kb_typed_facts_auto_promote_enabled; /* default on: auto-promote recurrent provisional
                                               relations */
   int kb_typed_facts_promote_threshold;    /* observations before auto-promote (default 3) */
   /* structured-PDF pipeline preset: the everyday knob for the 5 kb_pdf_*_enabled
    * stage gates. "off" (default) = plain pdftotext, "basic" = ingest + vector,
    * "full" = every stage (ingest, vector, tsr, assets, ocr). Drives the gates at
    * config load; an explicit per-stage gate still overrides. Empty = "off". */
   char kb_pdf_tier[16];
   int kb_pdf_ingest_enabled;       /* structured-pdf: route PDF uploads through the geometry
                                       extractor (kb_doc_pdf) instead of plain pdftotext (default off) */
   int kb_pdf_vector_enabled;       /* structured-pdf Phase A: embed PDF chunks into the isolated
                                       kb_pdf_embeddings relation + add the vector leg to
                                       search_chunks (default off; degrades to lexical when absent) */
   int kb_pdf_tsr_enabled;          /* structured-pdf Phase B: run the TSR sidecar at ingest to turn
                                       table regions into kb_table_cells (default off; degrades to
                                       text-only + tsr_status='unavailable' when the sidecar absent) */
   char tsr_command[1024];          /* structured-pdf Phase B: TSR sidecar endpoint/command
                                       (resolves like embedding_command; AIMEE_TSR_URL env fallback) */
   int kb_pdf_assets_enabled;       /* structured-pdf Phase C: render figure/table crops to the
                                       content-addressed blob store + kb_doc_assets at ingest, served
                                       via open_asset (default off; needs pdftoppm) */
   char kb_pdf_blob_dir[1024];      /* structured-pdf Phase C: blob store root override (default
                                       <kb_default_config_dir()>/kb-blobs) */
   int kb_pdf_blob_recon_secs;      /* structured-pdf Phase C: orphan-blob reconciliation interval
                                       seconds (default 3600; <=0 disables the sweep) */
   int kb_pdf_blob_orphan_alarm_mb; /* structured-pdf Phase C: warn when reclaimable orphan bytes
                                       exceed this many MB (default 1024; <=0 disables the alarm) */
   int kb_pdf_ocr_enabled;       /* structured-pdf Phase D: OCR a scanned/no-text-layer PDF via the
                                    OCR sidecar at ingest (default off; without it a scanned PDF is
                                    asset-only) */
   char ocr_command[1024];       /* structured-pdf Phase D: OCR sidecar endpoint/command (resolves
                                    like embedding_command; AIMEE_OCR_URL env fallback) */
   int kb_evidence_emit_enabled; /* auditable-correctness: emit per-turn retrieval_event (default
                                    off) */
   int fidelity_check_enabled; /* auditable-correctness P3: run the fidelity judge on terminal-text
                                  turns (default off; fail-closed dep on
                                  kb_evidence_emit_enabled + ingress_preinject_enabled) */
   int memory_pagerank_enabled;
   int memory_pagerank_iterations;
   double memory_pagerank_weight;
   char memory_pagerank_relations[256];
   char memory_citations_mode[16];
   int memory_citations_reprompt_on_miss;
   int memory_citations_strip_unverified;

   /* Session briefing (memory.briefing.*): start-of-session context bundle.
    * memory_briefing_enabled: 0 = disabled (default), 1 = enabled. Gates the
    *   auto-inject path; the `aimee memory briefing` command and the
    *   memory_briefing MCP tool always work regardless.
    * memory_briefing_limit_tokens: approximate character budget for the
    *   rendered bundle; 0 = default (MEMORY_BRIEFING_DEFAULT_LIMIT_TOKENS). */
   int memory_briefing_enabled;
   int memory_briefing_limit_tokens;

   /* Aggregation-aware query routing (memory.aggregation.*): detects
    * "coverage" shaped queries ("list all X", "every Y", "what Xs has ENTITY
    * Ved") and bypasses the hybrid vector-search path for an index-backed
    * route that returns an unranked-by-similarity set up to max_items.
    * memory_aggregation_enabled: 0 = disabled (default, point-query behaviour
    *   unchanged), 1 = enabled.
    * memory_aggregation_max_items: hard cap on returned rows; 0 = default
    *   (MEMORY_AGGREGATION_DEFAULT_MAX_ITEMS). */
   int memory_aggregation_enabled;
   int memory_aggregation_max_items;

   /* Prospective memory (memory.prospective.*): "when X, surface Y" triggered
    * recall.  memory_prospective_enabled gates the pre-turn matcher hook; the
    * CLI / MCP CRUD surface is always available so reminders can be armed
    * before the matcher is flipped on.  memory_prospective_max_matches caps
    * the number of reminders the matcher returns per turn (default 3). */
   int memory_prospective_enabled;
   int memory_prospective_max_matches;

   /* Memory lifecycle (memory.lifecycle.*): explicit lifecycle state on
    * every row with a pending-TTL sweep and a memory_alerts bundle.
    * memory_lifecycle_enabled: 0 = default, all rows look `active` and the
    *   sweep never runs (byte-identical behaviour for operators who don't
    *   opt in); 1 = sweep on every maintenance cycle, commitment-shape
    *   detection tags future-tense agent statements as `pending`.
    * memory_lifecycle_hide_archived: 1 = filter `archived` rows from
    *   default recall (memory_find_facts etc.). 0 keeps them in default
    *   recall; either way memory_get() can still fetch them.
    * memory_lifecycle_ttl_*_days: override per-shape TTL horizons.
    *   Zero falls back to the MEMORY_LIFECYCLE_TTL_DEFAULT_*_DAYS header
    *   constants. */
   int memory_lifecycle_enabled;
   int memory_lifecycle_hide_archived;
   int memory_lifecycle_ttl_date_days;
   int memory_lifecycle_ttl_relative_days;
   int memory_lifecycle_ttl_open_ended_days;

   /* Proactive recall (memory.recall.*): injects a six-section
    * identity/preferences/active_context/open_commitments/reminders/directives
    * bundle into the agent's exec context before response generation.
    * memory_recall_enabled: 0 = off (default; the CLI + MCP tool still
    *   work); 1 = session-start auto-inject fires on every agent run.
    * memory_recall_limit_tokens_session / _turn: token budgets for the
    *   two injection modes. Zero falls back to
    *   MEMORY_RECALL_DEFAULT_LIMIT_TOKENS_SESSION / _TURN. */
   int memory_recall_enabled;
   int memory_recall_limit_tokens_session;
   int memory_recall_limit_tokens_turn;

   /* Epistemic directives (memory.directives.*): durable "we know this
    * gap exists, ask when relevant" records.
    * memory_directives_enabled: 0 = off (default; CLI + MCP still work,
    *   auto-creation hooks stay silent); 1 = contradiction + retrieval-
    *   failure hooks create directives and recall surfaces them.
    * memory_directives_failure_threshold: how many confident-retrieval
    *   failures against the same normalised query are needed before the
    *   retrieval_failure directive auto-creates. Default 3 (see
    *   MEMORY_DIRECTIVE_RETRIEVAL_FAILURE_THRESHOLD_DEFAULT).
    * memory_directives_max_matches: cap for topic-matcher results per
    *   turn. Default 2. */
   int memory_directives_enabled;
   int memory_directives_failure_threshold;
   int memory_directives_max_matches;

   config_disposition_t dispositions[CONFIG_MAX_DISPOSITIONS];
   int disposition_count;
   config_disposition_t disposition_globals[CONFIG_MAX_DISPOSITIONS];
   int disposition_global_count;
   config_disposition_t disposition_workspaces[CONFIG_MAX_DISPOSITIONS];
   int disposition_workspace_count;
   config_disposition_t disposition_projects[CONFIG_MAX_DISPOSITIONS];
   int disposition_project_count;

   /* Charter: operator-authored identity; see CONFIG_CHARTER_* above.
    * Arrays are `count` of populated `[entry_len]` strings. */
   char charter_safety_axioms[CONFIG_CHARTER_MAX_ENTRIES][CONFIG_CHARTER_ENTRY_LEN];
   int charter_safety_axioms_count;
   char charter_hard_constraints[CONFIG_CHARTER_MAX_ENTRIES][CONFIG_CHARTER_ENTRY_LEN];
   int charter_hard_constraints_count;
   char charter_values[CONFIG_CHARTER_MAX_ENTRIES][CONFIG_CHARTER_ENTRY_LEN];
   int charter_values_count;
   char charter_tone_boundaries[CONFIG_CHARTER_MAX_ENTRIES][CONFIG_CHARTER_ENTRY_LEN];
   int charter_tone_boundaries_count;
   /* Working-profile drift limit: reserved for a future working-profile
    * governance proposal. 0 means "no limit" / unset. */
   int charter_working_profile_drift_limit;

   /* Working-profile prompt injection.
    * identity_working_profile_injection_enabled defaults off; flipping
    * it on still respects the allow list, which is empty by default =
    * inject every canonical field. Operators typically enable one
    * field at a time during validation. */
   int identity_working_profile_injection_enabled;
   char identity_working_profile_injection_fields[CONFIG_WORKING_PROFILE_ALLOW_MAX]
                                                 [CONFIG_WORKING_PROFILE_FIELD_LEN];
   int identity_working_profile_injection_fields_count;

   /* Entity profile cards: per-entity structured summaries built by aggregation.
    * memory_profile_cards_enabled: 0 = disabled (default), 1 = enabled.
    * memory_profile_cards_min_obs: minimum observations to build a card (default 10).
    * memory_profile_cards_stale_secs: refresh after this many seconds (default 86400 = 1 day). */
   int memory_profile_cards_enabled;
   int memory_profile_cards_min_obs;
   int memory_profile_cards_stale_secs;

   /* HyDE and query decomposition: pre-retrieval query rewriting.
    * memory_rewrite_enabled: 0 = disabled (default), 1 = enabled.
    * memory_rewrite_hyde: 1 = generate hypothetical answer before embedding (default 0).
    * memory_rewrite_decompose: 1 = decompose compound queries into sub-questions (default 0).
    * memory_rewrite_max_subqueries: max sub-questions to generate (default 4).
    * memory_rewrite_command: external command (stdin: JSON {query}, stdout: JSON rewrite result).
    *   Output schema: {"hyde_answer":"...","sub_questions":["q1","q2"]}.
    *   Empty string = disabled even when enabled=1. */
   int memory_rewrite_enabled;
   int memory_rewrite_hyde;
   int memory_rewrite_decompose;
   int memory_rewrite_max_subqueries;
   char memory_rewrite_command[512];

   /* Invertible chunking and session window expansion.
    * memory_window_radius: how many turns before/after a conversational hit to
    *   include when it has a source_session (0 = off, default 0; 1–3 recommended). */
   int memory_window_radius;

   /* Upper bound on results returned by `aimee kb search` / kb_search().
    * Requests with --max N above this value are silently clamped. Default 50;
    * internal buffers allow up to MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS. */
   int kb_search_max_results;

   /* Negation and explicit-absence memory.
    * memory_negation_enabled: 0 = disabled, 1 = enabled (default; graduated
    *   from Bucket 3 after the negation A/B — see benchmarks/bucket3-defaults).
    *   When enabled, insert/update computes not_<token> synthetic terms and
    *   writes them to the negation_tokens column; negated queries also use
    *   negation lexical matching for negative facts. */
   int memory_negation_enabled;

   /* memory_query_expansion_mode: "lexical" (default) or "semantic" (embedding-based).
    * memory_query_expansion_k: number of near-neighbour terms to inject (default 5). */
   char memory_query_expansion_mode[16];
   int memory_query_expansion_k;

   /* Two-lane retrieval: separate per-lane top-K for summary-shaped vs atomic-fact evidence.
    * memory_recall_lanes_enabled: 0 = disabled (default), 1 = split recall into two lanes.
    * memory_recall_lanes_summary_kinds: comma-separated memory kinds for the summary lane
    *   (default "episode").
    * memory_recall_lanes_fact_kinds: comma-separated memory kinds for the atomic-fact lane
    *   (default "fact,preference").
    * memory_recall_lanes_k_summary / _k_fact: per-lane top-K (default 40 each).
    * memory_recall_lanes_floor_summary / _floor_fact: minimum survivors after post-rerank
    *   floor fill (default 4 each; 0 = no floor). */
   int memory_recall_lanes_enabled;
   char memory_recall_lanes_summary_kinds[256];
   char memory_recall_lanes_fact_kinds[256];
   int memory_recall_lanes_k_summary;
   int memory_recall_lanes_k_fact;
   int memory_recall_lanes_floor_summary;
   int memory_recall_lanes_floor_fact;

   /* Memory improve loop sub-pass flags.
    * memory_improve_dedupe_enabled: 0 = skip dedupe (default), 1 = merge duplicate keys.
    * memory_improve_summarise_enabled: 0 = skip summarise (default), 1 = collapse clusters.
    * memory_improve_min_cluster: minimum cluster size for summarise pass (default 3).
    * memory_improve_max_confidence: max confidence for a memory to be summarised (default 0.5). */
   int memory_improve_dedupe_enabled;
   int memory_improve_summarise_enabled;
   int memory_improve_min_cluster;
   double memory_improve_max_confidence;

   /* Per-session episode summary cards.
    * memory_episode_summaries_enabled: 0 = disabled (default), 1 = generate a structured
    * episode card when a session closes via the cognifier command. */
   int memory_episode_summaries_enabled;

   /* Scene clustering and two-stage retrieval.
    * memory_scenes_enabled: 0 = disabled (default), 1 = enabled.
    * memory_scenes_global_escape_ratio: fraction of results from global pool (default 0.2). */
   int memory_scenes_enabled;
   double memory_scenes_global_escape_ratio;

   /* Quantitative / date-arithmetic post-retrieval deriver.
    * memory_derive_facts_enabled: 0 = disabled (default), 1 = run the deriver
    * on quantitative and temporal-interval queries before answer generation. */
   int memory_derive_facts_enabled;

   /* Retrieval-failure detection and recovery.
    * memory_failure_detection_enabled: 0 = disabled (default), 1 = enabled.
    *   When enabled, computes a coverage/separation confidence score after
    *   retrieval.  If the score is below the threshold, a wider re-fetch is
    *   attempted.  If confidence remains low, a LOW marker is injected into
    *   the assembled context so the answering LLM knows to abstain rather
    *   than speculate.
    * memory_failure_detection_threshold: confidence threshold (default 0.35).
    *   Queries scoring below this value trigger the fallback and LOW marker. */
   int memory_failure_detection_enabled;
   double memory_failure_detection_threshold;

   /* Retrieval answerability gate.
    * memory_abstain_enabled: 0 = disabled (default), 1 = refuse weak evidence.
    * memory_abstain_gate: effective default 0.40 at use site when enabled.
    * memory_chunk_min_confidence: 0 = disabled; otherwise candidate floor [0,1]. */
   int memory_abstain_enabled;
   double memory_abstain_gate;
   double memory_chunk_min_confidence;

   /* Weight profile inline overrides: applied on top of the file-based profile.
    * memory_bm25_weight: lexical (BM25-style) score weight (0 = use profile/default).
    * memory_semantic_weight: semantic/embedding score weight (0 = use profile/default). */
   double memory_bm25_weight;
   double memory_semantic_weight;

   /* Cosine-similarity floor scale for the semantic-memory legs. The floors in
    * memory_collect_*_semantic_matches were calibrated for the retired 384-d
    * builtin embedder, whose cosine range runs high; modern embedders compress
    * that range (pplx-0.6b relevant matches ~0.35-0.40, pplx-4b higher), so a
    * fixed 384-era floor rejects every real hit. 0 = auto-scale by the active
    * embedding dimension (db2_embedding_dim()); >0 pins the multiplier. */
   double memory_semantic_floor_scale;

   /* Dynamic fetch budget: total candidate pool size, intent-scaled.
    * memory_fetch_budget_enabled: 0 = use fixed pool (default), 1 = scale by specificity.
    * memory_fetch_budget_base: base candidate count; intent multiplier is applied on top
    *   (default 128; range 32–512).
    * memory_fetch_budget_shape_aware: when 1 (default when budget is enabled), fold
    *   query-shape width into the specificity factor — LIST / QUANTITATIVE /
    *   TEMPORAL_INTERVAL widen the pool, YES_NO shrinks it. 0 falls back to the
    *   token-count-only clamp-down-only form. See
    *   docs/proposals/pending/conversational-retrieval-rerank-and-hard-negatives.md. */
   int memory_fetch_budget_enabled;
   int memory_fetch_budget_base;
   int memory_fetch_budget_shape_aware;

   /* Hard-negative logging: emit failing eval top candidates to a JSONL file.
    * memory_hard_negative_log: path to the output file (empty = disabled). */
   char memory_hard_negative_log[512];

   /* Dogfood logger: per-moment JSONL records of memory-adjacent tool calls
    * so operators can review retrieval quality over weeks of real use.
    * See docs/proposals/pending/dogfood-agent-eval.md.
    *   dogfood_enabled: 0 = off, 1 = append a record on each call (default 1).
    *   dogfood_log_dir: directory for `YYYY-MM.jsonl` files
    *       (empty = <output_dir>/dogfood).
    *   dogfood_commit_raw: 0 = strip raw query text, persist only an
    *       FNV-1a hash; 1 = keep raw query too. Default 0 so committed
    *       logs never leak user text. */
   int dogfood_enabled;
   char dogfood_log_dir[512];
   int dogfood_commit_raw;
   /* dogfood_inline_tagging: when non-zero, dogfood_inline_hint_json()
    * returns a short JSON blob after memory-tool calls that a client UI
    * can render as a one-keystroke tagging prompt. Off by default;
    * consumers that don't render it pay no cost. See
    * docs/proposals/pending/dogfood-operational-closeout.md. */
   int dogfood_inline_tagging;

   /* Dogfood weak auto-labelling: each heuristic is independently
    * gated. All default off so an operator opts in per heuristic after
    * reviewing a week of sidecar output (sidecar labels always win on
    * merge, so a noisy auto-label is recoverable). See
    * docs/proposals/done/dogfood-autolabel-and-first-cycle.md.
    *   dogfood_autolabel_repair: user's next turn starts with a
    *     correction cue → outcome=miss on the prior memory-tool record.
    *   dogfood_autolabel_continuation: user's next turn advances the
    *     task without correcting → outcome=hit, surprise=false.
    *   dogfood_autolabel_repeat_question: same (session, tool,
    *     query_hash) already appeared in the month → outcome=miss.
    *     Self-contained at write time. */
   int dogfood_autolabel_repair;
   int dogfood_autolabel_continuation;
   int dogfood_autolabel_repeat_question;

   /* Learning router: explicit feedback becomes a proposal first, and the
    * proposal gate enforces corroboration / accept-before-commit. */
   int learning_router_enabled;
   int learning_proposal_ttl_days;
   int learning_max_commits_per_week;

   /* Candidate-generation synthesis pass (learning.synthesize.*). Runs on the
    * kb scheduler: builds an evidence neighbourhood, hands it to the
    * model sidecar, and writes the returned candidates as proposed artifacts.
    * learning_synthesize_enabled:    0 = off (default), 1 = run the pass.
    * learning_synthesize_command:    sidecar command (scripts/learning-synthesize.py).
    * learning_synthesize_max_tokens: per-call token budget (default 2048).
    * learning_synthesize_k:          neighbourhood size fed to the sidecar (default 8). */
   int learning_synthesize_enabled;
   char learning_synthesize_command[512];
   int learning_synthesize_max_tokens;
   int learning_synthesize_k;

   /* Version-bump replay (learning.{embed.model_version, synthesize.prompt_version}).
    * Bumping the embedding model_version re-embeds the evidence layer (without
    * re-running synthesis); bumping the synthesis prompt_version replays the
    * candidate-generation pass (without re-embedding). Defaults "v1". */
   char learning_embed_model_version[64];
   char learning_synthesize_prompt_version[64];

   /* Learning implicit signal detectors (phase 2).  Each default-off.
    * When enabled, the matching heuristic emits a learning_signal_input_t
    * on detection without operator action.  All require learning_router_enabled.
    *   citation_then_repair: next turn classified as correction after a
    *     memory-tool call → feedback_negative signal.
    *   citation_then_continuation: next turn classified as continuation →
    *     feedback_positive signal.
    *   repeat_question: same (session, tool, query_hash) triple already in
    *     this month → feedback_negative signal.
    *   repeated_correction: correction proposals for the same target_key
    *     exceed threshold → feedback_negative signal.
    *   workflow_repetition: kb_client_memory_upsert_workflow succeeds for
    *     a key that already had a workflow signal → tool_outcome signal. */
   int learning_implicit_citation_repair;
   int learning_implicit_citation_continuation;
   int learning_implicit_repeat_question;
   int learning_implicit_repeated_correction;
   int learning_implicit_workflow_repetition;
   /* When on, the continuation/repair autolabel also writes a retrieval OUTCOME
    * (retrieval_attribution for memory, ranker_outcome for kb_hybrid) for the
    * prior turn's surfaced rows — closing the demotion + learning-to-rank loops.
    * Default off. See docs/proposals/done/kb-hybrid-outcome-wiring.md. */
   int learning_implicit_retrieval_outcome;

   /* Autonomous mode: launch agent CLIs with their full autonomous flags,
    * relying solely on aimee guardrails for safety */
   int autonomous;

   /* Verify master switch. When 0 (default), aimee does not automatically gate
    * pushes/PR-creates or auto-generate an enforcing project.yaml: a repo is
    * only gated if it already has an explicit project.yaml with enforce:true
    * (which re-enables the gate per-project). Explicit `aimee git verify` runs
    * still execute steps on demand regardless. Set to 1 to restore automatic
    * gating + enforce:true auto-generated config for the current project. */
   int verify_enabled;

   /* §7 code-graph actuation: prepend a structural-context block (callers /
    * dependencies of the file paths a delegate task references) to the delegate's
    * system prompt. Advisory, fail-open, default off (opt-in). */
   int delegate_graph_context_enabled;

   /* Replayable-evidence roundtable verification (Part A). When 1 (default), a
    * review-mode roundtable runs each captured item through an independent replay
    * pass that re-grounds its structured evidence against the read-only code
    * index before the artifact is assembled (drops unreproducible items, caps
    * interpretive-only at "concern"). 0 = legacy behavior: no replay, no rejected
    * appendix — byte-identical to pre-Part-A. */
   int roundtable_replay_verify_enabled;

   /* roundtable_require_evidence (default 1, ON): the evidence gate. A review finding
    * with NO structured evidence (kind=none) is an unfalsifiable opinion the chair
    * cannot check against the code, so it is REJECTED rather than kept-but-capped —
    * members may only raise findings they back with checkable evidence (refs/symbol/
    * search). Only meaningful when roundtable_replay_verify_enabled is on (the chair
    * runs). Set 0 to restore the legacy behavior (interpretive items capped, not
    * dropped). */
   int roundtable_require_evidence;

   /* roundtable_chair_synthesis (default 1, ON — operator ruling 2026-07-17): after the
    * deterministic evidence verifier runs, add a CHAIR reasoning pass over the surviving
    * findings — an LLM that may DEMOTE or DROP a technically-true-but-over-flagged finding
    * (never escalate, never add), each change shown with the chair's rationale. It adds a
    * second (fallible) model to the adjudication step, but server-side bounds keep it
    * strictly conservative — it can only ever make the result less alarming, transparently,
    * and unparseable chair output is a no-op. Set 0 to disable. Single-round review only
    * (multi-round already synthesizes via the aggregator). */
   int roundtable_chair_synthesis;

   /* Verify scope. When 0 (default), `aimee git verify` and the push/PR verify
    * gate apply only to the session's current project (the repo the session is
    * rooted in). Cross-project repositories are neither auto-configured (no
    * project.yaml is generated) nor gated, and verify will not run against them.
    * Set to 1 to allow verify to run and enforce across other repositories. */
   int verify_cross_project;

   /* Cross-verification: delegates verify tool fixes, tool verifies delegate fixes */
   int cross_verify;
   char verify_cmd[512];
   char verify_role[32];
   char verify_prompt[2048];

   /* API retry: exponential backoff for transient provider failures.
    * Set retry_max_attempts=0 to disable retries. */
   int retry_max_attempts; /* max retries (default 3) */
   int retry_base_ms;      /* initial backoff delay (default 1000) */
   int retry_max_ms;       /* backoff ceiling (default 30000) */

   /* Agent iteration limits: cap tool-call rounds per user message.
    * 0 = use default (15 interactive, 25 delegate). */
   int max_iterations;          /* per-turn cap for interactive chat (default 15) */
   int max_iterations_delegate; /* per-turn cap for delegate sessions (default 25) */

   /* Delegation depth/spawn limits: prevent runaway delegation chains.
    * 0 = use default (depth 3, spawns 50). */
   int max_delegation_depth;  /* max nesting depth for delegate chains (default 3) */
   int max_delegation_spawns; /* max total delegates per root session (default 50) */

   /* Global background-thread budget for non-session local parallel work.
    * Config accepts the preferred background_threads key and legacy
    * compute_threads / worker_threads keys. 0 = default to 2. */
   int compute_threads;

   /* Per-session threadpool size for chat/tool/delegate work tied to an
    * aimee session. 0 = default to 4. */
   int session_threads;

   /* Backstop ceiling on concurrent on-demand (I/O-bound) background delegates.
    * Real throttling is the per-model concurrency limiter; this only guards
    * pathological fan-out. 0 = default to 512. Key: delegate_max_inflight. */
   int delegate_max_inflight;

   /* Global ceiling on concurrent agent execution contexts across ALL agents
    * (the agent_admission controller). 0 uses AGENT_ADMISSION_DEFAULT_GLOBAL_MAX. */
   int maximum_total_concurrent_agent_sessions;

   /* Per-model/provider concurrency limits: prevent rate-limit cascades.
    * concurrency_default = 0 uses CONCURRENCY_DEFAULT_LIMIT (5). */
   int concurrency_default;
   config_concurrency_entry_t concurrency_per_model[CONFIG_CONCURRENCY_MAX_ENTRIES];
   int concurrency_per_model_count;
   config_concurrency_entry_t concurrency_per_provider[CONFIG_CONCURRENCY_MAX_ENTRIES];
   int concurrency_per_provider_count;
   int concurrency_preempt_enabled;
   int concurrency_preempt_single_slot_only;
   int concurrency_preempt_requeue_max;

   /* Web search tool for delegates.
    * search_backend: "duckduckgo" (default), "searxng", or "tavily"
    * search_max_results: default result count (0 = use WEB_SEARCH_DEFAULT_MAX_RESULTS = 5)
    * search_searxng_url: required when backend = "searxng", e.g. "https://searxng.example.com"
    * search_tavily_api_key: required when backend = "tavily"
    * search_backends: optional comma-separated list enabling multi-engine fanout
    * search_fetch_pages: -1 unset (default on), 0 off, 1 on
    */
   char search_backend[32];
   int search_max_results;
   char search_searxng_url[512];
   char search_tavily_api_key[256];
   /* search_backends: optional comma-separated engine list for multi-engine
    * fanout, e.g. "duckduckgo,searxng". When empty, search_backend is used
    * alone. Engines missing their required credential are skipped rather than
    * failing the search, so listing an engine you have not configured yet
    * degrades to the ones you have.
    *
    * Fanout is OFF unless this is set: it multiplies latency and outbound
    * requests, and a default install has exactly one usable engine
    * (duckduckgo) so it would buy nothing. */
   char search_backends[256];
   /* search_fetch_pages: fetch the top results and return extracted page text
    * instead of only engine snippets. 0 = off, 1 = on, -1/unset = built-in
    * default (on). See WEB_SEARCH_FETCH_PAGES_DEFAULT. */
   int search_fetch_pages;

   /* Tool result compaction settings.
    * compact_enabled: 0 = off, 1 = on (default when unset).
    * compact_threshold: bytes before compaction triggers (0 = built-in default 4096).
    * compact_head_bytes / compact_tail_bytes: plain-text head/tail sizes (0 = built-in defaults).
    * compact_per_tool: per-tool threshold overrides stored as "tool=threshold" strings,
    *   where threshold -1 means disabled for that tool. */
#define CONFIG_COMPACT_MAX_PER_TOOL 8
   int compact_enabled;
   int compact_threshold;
   int compact_head_bytes;
   int compact_tail_bytes;
   char compact_per_tool[CONFIG_COMPACT_MAX_PER_TOOL][128]; /* "tool_name=threshold" */
   int compact_per_tool_count;

   /* Session-compaction summary derivation. 0 (default) = the legacy prose scan
    * (guess paths by shape, match "error"/"decided" keywords). 1 = derive from the
    * economizer's deterministic extractors instead: Coordinate Closet coordinates
    * conserved VERBATIM, and fold_register's settled/hazard classification of the
    * agent's own turns. Default-off because compaction quality is still unmeasured
    * (docs/proposals/pending/compaction-quality-baseline.md); the legacy path stays
    * selectable until that baseline can say which is better. */
   int compact_from_record;

   /* Coordinate Closet (fold §2): conserve verbatim identifiers from compacted
    * tool results. Nested under the "compact" config section. Default-off.
    * coord_closet_enabled: 0 = off (default), 1 = on.
    * coord_closet_budget_bytes: hard byte cap for the conserved block (0 = default).
    * coord_closet_max_ratio_pct: closet <= raw_len * pct/100 (0 = default 100).
    * coord_closet_denylist: extra secret patterns (comma/space separated). */
   int coord_closet_enabled;
   int coord_closet_budget_bytes;
   int coord_closet_max_ratio_pct;
   char coord_closet_denylist[256];

   /* Rolling context fold (fold §1, P2b). Folds a prefix of old turns into a
    * skeleton + Coordinate Closet on the delegate request path. Default-off.
    * The fold's closet reuses the coord_closet_* settings above (one policy).
    * fold_enabled: 0 = off (default), 1 = on.
    * fold_retained_msgs / fold_min_fold_msgs / fold_excerpt_bytes: 0 = module
    *   defaults (see context_fold.h). */
   int fold_enabled;
   int fold_retained_msgs;
   int fold_min_fold_msgs;
   int fold_excerpt_bytes;
   int fold_register_enabled; /* §6: annotate folded assistant lines with their register */
   /* Fold-freeze (§3): pin the fold boundary across turns so the folded prefix
    * stays byte-identical and the provider cache stays warm. Default-off.
    * fold_freeze_tail_cap_msgs: re-epoch (advance the boundary) when the un-folded
    * tail exceeds this many messages (0 = module default). */
   int fold_freeze_enabled;
   int fold_freeze_tail_cap_msgs;
   /* Fold recall (§4): when the agent re-touches a folded-away path/handle, emit a
    * recall hint so the body can be paged back in on demand. Default-off.
    * fold_recall_ttl_turns: don't re-surface the same key within this many turns. */
   int fold_recall_enabled;
   int fold_recall_ttl_turns;
   /* fold_recall_inject: put the hint in front of the model instead of only reporting
    * it. Separate from _enabled and default-off, because tracking what was evicted is
    * inert while injecting CHANGES WHAT THE MODEL DOES, and whether that helps or
    * derails a turn is a behavioural question for live traffic to answer. */
   int fold_recall_inject;

   /* The single economizer mode. OFF is the pristine baseline. PROOF_GATED
    * verifies the signed empty registry and freezes the completed provider body;
    * it cannot select a transform in this release. Default OFF. */
   int economizer_mode;

   /* Pluggable-module enablement (`modules:` block). The canonical, user-facing surface for
    * enabling/disabling modules — memory (request stage), governance (response stage), delegates +
    * workflows (orchestration hooks), and optional roundtable panels. Each is a TRISTATE:
    *   -1  unspecified — the config does not set it; the resolver falls back to the module's
    *       deprecated env toggle (AIMEE_STAGE_MEMORY / _GOVERNANCE / AIMEE_ORCH_DELEGATES /
    *       _WORKFLOWS / AIMEE_MODULE_ROUNDTABLE) and then to its descriptor default (governance
    *       and roundtable default OFF; the remaining legacy gateway-stage modules default ON).
    *    0  user-disabled.    1  user-enabled.
    * Resolution is centralized in config_module_enabled() so the future admin/governance FORCE
    * tier (aimee-kb governance state that can pin a module on or off over the user's choice)
    * slots in at ONE site. Defaults are -1 (unspecified) so existing env-configured deployments
    * are unaffected until an operator writes the `modules:` block.
    * CONTRIBUTORS: do not resolve module enablement inline at a wire site. For legacy gateway-stage
    * modules, call config_module_enabled(cfg->module_X, <module env predicate>). For roundtable,
    * call config_module_roundtable_enabled(), which applies modules.roundtable, then
    * AIMEE_MODULE_ROUNDTABLE, then the descriptor default — and, per the config_t encapsulation
    * rule, reads the env var HERE so the roundtable module holds neither a config_t nor a getenv.
    * Keeping resolution behind one owner API leaves one place for a future FORCE tier. */
   int module_memory;
   int module_governance;
   int module_delegates;
   int module_workflows;
   /* Optional multi-agent panel deliberation. Unspecified (-1) falls back to the
    * roundtable-owned AIMEE_MODULE_ROUNDTABLE resolver, whose descriptor default is OFF. */
   int module_roundtable;
   /* The economizer module toggle. econ_mode() returns ECON_MODE_OFF whenever this is
    * user-disabled (0), so the `modules:` off-switch overrides the economizer mode. */
   int module_economizer;

   /* Autonomous-development pipeline knobs (Phase-C). These were env-var-only
    * (AIMEE_AUTONOMY_*); the config values are bridged to those env vars at startup
    * (autonomy_config_to_env) so the wfe library — which reads them via getenv across a
    * module boundary — sees them, while an explicitly-set env var still overrides.
    * Exposed here so they appear in the typed config surface + web Settings. Defaults
    * match the historical env defaults; a change applies on the next server start.
    * autonomy_skeptics: N adversarial skeptics on the implement gate (0 = off).
    * autonomy_fanout: 1 = engine-level fan-out manager loop (0 = single-dispatch).
    * autonomy_unit_retry: per-unit retry-different-delegate cap.
    * autonomy_unit_max: max fan-out units (a larger decomposition parks).
    * autonomy_ci_retry_max: per-work-item red-CI retry cap before parking. */
   int autonomy_skeptics;
   int autonomy_fanout;
   int autonomy_unit_retry;
   int autonomy_unit_max;
   int autonomy_ci_retry_max;
   /* Run safety caps + auto-resume policy — historically env-only (AIMEE_AUTONOMY_*);
    * now config-backed + live via config_autonomy_lookup so they are tunable from the web
    * Settings GUI (an exported env var still overrides). Defaults match the historical env
    * defaults, so behavior is unchanged until an operator changes them.
    * autonomy_max_turns: cumulative persisted-turn cap per run (runaway backstop).
    * autonomy_max_wall_secs: per-resume wall-clock cap in seconds.
    * autonomy_stale_abandon_secs: grace before a cap/stuck park is reaped -> abandoned (0
    * disables). autonomy_concurrency: max concurrently-driven autonomous runs.
    * autonomy_auto_resume_cap_parks: 1 = scheduler auto-resumes a wall-cap park (giving it a
    *   fresh wall window) instead of leaving it to be reaped; bounded by autonomy_max_resumes.
    * autonomy_max_resumes: max auto-resumes per run before the reaper is allowed to win. */
   int autonomy_max_turns;
   int autonomy_max_wall_secs;
   int autonomy_stale_abandon_secs;
   int autonomy_concurrency;
   int autonomy_auto_resume_cap_parks;
   int autonomy_max_resumes;

   /* Session/worktree cleanup policy.
    * worktree_stale_secs: inactivity threshold before a session is pruned
    *   (0 = use default: 14400 = 4 hours).
    * max_sessions: cap on concurrent active sessions; 0 = unlimited.
    *   When the cap is reached, the oldest idle session is cleaned before
    *   a new one starts.
    * max_worktrees: cap on active sibling worktrees; 0 = unlimited. */
#define CONFIG_DEFAULT_STALE_SESSION_SECS 14400
   int worktree_stale_secs;
   int max_sessions;
   int max_worktrees;

   /* Sandbox configuration for tool execution.
    * sandbox.mode: "off" (default), "workspace_only", "allowlist"
    * sandbox.network: true = block outbound network (Linux only)
    * sandbox.allow_paths: array of extra paths to expose in allowlist mode */
   sandbox_config_t sandbox;

   /* System prompt tier selection.
    * prompt_tier: "MINIMAL", "STANDARD" (default), or "EXTENDED"
    * prompt_file: path to a custom prompt file (overrides tier when set)
    * delegate_prompt_tier: tier for delegate sub-agents (default "MINIMAL") */
   char prompt_tier[16];
   char prompt_file[MAX_PATH_LEN];
   char delegate_prompt_tier[16];

   /* Background process management.
    * max_background_processes: concurrent process limit (0 = use PROC_MAX_CONCURRENT = 5). */
   int max_background_processes;

   /* Rewind / file-snapshot settings.
    * rewind_auto_snapshot: 1 = automatically capture a file snapshot before each write_file /
    *   edit_file tool call so the file can be restored via `aimee rewind restore`.
    *   Default 0 (off) to avoid overhead. */
   int rewind_auto_snapshot;

   /* LSP server configuration.
    * Zero or more server entries mapping file extensions to LSP commands.
    * Servers are started lazily on first file touch and shut down at session end.
    * Empty lsp_server_count means LSP enrichment is disabled. */
   struct config_lsp_server
   {
      char name[64];
      char command[512];
      char args[16][256];
      int arg_count;
      char extensions[8][16];
      int extension_count;
   } lsp_servers[8];
   int lsp_server_count;

   /* OpenTelemetry trace export.
    * otel_endpoint: OTLP/HTTP base URL, e.g. "http://192.168.1.100:4318".
    *   Empty string (default) disables export with zero overhead.
    * otel_service_name: reported service.name attribute (default "aimee"). */
   char otel_endpoint[512];
   char otel_service_name[64];

   /* MCP client sessions declared in aimee.yaml.
    * Each entry can describe a stdio server process today; SSE metadata is
    * parsed and retained for the follow-up transport implementation. */
   config_mcp_client_t mcp_clients[CONFIG_MCP_MAX_CLIENTS];
   int mcp_client_count;
   int mcp_osv_enabled;
   int mcp_osv_offline;
   int mcp_osv_enforce;
   int mcp_osv_cache_ttl_hours;
   char mcp_osv_endpoint[256];
   char mcp_osv_allow[CONFIG_MCP_OSV_MAX_ALLOW][256];
   int mcp_osv_allow_count;

   /* External computer-use/browser MCP capability. Disabled by default.
    * The external driver remains an MCP client; these keys only control
    * exposure and deterministic per-action risk policy. */
   int computer_use_enabled;
   char computer_use_default_navigation[16];
   int computer_use_redact_sensitive_screenshots;
   char computer_use_allowed_domains[CONFIG_COMPUTER_USE_MAX_DOMAINS][128];
   int computer_use_allowed_domain_count;

   /* Team API key proxy (aimee-proxy).
    * proxy_url: base URL of the proxy, e.g. "http://proxy.internal:8400".
    *   Empty (default) disables proxy mode; requests go directly to providers.
    * proxy_token: team bearer token issued by `aimee-proxy token create`. */
   char proxy_url[256];
   char proxy_token[128];

   /* Ingest integrity gate (integrity.*): Layer 1 deterministic pattern gate
    * at every untrusted-by-default write entry point.
    * integrity_enabled: 0 = disabled (default), 1 = enabled.
    * integrity_dry_run: 1 = shadow mode — gate fires, logs, emits evidence,
    *   but never changes routing (default 1 so enabling is always safe first).
    * See docs/proposals/accepted/ingest-poison-gate.md. */
   int integrity_enabled;
   int integrity_dry_run;

   /* Virtual context assembly (session.virtual_context.*): session-local
    * tool-chain stub generation and prompt working-set management.
    * virtual_context_enabled: 1 = on (default, since the rollout-validation gate
    *   cleared); 0 = off (rollback to raw turns, no data loss).
    * virtual_context_assembly_budget: max bytes of chain stubs to inject into delegate
    *   prompts (default 4096).
    * See docs/proposals/done/virtual-context-assembly-and-tool-chain-paging.md and
    * docs/proposals/done/virtual-context-assembly-rollout-validation.md. */
   int virtual_context_enabled;
   int virtual_context_assembly_budget;

   /* Prompt-cache-aware deferred payload rewrite (transport.cache_aware_rewrite.*).
    * cache_aware_rewrite_enabled: 0 = disabled (default), 1 = enabled.
    * cache_aware_rewrite_min_savings_tokens: pending savings threshold before a
    *   rewrite is forced to realize compaction gains (default 500).
    * cache_aware_rewrite_hard_context_threshold: fraction of context limit that
    *   forces an immediate rewrite regardless of cache warmth (default 0.85).
    * cache_aware_rewrite_max_defer_turns: absolute ceiling on consecutive deferrals
    *   before a cache-horizon forced rewrite (default 20; 0 = no ceiling).
    * cache_aware_rewrite_segment_check_turns: force a rewrite every N consecutive
    *   deferrals to catch segment drift (default 5; 0 = disabled).
    * See docs/proposals/accepted/prompt-cache-aware-deferred-payload-rewrite.md. */
   int cache_aware_rewrite_enabled;
   int cache_aware_rewrite_min_savings_tokens;
   double cache_aware_rewrite_hard_context_threshold;
   int cache_aware_rewrite_max_defer_turns;
   int cache_aware_rewrite_segment_check_turns;

   /* Live transport controls (transport.*). The measured defaults enable KB
    * pooling and resident-client keep-alive; either can be set false for the
    * one-shot rollback path. gzip stays default-off until a link profile meets
    * both the wire-reduction and latency promotion gates. */
   int transport_kb_pool_enabled;
   int transport_server_keepalive_enabled;
   int transport_thinclient_gzip_enabled;
   int transport_kb_gzip_enabled;

   /* Cost-shaped delegate-routing bandit reward (cost_reward.*).
    * cost_reward_enabled: 0 = off (default; the bandit learns from the binary
    *   success outcome only), 1 = on (subtract a normalized cost penalty so the
    *   router can prefer cheaper arms when quality is similar).
    * cost_reward_lambda_pct: penalty weight as a percent (default 30 = 0.30);
    *   reward = clamp01(success - lambda * min(cost / cost_ref, 1)).
    * cost_reward_ref_usd_milli: per-turn cost ($, in milli-dollars) that maps to a
    *   full normalized penalty (default 500 = $0.50). */
   int cost_reward_enabled;
   int cost_reward_lambda_pct;
   int cost_reward_ref_usd_milli;

   /* Complexity-score reasoning-effort cap (reasoning_cap.*; §5).
    * reasoning_cap_enabled: 0 = off (default), 1 = on. When on, a deterministic
    *   0-10 complexity score for the turn caps (only ever lowers) the reasoning
    *   effort that would otherwise be sent to a reasoning-effort-capable provider
    *   surface (Codex/OpenAI enum, Claude --effort). An explicit per-request
    *   override and providers without a reasoning surface are left untouched. */
   int reasoning_cap_enabled;

   /* Short-window response dedup for buffered ingress (response_dedup.*; §4).
    * dedup_enabled: 1 = on (default), 0 = off. When on, a re-sent identical
    * buffered, non-streaming, tool-free completion carrying an explicit
    * Idempotency-Key is served from a small TTL cache instead of paying for the
    * provider call again; the avoided call is recorded as usage_kind=avoided
    * (not spend). Strictly per-principal: the cache key carries the account
    * boundary so one caller never reads another's response. */
   int dedup_enabled;

   /* Cache-aware request shaping for the aimee-owned Anthropic path (§3).
    * cache_shaping_enabled: 1 = on (default), 0 = off. When on, the tool-bearing
    * Anthropic request builder marks the aimee-owned system prefix cacheable
    * (cache_control: ephemeral content block), matching the no-tools path, so the
    * provider caches the stable system prompt across calls. Anthropic-only; never
    * alters the stateless /v1/messages proxy or any non-Anthropic provider. */
   int cache_shaping_enabled;

   /* Extended thinking on aimee's OWN Anthropic requests (extended_thinking.*).
    * extended_thinking_enabled: 0 = off (default), 1 = on. The IR request builder
    *   never set the Anthropic `thinking` config, so ir->thinking was populated
    *   only by an inbound client request and an aimee-originated turn asked for no
    *   reasoning at all -- measured 2026-08-09: reasoning appears only when a
    *   provider volunteers it unprompted. Default-OFF because thinking tokens are
    *   billed: turning this on changes what aimee spends, not just what it reads.
    *
    * There is no budget knob. The shape this used to feed --
    * {"type":"enabled", budget_tokens: N} -- was removed by Anthropic and is a
    * 400 on Opus 4.7 and later; the builder now emits {"type":"adaptive"},
    * which takes no budget, and only for a model whose capabilities report
    * that it accepts that shape (MODEL_CAP_THINKING_ADAPTIVE). Enabling this
    * against a model nobody has reported that for is a no-op by design: see
    * agent_request_build.c. */
   int extended_thinking_enabled;

   /* Ingress cost-accounting rollout knobs (ingress.*; §2/§4/#3).
    * ingress_usage_accounting_enabled: 1 = on (default), 0 = off. Master gate for
    *   writing ingress cost rows (OpenAI/Codex + Anthropic /v1/messages + /v1/runs).
    *   Default-on so accounting runs out of the box; set false to opt out.
    * ingress_audit_async: 1 = hand the cost row to a background writer so the response
    *   is not blocked on the DB insert (default); 0 = write inline. Pairs with
    *   usage_accounting so the default-on accounting write stays off the response path.
    * ingress_trusted_proxy_secret: shared secret that authorises a front proxy to
    *   stamp X-Aimee-Principal / X-Aimee-Source on a request. EMPTY (default) means
    *   NO client-supplied principal/source is ever trusted — the server derives the
    *   principal from the kernel-verified UDS peer uid instead. A request is trusted
    *   only when it presents X-Aimee-Proxy-Authorization equal to this secret.
    * dedup_window_seconds: §4 dedup TTL (default 5).
    * cache_min_chars: §3 only cache-marks a system prompt at least this many bytes
    *   long, so tiny prompts are not marked (default 0 = always mark when on). */
   int ingress_usage_accounting_enabled;
   int ingress_audit_async;
   char ingress_trusted_proxy_secret[128];
   int dedup_window_seconds;
   int cache_min_chars;

   /* Neural-assisted semantic guardrails (guardrails.semantic.*).
    * semantic_mode: the single escalation control. One of GSEM_MODE_* names:
    *   "off"      — no assessment runs (default).
    *   "dry_run"  — shadow mode: score is logged/stored but never changes the outcome.
    *   "advisory" — enforce warn/prompt advisories, but a "block" recommendation is
    *                downgraded to "prompt" (never a hard block).
    *   "enforce"  — a "block" recommendation hard-blocks the tool.
    * (Replaces the former enabled/dry_run/advisory_only/allow_ml_only_block quad, which
    * encoded exactly this off->dry_run->advisory->enforce ladder.)
    * semantic_command: external sidecar command (stdin: JSON request, stdout: JSON response).
    *   Empty (default) disables sidecar invocation; no assessment runs when empty.
    * semantic_warn_threshold, prompt_threshold, block_threshold: score bands.
    * See docs/proposals/accepted/neural-assisted-guardrails.md. */
   char guardrails_semantic_mode[16];
   char guardrails_semantic_command[512];
   double guardrails_semantic_warn_threshold;
   double guardrails_semantic_prompt_threshold;
   double guardrails_semantic_block_threshold;
   /* §7 code-graph actuation: surface a structural blast-radius advisory (the
    * graph-impacted dependent files) before an edit. Advisory + fail-open;
    * default off (opt-in). See docs/proposals/pending/code-graph-intelligence.md §7. */
   int guardrails_blast_radius_advisory_enabled;

   /* aimee-kb public HTTP API (kb.api.*).
    * kb_api_http_port: TCP port for the /v1/... REST API (0 = disabled, default).
    * kb_api_bearer_token: transient process-memory view of the Vault-backed API
    * bearer (empty = no auth). It is never serialized to aimee.yaml. A
    * first-boot/Kubernetes environment value is sealed before the listener starts
    * and removed by a clean re-exec.
    *   May be self-describing for scoped access:
    *     scope:<kind>:<id>:<secret>   — only authorizes requests in that scope
    *                                     (e.g. scope:project:foo:s3cr3t); cross-
    *                                     scope requests get 403. See kb_scope.h.
    *     <secret>                     — unscoped/admin token (full access). */
   int kb_api_http_port;
   char kb_api_bearer_token[256];

   /* telemetry.metrics_token (P9a): transient process-memory view of the
    * Vault-backed SHA-256 verifier for GET /v1/metrics + POST
    * /v1/telemetry/metrics. The digest is credential material and is never
    * serialized outside Vault. Empty = no token (those routes are then
    * org-admin only). One token covers both scrape (read) and ingest (write). */
   char telemetry_metrics_token[128];

   /* Remote aimee-kb client pointer (used when this host does NOT run a local
    * aimee-kb sidecar). When set, aimee-server exports these into its own
    * runtime cache so kb_client reaches the remote kb. The bearer is a transient
    * process-memory view of the same Vault record as kb_api_bearer_token and is
    * never serialized to aimee.yaml.
    * Distinct from kb_api_bearer_token above, which is the LOCAL kb server's
    * inbound-auth token, and NO LONGER required to equal it: set
    * AIMEE_KB_CLIENT_BEARER_TOKEN to a scoped `service` credential
    * (scope:service:<name>:<secret>) so aimee-server reaches every project's
    * data while remaining refused by aimee-kb's administrative routes. When it
    * is unset, AIMEE_KB_API_BEARER_TOKEN is used as before — which makes
    * aimee-server the install owner on aimee-kb. Either variable is accepted
    * only as first-boot transport, then synchronously sealed and scrubbed before the
    * long-lived process starts. */
   char kb_client_url[CONFIG_DB2_URL_LEN];
   char kb_client_bearer_token[256];

   /* Setup-wizard page 2 (LLM / embedding / KB backend). The wizard RECORDS the
    * operator's choices here; the deploy layer (compose) reads them and brings up
    * only what is configured — nothing is deployed until page 2 is set. Empty
    * kb_mode means "not configured yet" (a fresh install).
    *
    * kb_mode:
    *   "remote" — connect to an existing aimee-kb (kb_client_url/bearer above);
    *              NOTHING is deployed locally (no kb, no llm).
    *   "local"  — run a local aimee-kb and deploy the per-role LLM backends below.
    *
    * The EMBEDDER is served by the kb itself unless embedder_url points at an
    * operator endpoint. An unset embedder_dims (0) is derived from the selected
    * model. SYNTHESIS is whatever answers synthesis_endpoint. */
   char kb_mode[16]; /* "" | "local" | "remote" */

   /* Does THIS IMAGE bundle llama.cpp? "1" on the aimee-kb-*-llm variants, "0" or
    * empty otherwise. Set as ENV by the Dockerfile in every variant, so it is a
    * fact about the running image rather than a preference. The setup wizard reads
    * it: offering "run gemma-4 locally" on an image with no llama.cpp is an option
    * that cannot work, and the failure would only surface later as synthesis
    * silently never starting. Absent reads as NOT bundled. */
   char aimee_with_llamacpp[8];

   /* WHICH synthesis model this image bakes: "gemma-4-E2B-it", "gemma-4-E4B-it",
    * or empty on a non-llm variant. Set as ENV by the Dockerfile, like
    * aimee_with_llamacpp — a fact about the image, not a preference.
    *
    * The wizard needs it because the model is no longer a runtime choice: an image
    * carries one, so the UI reports what it has instead of offering a menu it
    * cannot honour. Same shape as the embedder, and for the same reason. */
   char aimee_synthesis_model[64];

   /* Synthesis. ONE endpoint, whether the model is bundled or remote: an
    * aimee-kb *-llm image variant runs gemma-4-E2B-it or gemma-4-E4B-it on the
    * same host as the kb, so "local" is just a 127.0.0.1 URL and needs no
    * separate variable. Empty means synthesis is OFF, which is a supported
    * state — embedding, search, recall and indexing never call this endpoint.
    *
    * Retired together with the aimee-llm container: llm_embed_backend,
    * llm_synth_backend, llm_synth_host, llm_synth_gpu, llm_synth_tier. Those
    * chose where to place a separate inference container; the aimee-kb image
    * variant now encodes that choice, so selecting it at runtime is meaningless.
    * They could also disagree with the fields they gated: backend="external"
    * with an empty URL emitted nothing and failed silently. */
   char synthesis_endpoint[512];
   char synthesis_model[128];
   char synthesis_api_key[256];

   /* Let the synthesis model think (default ON, and global rather than per-stage).
    * It measured positive-to-neutral everywhere it was tried: on the 69-note
    * extraction set the default model gains 0.084 F1 with thinking on, 95% CI
    * [+0.0094,+0.1712] by paired bootstrap (docs/SYNTHESIS_MODELS.md).
    *
    * This was previously not a setting at all — it was implied by the stage's tier,
    * suppressed for the mechanical stages because a reasoning pass can consume the
    * output budget and truncate the JSON answer. That risk is bounded by
    * MF_LLM_OUT_CAP, and it is the operator's call, so it is one switch here rather
    * than a rule baked into the stage table. Turn it off if you point synthesis at a
    * model that reasons to the cap without answering. */
   int synthesis_thinking;

   /* aimee-server public HTTP API (aimee.api.*). The /v1 surface is always
    * served over the UDS (aimee-http.sock, filesystem-permission auth, no
    * token). These add an optional localhost TCP listener for OpenAI-style
    * external tools.
    * server_api_http_port: TCP port for /v1 over 127.0.0.1 (0 = disabled,
    *   default). Refuses to bind unless server_api_bearer_token is set.
    * server_api_bearer_token: static bearer token required on the TCP
    *   listener (empty = TCP disabled). The UDS listener never requires it.
    * server_api_rate_limit_per_min: max authorized requests per 60s window on
    *   the TCP listener (0 = unlimited, default); over-limit ⇒ 429 +
    *   Retry-After. The UDS listener is never rate-limited. */
   int server_api_http_port;
   /* server_api_tls_port: TCP port for /v1 over native TLS (0 = disabled, default).
    * Terminates TLS in aimee-server itself (cert+key at <config>/tls/server.crt +
    * server.key); refuses to bind unless server_api_bearer_token is set. A TLS+bearer
    * connection is an attested write path for the credential vault (native-TLS
    * provisioning). Streaming (SSE) over TLS is not yet supported (phase 1c). */
   int server_api_tls_port;
   /* server_api_mtls: mutual-TLS client-identity mode for the native-TLS listener
    * (mtls-client-identity). 0 = off (default), 1 = optional (request a client
    * cert; bearer-only still works — a documented downgrade), 2 = required (refuse
    * a TLS conn without a valid client cert). A verified client cert becomes a
    * per-client "cert:<CN>" principal. */
   int server_api_mtls;
   /* PEM bundle of the CA(s) that signed client certs. Empty => <config>/tls/client-ca.crt. */
   char server_api_mtls_client_ca[MAX_PATH_LEN];
   char server_api_bearer_token[256];
   /* Additional bearers accepted alongside server_api_bearer_token.
    *
    * Enrolling a client used to be implemented AS rotating the single global
    * bearer, so the second client to enrol silently evicted the first — every
    * already-paired client started failing at the same instant with no way to
    * tell that from a bad token. Pairing a new client is an additive operation
    * and must not revoke anyone else's credential; rotation, which is the
    * operation that SHOULD revoke, stays separate and explicit.
    *
    * Bounded so a compromised or looping enroller cannot grow the accepted set
    * without limit — past the cap, enrolment fails closed rather than evicting. */
   /* Keep the existing configured-token width: deployments may already have
    * manually supplied bearer_tokens_extra values longer than the 64-hex
    * tokens minted by the enrollment API. Truncating them on upgrade would
    * silently revoke those clients. */
   char server_api_bearer_extra[AIMEE_API_BEARER_EXTRA_MAX][256];
   int server_api_bearer_extra_count;
   int server_api_rate_limit_per_min;
   /* server_api_max_event_streams: cap on concurrent SSE event streams
    * (/v1/sessions/{id}/events, /v1/chat/stream, /v1/runs/{id}/events), each of
    * which holds an offloaded worker thread for its lifetime. Bounds fd/thread
    * use; over-limit ⇒ 503. 0 = use the built-in default (256). Size this to the
    * expected number of concurrent presence subscribers + in-flight streams. */
   int server_api_max_event_streams;
   /* server_api_cli_session_forwarding: enable the server-hosted Claude PTY
    * session routes (/v1/cli/session*), which run claude in a tmux session on
    * the server and forward its raw terminal to a thin client. Default off. */
   int server_api_cli_session_forwarding;
   /* server_api_remote_writes: how far a TCP bearer may mutate (the UDS path is
    * always full). 0 = off (default; mutating routes local-UDS-only), 1 = data
    * (data-mutating /v1 routes allowed over TCP, capability-gated), 2 = full
    * (CAPS_ALL: data writes + delegate/tool over /v1). Parsed from
    * aimee.api.remote_writes ("off"|"data"|"full"); see SERVER_REMOTE_WRITES_*. */
   int server_api_remote_writes;

   /* server_api_client_transport: how first-party CLI clients reach aimee-server.
    *   "socket" (default) — the private NDJSON Unix socket (legacy path).
    *   "http"             — the /v1 HTTP surface (UDS or 127.0.0.1:port).
    *   "auto"             — prefer HTTP, fall back to the socket on failure.
    * Parsed from aimee.api.client_transport; empty ⇒ the "socket" default. */
   char server_api_client_transport[16];

   /* Shadow-traffic publishing has NO persistent enable knob on purpose: it is
    * armed at RUNTIME via POST /v1/shadow/enable and always boots DISARMED, so an
    * operator cannot enable it, reboot, and have live-traffic tapping silently
    * persist. See shadow_mirror.c. */

   /* Bayesian promotion-threshold calibration (intelligence.calibrate.*).
    * calibration_enabled: 0 = off (default), 1 = shadow mode (write profiles,
    *   gate ignores them), 2 = A/B mode (20% of promotion decisions routed through
    *   calibrated thresholds; writes calibration_ab_trace artifacts), 3 = live
    *   (all decisions use calibrated thresholds).
    * calibration_command: external sidecar command for Beta-binomial fit.
    *   Empty (default) disables sidecar; calibration only writes empty shells.
    * calibration_buckets: number of confidence buckets (default 10).
    * calibration_prior_alpha0, calibration_prior_beta0: Beta prior parameters.
    * calibration_credible_delta: credible-bound tail probability (default 0.10).
    * calibration_conformal_window: rolling audit-row window for conformal floor.
    * calibration_conformal_epsilon: miscoverage target (default 0.05).
    * calibration_tau_*: per-surface posterior targets for auto/flag decisions.
    * calibration_prompt_version, calibration_model_version: version key components
    *   used to refit profiles when prompts or scoring models change.
    * See docs/proposals/done/bayesian-promotion-threshold-calibration.md. */
   int calibration_enabled;
   char calibration_command[512];
   int calibration_buckets;
   double calibration_prior_alpha0;
   double calibration_prior_beta0;
   double calibration_credible_delta;
   int calibration_conformal_window;
   double calibration_conformal_epsilon;
   double calibration_tau_memory_auto;
   double calibration_tau_memory_flag;
   double calibration_tau_working_profile_auto;
   double calibration_tau_working_profile_flag;
   char calibration_prompt_version[64];
   char calibration_model_version[64];

   /* Outcome-driven demotion settings.
    * demotion_enabled: 0 = off (default), 1 = shadow mode (compute scores and
    *   fit profiles; lifecycle does not yet consume them), 2 = live (rows whose
    *   score falls below the class p10 threshold are demoted; writes
    *   demotion_action artifacts and reduces memory confidence).
    * demotion_window: attribution-row window per memory row (default 64).
    * demotion_half_life_days: exponential decay half-life for older attributions
    *   (default 30.0).
    * demotion_n_min: minimum attribution rows required before scoring a row
    *   (default 5).  Rows below this threshold remain stable.
    * See docs/proposals/done/outcome-driven-demotion-and-poison-resilience.md. */
   int demotion_enabled;
   int demotion_window;
   double demotion_half_life_days;
   int demotion_n_min;

   /* KB hybrid retrieval fusion.
    * kb_fusion_mode:        "rrf" (default) | "static_alpha" | "dynamic_alpha".
    * kb_fusion_static_alpha: fixed blend weight for static_alpha mode (default 0.5). */
   char kb_fusion_mode[32];
   double kb_fusion_static_alpha;

   /* KB hybrid ranker.
    * kb_ranker_enabled:      0 = off (default), 1 = use linear ranker after RRF.
    * ranker_fuse_command:    external sidecar for tree/blob models; empty = in-process only.
    * drift_detect_shadow_enabled: 0 = off (default), 1 = observe and emit drift_signal artifacts.
    */
   int kb_ranker_enabled;
   char ranker_fuse_command[512];
   int drift_detect_shadow_enabled;

   /* Learning-to-rank weight fitting (the Calibrate half of the ranker).
    * See docs/proposals/done/learning-to-rank-weight-fitting.md.
    * kb_ranker_fit_enabled:   0 = off (default). Gates `aimee kb ranker fit`
    *                          and any scheduled refit; export-view is always safe.
    * kb_ranker_fit_command:   fitter sidecar command (e.g. "python3 scripts/rank-fit.py");
    *                          empty → fit refuses (never ships an unfit model).
    * kb_ranker_fit_min_groups: floor of labelled groups before a fit is attempted
    *                          (0 → default 8) — the thin-log overfit guardrail.
    * kb_ranker_fit_benchmark: path to the recall-track fixture used as the
    *                          promotion gate (empty → benchmarks/rank/kb_hybrid/queries.json).
    *                          THE DEFAULT IS A PLUMBING SMOKE FIXTURE, NOT A GATE: it
    *                          holds 5 queries of 2-4 candidates, which is below the
    *                          fitter's minimum (RANK_FIT_MIN_BENCH_QUERIES) and so
    *                          refuses every promotion with reason
    *                          "benchmark_underpowered". That refusal is deliberate —
    *                          set this to a real held-out fixture before expecting
    *                          any model to promote.
    * kb_ranker_fit_bench_k:   NDCG cutoff for the gate (0 → default 5).
    * kb_ranker_fit_objective: "pointwise" (default) or "pairwise" — the fitter
    *                          objective. Pairwise learns within-query ordering
    *                          and is robust to positive-skewed labels. */
   int kb_ranker_fit_enabled;
   char kb_ranker_fit_command[512];
   int kb_ranker_fit_min_groups;
   char kb_ranker_fit_benchmark[512];
   int kb_ranker_fit_bench_k;
   char kb_ranker_fit_objective[16];

   /* Graph reasoning (Datalog sidecar).
    * reasoning_datalog_command: path to Datalog evaluator; empty = disabled.
    * reasoning_row_budget:      max derived facts per query (0 = default 10000).
    * reasoning_time_limit_ms:   max ms per query (0 = default 5000). */
   char reasoning_datalog_command[512];
   int reasoning_row_budget;
   int reasoning_time_limit_ms;

   /* Contextual bandits and counterfactual replay (intelligence.bandit.*).
    * bandit_optimize_command: path to Thompson-sampling sidecar; empty = disabled.
    * bandit_exploration_fraction: max fraction of decisions sampled from posterior
    *   for exploration (default 0.05 = 5%).
    * bandit_ipw_weight_cap: cap on IPW importance weights to control variance
    *   (default 10.0).
    * bandit_live_decision_enabled: 0 = off (default), 1 = allow live
    *   kb_bandit_sample() calls from decision points with a closed reward loop.
    *   Static points remain promotion/replay driven until their automatic reward
    *   is validated.
    * bandit_exploration_window_seconds: rolling window the budget Gate enforces
    *   (default 604800 = 7 days). */
   char bandit_optimize_command[512];
   double bandit_exploration_fraction;
   double bandit_ipw_weight_cap;
   int bandit_live_decision_enabled;
   int bandit_exploration_window_seconds;

   /* Deliberate planning (intelligence.planner.*).
    * planner_search_command: path to bounded-MCTS plan-search sidecar; empty = disabled.
    * constraint_solver_command: path to SMT constraint validator (Z3 reference); empty = disabled.
    * planner_budget_default: default MCTS rollout budget for plan_candidate generation (default
    * 32). planner_exploration_constant: UCB1 exploration constant for MCTS (default 1.41). */
   char planner_search_command[512];
   char constraint_solver_command[512];
   int planner_budget_default;
   double planner_exploration_constant;

   /* MDL-guided synthesis selection (intelligence.synthesize.*).
    * kb_mdl_tiebreak_enabled: 1 = use MDL to select within agreement clusters
    *   (default 1); 0 = fall back to attempt-index order.
    * kb_mdl_bump_drift_alert: fraction of MDL disagreements after a prompt bump
    *   that triggers a review flag (default 0.30).
    * kb_synthesize_command: sidecar command for N-attempt synthesis (stdin=JSON, stdout=JSON).
    * kb_synthesize_n_attempts: number of synthesis attempts per evidence bundle (default 3).
    * kb_reflection_synthesis_shadow: 1 = shadow mode for idle-reflection synthesis
    *   (kb_reflection.c) — the LLM runs and its winner is scored and logged as
    *   evidence, but the durable `session_synthesis` proposed candidate is NOT
    *   written (fail-closed, no promotion). 0 = normal (default): the scored
    *   winner is written into the promotion pipeline. Promotion of the shadow
    *   stream to normal is a bandit decision (reflection_synthesis_mode), wired
    *   separately; this flag never flips itself. */
   int kb_mdl_tiebreak_enabled;
   double kb_mdl_bump_drift_alert;
   char kb_synthesize_command[512];
   int kb_synthesize_n_attempts;
   int kb_reflection_synthesis_shadow;

   /* KB background ingest worker pool (kb.worker_count, kb.connection_workers,
    * kb.background_ingest.*). kb_worker_count: aimee-kb in-process KB ingest
    * worker threads (default 2; explicit config may raise it to 8). kb_connection_workers:
    * aimee-kb concurrent connection-handling threads (default 2, max 8). kb_bg_ingest_enabled:
    * 1 = fire timer on startup and every interval (default 1). kb_bg_ingest_interval_hours:
    * hours between automatic full-workspace scans (default 6). kb_bg_watch_enabled: 1 = inotify
    * watch on workspace roots (default 1 on Linux, 0 elsewhere). kb_bg_watch_debounce_secs:
    * minimum seconds between watch-triggered queues per project (default 30). */
   int kb_worker_count;
   /* DB2 connection pool size (db2.connection_pool_size). 0 = default 16. */
   int db2_connection_pool_size;
   int kb_connection_workers;
   int kb_bg_ingest_enabled;
   int kb_bg_ingest_interval_hours;
   int kb_bg_watch_enabled;
   int kb_bg_watch_debounce_secs;
   /* §5 hybrid code retrieval (/v1/code/hybrid): per-signal RRF weights + the RRF
    * rank constant k. Tunable so an operator can favor lexical-code vs structural-
    * graph relevance without a rebuild. Defaults: weights 1.0 (equal), k = 60.
    * A weight <= 0 disables that signal's contribution. See kb_rrf.c / §5. */
   double code_hybrid_weight_code;
   double code_hybrid_weight_graph;
   double code_hybrid_weight_vector;
   double code_hybrid_weight_memory;
   double code_hybrid_rrf_k;
   /* graph-feedback §3 actuation (default off): when set, the /v1/code/hybrid fusion
    * applies the project's earned-trust lessons as an RRF TIE-BREAK ONLY (never moves a
    * candidate across a real score gap). Off ⇒ identical to the pre-§3 ranking. */
   int code_trust_actuation_enabled;
   /* §4: when the LLM-judge-sampled precision of surprising-link structural candidates
    * falls below this floor, an unjudged /v1/code/graph/surprising request suppresses
    * its candidates (they're mostly false positives). 0 (default) disables it. */
   double code_surprising_precision_floor;
   /* embedder-runtime-fetch-autodim §2c: when 0 (default) a recorded-vs-configured
    * embedding-dim mismatch is REFUSED at startup (refuse-and-instruct). When 1, the
    * attended `aimee kb reembed --confirm` reset path is AVAILABLE (it still never
    * auto-runs — the destructive reset requires the explicit confirmed command). */
   int kb_reembed_on_dim_change;

   /* webchat-project-lifecycle slice 2: TTL (seconds) after which a stale
    * project-purge generation fence (kb_runtime_state `project_purging:<key>`)
    * is treated as absent by kb writers. The owning delete operation heartbeats
    * the fence between phases, so only a crash between purge and finalize
    * leaves a fence to expire. JSON key kb.purge_fence_ttl_s (default 900). */
   int kb_purge_fence_ttl_s;

   /* KB maintenance (kb.maintenance.*).
    * kb_maintenance_enabled:        0 = off (default), 1 = run background decay/prune.
    * kb_maintenance_interval_hours: hours between maintenance passes (default 24).
    * kb_maintenance_lambda:         exponential decay rate per day (default 0.005).
    * kb_maintenance_floor:          confidence floor below which rows are pruned (default 0.10).
    * kb_maintenance_min_age_days:   skip rows touched within this many days (default 7).
    * kb_maintenance_orphan_days:    prune orphaned chunks older than this many days (default 90).
    */
   int kb_maintenance_enabled;
   int kb_maintenance_interval_hours;
   double kb_maintenance_lambda;
   double kb_maintenance_floor;
   int kb_maintenance_min_age_days;
   int kb_maintenance_orphan_days;

   /* KB continuous mining (kb.mining.*).
    * kb_mining_enabled:    0 = scheduler off, 1 = run the mining tick loop.
    * kb_mining_min_poll_s: minimum seconds between scheduler ticks (default 300). */
   int kb_mining_enabled;
   int kb_mining_min_poll_s;
   /* kb_mining_failure_learning_enabled (ingress-compression §4, default off):
    * route the recurrence job's failure clusters through the learning pipeline
    * (signal -> reviewed proposal -> Gate-Promote -> artifact) instead of writing
    * a workflow_pattern artifact directly. JSON key kb.mining.failure_learning_enabled. */
   int kb_mining_failure_learning_enabled;

   /* Trigger endpoint (event-triggered-autopilot) */
   char trigger_auth_token[256];
   int trigger_max_concurrent; /* default 2 */
   trigger_rule_t trigger_rules[TRIGGER_RULES_MAX];
   int trigger_rule_count;
   cron_job_t cron_jobs[CRON_JOBS_MAX];
   int cron_job_count;

   /* Learning review / reflection scheduler. Settable under learning.review.*
    * (config_apply_review_settings).
    * review_scheduler_enabled:      1 = run idle reflection (default), 0 = off.
    * review_idle_trigger_minutes:   idle window before reflection fires (default 30).
    * review_session_cooldown_hours: min age of session artifact before reflection
    *                                (default 24; 0 disables the cooldown).
    * review_batch_cap:              max session artifacts per reflection pass (default 10).
    */
   int review_scheduler_enabled;
   int review_idle_trigger_minutes;
   int review_session_cooldown_hours;
   int review_batch_cap;

   /* Deep curator extraction (kb.curator.*).
    * kb_curator_extract_docs_enabled:  default ON (config_kb_curator_defaults). Queues extract_doc
    *   jobs at ingest (kb_http hook) AND via the curator-drain backfill, so docs ingested
    *   through the drain path (kb_doc_refresh) are curated too.
    * kb_curator_extract_code_enabled:  default ON. 1 = queue extract_code_unit jobs.
    * kb_curator_extract_command[512]:  sidecar command (default: scripts/curator-extract.py).
    * kb_curator_extract_max_tokens:    max_tokens per curator completion (default 512).
    * kb_curator_max_attempts:          max drain attempts per job before marking failed (default
    * 3).
    */
   int kb_curator_extract_docs_enabled;
   /* Curator pipeline preset: the everyday knob for the 12 kb_curator_*_enabled
    * stage gates. "off" = no curation, "lite" = core extract+index only, "full"
    * (default) = every stage. Drives the stage gates at config load; an explicit
    * per-stage gate (kb.curator.stages.<stage>.enabled) still overrides. Empty =
    * "full" (back-compat: pre-preset configs). */
   char kb_curator_tier[16];
   /* Curator charter versioning (extract prompt + embed model). A bump to
    * either replays the affected pass on kb startup (kb_curator_version). */
   char kb_curator_extract_prompt_version[64];
   char kb_curator_embed_model_version[64];
   /* Outbound invalidation push: server socket kb pushes curator.invalidated
    * events to on doc invalidation. Empty = disabled (poll the feed). */
   char kb_curator_invalidation_notify_socket[512];
   int kb_curator_extract_code_enabled;
   /* Dedicated extract_code worker threads (kb_curator_extract_code_workers).
    * Default 1 = the shared drain thread's one-unit-per-pass behavior. N>1
    * spawns N-1 additional workers that drain ONLY extract_code jobs in
    * parallel — sized to the synth backend's parallel slots (e.g. 4 workers
    * for a --parallel 4 llama-server), since each job is one LLM sidecar
    * call. Clamped to [1,8]. */
   int kb_curator_extract_code_workers;
   /* Dedicated extract_doc worker threads (kb_curator_extract_docs_workers),
    * exactly as above but for the doc stage. Default 1.
    *
    * Without this, extract_doc has only the shared LLM lane, which spends most
    * of each pass on the other LLM stages — so docs drain at a small fraction
    * of the rate a configured extract_code pool achieves even though both are
    * one sidecar call per job. Clamped to [1,8]. */
   int kb_curator_extract_docs_workers;
   /* resolve_entities pass: 0 = off (default), 1 = commit proposed `entity`
    * mentions into curator_entity_vectors on the curator drain. */
   int kb_curator_resolve_entities_enabled;
   /* narrative indexer: 0 = off (default), 1 = embed proposed doc_summary /
    * synthesis / open_question artifacts into curator_narrative_vectors. */
   int kb_curator_index_narrative_enabled;
   /* claims indexer: 0 = off (default), 1 = embed proposed `claim` artifacts
    * into curator_claim_vectors (subj_attr + value named vectors). */
   int kb_curator_index_claims_enabled;
   /* detect_contradictions: 0 = off (default), 1 = link claims that assert
    * conflicting values for the same subject+attribute. */
   int kb_curator_detect_contradictions_enabled;
   /* code_unit indexer: 0 = off (default), 1 = embed proposed `code_unit`
    * artifacts into curator_code_unit_vectors (intent/signature/body vectors). */
   int kb_curator_index_code_unit_enabled;
   /* link_artifacts bridge: 0 = off (default), 1 = link code_units to the
    * entities their domain_concepts name (doc<->code via the entity graph). */
   int kb_curator_link_artifacts_enabled;
   /* projection_graph build: 1 = on (default) — the drain auto-publishes a fresh
    * code_projection_edges generation per CHANGED project (content-addressed, so
    * unchanged projects are skipped), materializing the typed code graph on
    * `workspace add` with no manual `aimee graph sync-code`. Pure DB2, no sidecar. */
   int kb_curator_projection_graph_enabled;
   /* synthesize_topic: 0 = off (default), 1 = pick a high-centrality entity and
    * emit a `synthesis` narrative via the synthesize sidecar. Requires a
    * configured kb_curator_synthesize_command. */
   int kb_curator_synthesize_enabled;
   /* promote_entity: 0 = off (default), 1 = promote an entity cited by
    * >= promote_min_sources distinct sources one step up the scope lattice
    * (project -> workspace -> global). No sidecar needed (graph + audit only). */
   int kb_curator_promote_entity_enabled;
   int kb_curator_promote_min_sources;
   char kb_curator_extract_command[512];
   /* Comma-separated stage-name order for the curator pipeline (GUI reorder).
    * Empty = registry (default) order. Validated against the dependency DAG; an
    * invalid order falls back to registry order with a WARN. Opt-in: unset changes
    * nothing. See docs/proposals/done/user-configurable-curator-pipeline.md. */
   char kb_curator_stage_order[512];
   /* User-defined curator presets as a JSON array string:
    * [{"name":"...","enabled":["kb_curator_..._enabled",...]}]. Merged with the
    * built-in presets on the curator.stages endpoint; the GUI saves/deletes named
    * profiles here via config.set (flat string, no bespoke op). Empty = none. */
   char kb_curator_user_presets[4096];
   /* User-defined composed curator stages (Phase D) as a JSON array string:
    * [{"name":"...","base_op":"<built-in stage>","budget":N,"enabled":true}].
    * Each recomposes a vetted built-in op (reuses its run fn) under a new
    * name/budget, appended to the built-in registry the lane workers iterate.
    * base_op MUST name a built-in run()-backed stage (no arbitrary code); the
    * custom stage runs on that base op's NATIVE lane — re-laning is disallowed in
    * v1 because two consumers of one queue on two threads would double-drain it
    * (the dequeue is a non-atomic SELECT-then-commit). Invalid entries are skipped
    * with one WARN (fail-safe); unknown fields are ignored (forward-compat).
    * The GUI edits this via config.set (flat string, no bespoke op). Empty = none.
    * See docs/proposals/done/user-configurable-curator-pipeline.md §5. */
   char kb_curator_custom_stages[4096];
   int kb_curator_extract_max_tokens;
   int kb_curator_max_attempts;
   /* Curator LLM provider (curator-llm-backend §2), operator-owned and kb-level
    * (the shared kb cannot borrow a per-user server's credentials). Two-tier:
    *   provider.*  — the default / Tier-A provider (mechanical extract/index
    *                 stages; a small grammar-constrained model suffices).
    *   tier_b.*    — the reasoning/judge stages (judge, resolve_entities,
    *                 detect_contradictions, synthesize, promote_entity). NO weak
    *                 fallback: these stay idle until tier_b is configured, so a
    *                 small model never poisons the graph.
    * An empty base_url means that tier is unconfigured (its stages idle). The
    * api_key is the operator deployment secret; empty => keyless local endpoint.
    * wire is OpenAI-compatible /chat/completions (the only wire today). */
   char kb_curator_provider_base_url[256];
   char kb_curator_provider_model[128];
   char kb_curator_provider_api_key[256];
   /* judge_command: LLM sidecar that adjudicates the resolve_entities
    * [0.70, 0.85) ambiguous band (same_entity? merge : create). Empty = off;
    * with no judge configured, ambiguous mentions fall through to "create". */
   char kb_curator_judge_command[512];
   /* synthesize_command: LLM sidecar that turns a topic + top-K sources into a
    * `synthesis` narrative. Empty = off. synthesize_k: number of source
    * artifacts fed per topic (default 8). */
   char kb_curator_synthesize_command[512];
   int kb_curator_synthesize_k;

   /* Cross-repo dependency graph (kb.curator.cross_repo_graph), proposal
    * docs/proposals/pending/cross-repo-dependency-graph.md. The resolver is
    * query-time (P1); `enabled` gates the cross-repo pass — when 0, the query
    * API/CLI return empty (no resolution) and no review-queue writes occur; existing
    * queue/audit rows are preserved, not pruned. The thresholds are the
    * versioned distinctiveness/tiering knobs (§3.3/§3.4) — bumping any of them must
    * bump distinctiveness_v so a tier decision replays exactly. Defaults are the
    * initial calibration (re-tuned against the S8 fixture corpus). caps/timeout
    * bound the query path (§4.2); review_queue_max bounds the AMBIGUOUS queue (§3.8). */
   int kb_curator_cross_repo_graph_enabled;
   int kb_curator_cross_repo_distinctiveness_v; /* blocked_symbols/threshold model version */
   int kb_curator_cross_repo_k;       /* K: callee in >= K trusted repos -> not distinctive */
   int kb_curator_cross_repo_m;       /* M: defined in >= M trusted repos -> not distinctive */
   int kb_curator_cross_repo_p_pct;   /* P: callee in >= P% of caller A's files -> local method */
   int kb_curator_cross_repo_len_min; /* L: minimum symbol length (code points) */
   int kb_curator_cross_repo_caller_collision_c; /* C: callee in >= C files of A -> one-tier
                                                    downgrade */
   int kb_curator_cross_repo_max_candidates;     /* candidate cap before truncated:true */
   int kb_curator_cross_repo_query_timeout_ms;   /* statement_timeout for the query path */
   int kb_curator_cross_repo_review_queue_max;   /* AMBIGUOUS review-queue cap before eviction */

   /* Evidence embedding (kb.evidence.embed).
    * kb_evidence_embed_enabled: 1 = drain evidence_index_ops and fill
    *   evidence_vectors via the configured embedding_command (default 1; the
    *   builtin embedder needs no external sidecar). 0 = leave ops pending.
    * kb_evidence_embed_batch:   max ops drained per poll (default 32). */
   int kb_evidence_embed_enabled;
   int kb_evidence_embed_batch;

   /* Skill lifecycle and review settings (skills.*). */
   int skills_review_enabled;
   int skills_review_nudge_interval;
   int skills_stale_after_days;
   int skills_archive_after_days;
   int skills_dispatch_enabled;
   int skills_dispatch_max_index;
   int skills_dispatch_advisory;
   int skills_capability_autostub;
   int skills_eval_gate_enabled;
   double skills_eval_threshold;

   /* Worktree garbage collection (`aimee worktree gc` + auto-GC at session
    * start). Worktrees under `<git_root>/.aimee/worktrees/` are temporary
    * by design; the GC removes ones with no commits ahead of base branch
    * AND idle for `worktree_gc_max_age_days`.
    *
    * worktree_gc_enabled:        1 = auto-run at session-start once/day (default), 0 = manual only.
    *                             The AIMEE_WORKTREE_GC env var overrides this either way.
    * worktree_gc_max_age_days:   threshold for "idle"; default 14. Overridable via
    *                             AIMEE_WORKTREE_GC_DAYS.
    */
   int worktree_gc_enabled;
   int worktree_gc_max_age_days;

   /* Auxiliary model routing (auxiliary.*).
    * aux_enabled: 0 = off (default), 1 = route aux tasks to cheap models.
    * aux_default_provider: agent name from agents.json (empty = primary model).
    * aux_default_model: model override for the default provider (empty = agent default).
    * aux_default_max_tokens: token cap for aux calls; 0 = use agent default.
    * aux_tasks: per-task provider/model/token overrides. */
#define CONFIG_AUX_MAX_TASKS 16
   int aux_enabled;
   char aux_default_provider[64];
   char aux_default_model[128];
   int aux_default_max_tokens;
   config_aux_task_t aux_tasks[CONFIG_AUX_MAX_TASKS];
   int aux_task_count;

   /* Model metadata refresh (model_meta.*).
    * model_meta_refresh_minutes: interval for background models.dev cache refresh; default 60.
    * model_meta_capability_routing: 1 = filter by capability flags (default),
    *   0 = cost-tier only. When on, a candidate must satisfy the packet's
    *   required capabilities and minimum context window; when nothing does,
    *   routing escalates to the most capable seat rather than failing.
    */
   /* routing.prefer_local: try FREE local delegates first whenever one is
    * eligible, before falling back to paid remote seats. Off by default.
    *
    * This is an ORDERING preference among agents that already satisfy the packet
    * - never a relaxation of eligibility. A local agent still cannot exceed its
    * declared max_scope: local tokens are free, which removes the COST argument
    * for over-selecting, but not the wall-clock one. A local model failing
    * whole-task work still burns a session, a review and an escalation, and still
    * produces a bad diff. So "free" changes which seat is preferred, not which
    * seats are eligible. */
   int prefer_local_agents;

   int model_meta_refresh_minutes;
   int model_meta_capability_routing;

   /* Vector index strategy (db2.vector.*).
    * corpus_index: "auto"|"hnsw"|"diskann" — default "auto" (behaves as hnsw until threshold).
    * corpus_diskann_threshold: row count per corpus table where auto picks diskann (default 1M). */
   char db2_vector_corpus_index[16];
   int64_t db2_vector_corpus_diskann_threshold;
   /* Mixture-of-Agents ensemble compatibility representation (ensemble.*).
    * ensemble_reference_models: exact seats overlaid from the acquired saved
    * roundtable. When no preset is acquired, runtime discards this legacy list
    * and constructs a provider-diverse panel of at most two agents.
    * ensemble_aggregator: agent name for the synthesis pass.
    * ensemble_min_successful: min references that must succeed before degrading (default 2).
    * ensemble_max_cost_usd: optional per-run cost cap in USD; 0 (or unset) means
    * no limit, which is the default. Set a positive value to cap a run. */
   /* First dim = ENSEMBLE_MAX_REFS (delegate_ensemble.h); a _Static_assert in
    * delegate_ensemble.c enforces they stay in sync. config.h can't include that
    * header (it would cycle), so the literal is kept here. */
   char ensemble_reference_models[CONFIG_ENSEMBLE_MAX_REFS][128];
   int ensemble_reference_count;
   /* Optional per-participant review persona, paired by index with
    * ensemble_reference_models. Empty entries fall back to the engine's diverse
    * default lineup. Width = PERSONA_NAME_MAX (persona.h); first dim =
    * ENSEMBLE_MAX_REFS. */
   char ensemble_reference_personas[CONFIG_ENSEMBLE_MAX_REFS][64];
   int ensemble_reference_persona_count;
   char ensemble_aggregator[128];
   int ensemble_min_successful;
   double ensemble_max_cost_usd;

   /* Agent roundtable (roundtable.*). Participants and aggregator are reused
    * from ensemble.*; these fields control the multi-round loop. */
   int roundtable_max_rounds;
   int roundtable_converge_threshold;
   int roundtable_deadline_ms;
   char roundtable_turns[16];
   /* Name of the active roundtable preset (see roundtable_preset.{c,h}). Empty =
    * no named preset selected; the ensemble and roundtable fields above are used
    * as-is. Setting a preset "active" copies its values into those fields and
    * records the name here so the GUI can show the current selection. */
   char roundtable_default[64];

   /* Roundtable authoring pipeline (roundtable.pipeline_*). The outer
    * REVIEW<->revise loop, done-bar, and cost/pass backstops; see
    * docs/proposals/accepted/agent-roundtable-authoring-pipeline.md section 6. */
   char roundtable_pipeline_done_bar[40];          /* zero_blocking | zero_blocking_suggestions
                                                    * | zero_blocking_questions_answered */
   int roundtable_pipeline_max_passes;             /* 0 = unbounded (default) */
   int roundtable_pipeline_max_attempts_per_pass;  /* infra-retry ceiling, default 2 */
   double roundtable_pipeline_max_cost_usd;        /* per-phase cap; 0 = unbounded */
   double roundtable_pipeline_max_total_cost_usd;  /* whole-pipeline cap; 0 = unbounded */
   int roundtable_pipeline_gate_ttl_h;             /* awaiting-human gate TTL hours; 0 = none */
   int roundtable_pipeline_parked_releases_slot;   /* bool: parked gate frees the active slot */
   int roundtable_pipeline_unknown_context_tokens; /* conservative chunk budget fallback */

   /* Context engine selection (context.engine).
    * Empty string means use the default "compactor" engine. */
   char context_engine[64];
} config_t;

#define CONFIG_LSP_MAX_SERVERS    8
#define CONFIG_LSP_MAX_ARGS       16
#define CONFIG_LSP_MAX_EXTENSIONS 8

/* Convenience alias used by lsp_manager.c */
typedef struct config_lsp_server config_lsp_server_t;

/* Config schema types for validation */
typedef enum
{
   SCHEMA_STRING,
   SCHEMA_INT,
   SCHEMA_BOOL,
   SCHEMA_ARRAY,
   SCHEMA_OBJECT,
   /* A key kept in two shapes for backward compatibility: the current scalar
    * form and a legacy object form the parser still honours. `cross_verify` is
    * one — flat bool now, `{enabled, verify_cmd, role, prompt}` before — and
    * test_config_cross_verify pins that the old form must keep working. A single
    * declared type would make one of the two shapes a validation error. */
   SCHEMA_BOOL_OR_OBJECT
} schema_type_t;

typedef struct
{
   const char *key;
   schema_type_t type;
   int required;
} config_schema_entry_t;

/* Global strict mode flag (set via --strict or AIMEE_STRICT=1) */
extern int g_config_strict;

#include <stdarg.h>
#include <stdio.h>
/* Emit a config-validation diagnostic to stderr (strict-aware: "error" in
 * strict mode, else "warning"); always returns 1 so callers can
 * `issues += config_issue(...)`. Inline -> no link dependency. */
static inline int config_issue(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fprintf(stderr, "aimee: config %s: ", g_config_strict ? "error" : "warning");
   vfprintf(stderr, fmt, ap);
   fprintf(stderr, "\n");
   va_end(ap);
   return 1;
}

/* Load config from default path. Returns defaults if missing.
 * In strict mode, returns -1 on validation errors. */
int config_load(config_t *cfg);

/* Bounded enrolled-bearer operations owned by the config module. Callers get a
 * coherent copy or append one token without naming or copying config_t. */
int config_server_api_bearer_extra_snapshot(char out[][256], int max);
/* There is deliberately NO write half. Enrolled bearers live in Vault, not the
 * config file: config_save persists neither server_api_bearer_token nor
 * server_api_bearer_extra, and config_load MIGRATES any legacy values out to
 * Vault and scrubs them from the struct (config.c). Callers own those secrets
 * through the vault API; the snapshot above is the read view. */

/* Live config snapshot (live-config-reload P1a) — a double-buffer + seqlock holding the
 * current config for immediate, push-driven reload. config_t is a flat POD, so reads are a
 * lock-free struct copy. Additive in P1a: not yet wired into config_load or a push trigger.
 *   config_snapshot_init  — seed the snapshot from a loaded config (once, at startup).
 *   config_snapshot_get   — copy the live snapshot into `out` under a reader pin. -1 if uninit
 *                           or the bounded reader counter is saturated.
 *   config_reload         — re-read the file, VALIDATE-or-keep, and publish only if the
 *                           content-hash token changed (self-reload no-op guard).
 *                           Returns 1 = published, 0 = no-op (unchanged), -1 = kept (bad). */
void config_snapshot_init(const config_t *cfg);

/* Load the config and publish it as the live snapshot in one step, without the
 * caller ever holding a config_t. What the daemons actually want at startup. */
int config_snapshot_seed(void);

/* The accessor primitive: copy `size` bytes at `offset` out of the live config,
 * preferring the pinned snapshot and heap-loading when none is live. Declared here
 * so the module's own hand-written accessors can use it; the generated shards each
 * carry their own local prototype. */
int config_field_read(size_t offset, size_t size, void *dst);

/* dispositions[index].source as a config_disposition_source_t value. Hand-written
 * because the accessor generator skips typedef'd-enum struct members. */
int config_disposition_source(int index);
int config_snapshot_get(config_t *out);

int config_reload(void);

/* config_reload_if_changed — reload iff the config file changed on disk since the last call
 * (out-of-band write: CLI local config.set, manual edit, or autonomous config_save). Poll it
 * from the server's main-loop tick so a file change takes effect without a restart or SIGHUP.
 * First call reconciles unconditionally. Returns config_reload()'s 1/0/-1 on a change, else 0. */
/* Non-zero when the config is readable. Replaces `config_load(&cfg) == 0` used
 * purely as a validity probe -- note this is NOT "the file exists": a missing or
 * unparsable file loads defaults and succeeds. See config.c for the full set of
 * conditions that make it fail. */
int config_present(void);

/* Copy out the sandbox block whole (zeroed when the config cannot be read).
 * See config_save.c for why this one is a struct rather than field accessors. */
void config_sandbox(sandbox_config_t *out);

int config_reload_if_changed(void);

/* Re-applier registry (live-config-reload P3): a hook invoked after config_reload publishes a
 * new snapshot, so it can push bound state (env bridge, log level, TLS, …) live.
 *
 * It used to receive the OLD and NEW config_t so a hook could diff its own section. No
 * registered hook ever did -- both just re-read or invalidate -- and handing out two whole
 * structs to be ignored is the leak this refactor exists to close. The hook now takes
 * nothing: the snapshot is PUBLISHED BEFORE the re-appliers run (see config_reload), so an
 * accessor called inside one already returns the new value. A future hook that genuinely
 * needs the previous value should cache what it cares about on the way past rather than ask
 * for the whole prior config.
 *
 * Register once at startup. Re-appliers run under the reload writer lock: they must be quick
 * and must NOT call config_reload. */
typedef void (*config_reapplier_fn)(void);
void config_reload_register_reapplier(config_reapplier_fn fn);

/* Live autonomy.* accessor (thread-safe) for wfe: for an AIMEE_AUTONOMY_* env NAME that maps
 * to a config field, write the effective value to *out and return 1 — preferring an operator-
 * exported env var, else the live config snapshot. Returns 0 for a non-config autonomy var
 * (e.g. MAX_TURNS) so the caller falls back to its own getenv default. Replaces the unsafe
 * setenv env-bridge for live reload: no cross-thread setenv, wfe reads the seqlock snapshot. */
int config_autonomy_lookup(const char *env_name, long *out);

/* Anti-pattern guardrail escape hatch. Deliberately env-only and NOT a config key, on the
 * web_egress doctrine (src/posix/web_egress.c): a switch that DISABLES a guard must require
 * touching the deployment, not the running config, because config.set is reachable from
 * inside the running system. Config still owns the read, so no consumer calls getenv.
 *
 * Returns 1 only for an explicitly truthy AIMEE_ANTIPATTERNS_BYPASS (1/true/on/yes, any
 * case). This is VALUE-checked, not presence-checked: the guard previously tested
 * `!getenv(...)`, so `AIMEE_ANTIPATTERNS_BYPASS=0` disabled the anti-pattern check — the
 * opposite of what setting 0 means. Unrecognized values fail CLOSED (guard stays on). */
int config_antipatterns_bypass(void);

/* Structured-PDF sidecar endpoints: the config key (tsr_command / ocr_command) if set, else
 * the deployment env var (AIMEE_TSR_URL / AIMEE_OCR_URL), else "" meaning the feature is off.
 * Config owns the precedence so no KB caller reads the environment. Returns a thread-local
 * buffer valid until the next call to the SAME accessor on this thread; never NULL. */
const char *config_tsr_endpoint(void);
const char *config_ocr_endpoint(void);

/* Effective roundtable-module activation: the modules.roundtable tristate resolved against
 * AIMEE_MODULE_ROUNDTABLE via config_module_enabled. Config owns the env read (an invalid
 * value warns and defaults off) so the roundtable module holds no config_t and no getenv. */
int config_module_roundtable_enabled(void);
/* Force a read from DISK, bypassing the live snapshot. Use for a read-modify-save that must
 * reflect the current on-disk file (e.g. config.set) so it never clobbers an external edit,
 * and internally by config_reload. Ordinary readers should use config_load. */
int config_load_file(config_t *cfg);

/* Credential fields are represented in config_t only for runtime compatibility;
 * their writer is injected by the server/KB Vault bootstrap. With no writer
 * installed, credential mutations fail closed instead of reaching aimee.yaml. */
typedef int (*config_secret_writer_fn)(const char *name, const char *value);
typedef int (*config_secret_present_fn)(const char *name);
void config_secret_writer_set(config_secret_writer_fn writer);
int config_secret_store(const char *name, const char *value);
/* Seal credential values found in an older aimee.yaml through `writer`, then
 * remove them from the file. `present` lets the credential store remain
 * authoritative during an idempotent retry. Returns 1 when the file was
 * scrubbed, 0 when it contained no credentials, and -1 on any failure. */
int config_migrate_legacy_credentials(config_secret_writer_fn writer,
                                      config_secret_present_fn present);

/* Economizer policy. SAFE permits only deterministic, mechanically lossless
 * transforms of fresh local content. AGGRESSIVE additionally permits the
 * existing lossy history/tool-result reducers. */
typedef enum
{
   ECON_MODE_OFF = 0,
   ECON_MODE_SAFE = 1,
   ECON_MODE_AGGRESSIVE = 2
} econ_mode_t;

int econ_mode(const config_t *cfg);

/* Live-config forms for callers outside the config module: same policy, read
 * through accessors instead of a caller-held config_t. Prefer these. */
int econ_mode_current(void);
int econ_gateway_mutate_on_current(void);
const char *econ_mode_name(int mode); /* "off"/"safe"/"aggressive" */
int econ_mode_parse(const char *s);   /* mode, or -1 for unknown */

/* Semantic-guardrails escalation mode — the single control that replaced the
 * enabled/dry_run/advisory_only/allow_ml_only_block quad. */
enum
{
   GSEM_MODE_OFF = 0,  /* no assessment runs */
   GSEM_MODE_DRY_RUN,  /* shadow: scored + logged, never changes the outcome */
   GSEM_MODE_ADVISORY, /* warn/prompt advisories; a "block" is downgraded to "prompt" */
   GSEM_MODE_ENFORCE   /* a "block" recommendation hard-blocks the tool */
};
const char *guardrails_semantic_mode_name(int mode); /* "off"/"dry_run"/"advisory"/"enforce" */
int guardrails_semantic_mode_parse(const char *s);   /* string -> GSEM_MODE_* (OFF on unknown) */

/* Reduction gates used by the existing context economizer. */
int econ_reduction_master_on(const config_t *cfg);
int econ_gateway_mutate_on(const config_t *cfg);

/* Resolve whether a pluggable module is enabled, from the layered enablement surface. This is
 * the ONE place module enablement is decided; every wire site calls it with the module's tristate
 * config field and its deprecated env default. Precedence (highest first):
 *   1. admin/governance FORCE (aimee-kb governance state) — NOT YET WIRED; the documented seam
 *      for the future tier that can pin a module on/off over the user's choice.
 *   2. user config-store tristate (`modules:` block): `config_tristate` is 0 or 1 -> honored.
 *   3. deprecated env default (`env_default`, already the module's env-or-default-ON value).
 * `config_tristate` is one of cfg->module_* (-1 = unspecified). Pure: reads its args only. */
int config_module_enabled(int config_tristate, int env_default);

/* Internal policy levers. SAFE enables only json_compact. AGGRESSIVE enables
 * the legacy lossy reducers as well. */
typedef struct
{
   int json_compact;
   int history_fold;
   int compress;
   int command_filter;
   int freeze_guard_horizon; /* break-even reuse horizon for the fold freeze guard */
   int gateway_seam;
   int gateway_session_disable_ttl_ms;
} econ_preset_t;

void econ_preset(const config_t *cfg, econ_preset_t *out);
/* Live-config form; prefer this outside the config module. */
void econ_preset_current(econ_preset_t *out);

/* Resolve the effective operating mode (engineer/novel) for the running
 * process. Precedence: the AIMEE_MODE environment variable (the propagation
 * channel for the ephemeral /novel toggle and spawned delegates) wins; then
 * the persisted mode file (<config dir>/mode, written by `aimee init --novel`);
 * defaulting to AIMEE_MODE_ENGINEER. Defined in config_mode.c. */
aimee_mode_t config_current_mode(void);

/* Raw durable persona NAME (AIMEE_MODE env -> <config dir>/mode -> "engineer").
 * Unlike config_current_mode this preserves arbitrary custom persona names
 * (which the enum collapses to engineer). Writes up to n bytes to out. */
void config_current_persona(char *out, size_t n);

/* Persist the durable operating mode to <config dir>/mode (used by
 * `aimee init --novel`). mode is "engineer" or "novel". Returns 0 on success,
 * -1 on write failure. */
int config_persist_mode(const char *mode);

/* Save config to default path (atomic write via rename). */
int config_save(const config_t *cfg);

/* config_set: the single, surgical config write path (Proposal B). Validates and sets
 * one key in the config YAML document (preserving all other keys), persists, and
 * republishes the snapshot. Returns 0, or -1 on unknown key / invalid value / IO error.
 * Never serialises config_t — no whole-file rebuild, no parse/save drift. */
int config_set(const char *key, const char *value);

/* KB typed-facts group applied together; a negative argument leaves that field
 * unchanged (promote_threshold ignores <= 0). */
int config_set_typed_facts(int enabled, int auto_promote, int promote_threshold);

/* Register a workspace. The cap, the duplicate check and the parallel arrays are
 * config's business. 0 = added, -1 = save failed, -2 = already registered,
 * -3 = table full. provider/remote/head may be NULL for the defaults. */
int config_workspace_add(const char *path, const char *provider, const char *remote,
                         const char *head);
/* Remove by path, closing the gap. 0 = removed, -1 = save failed,
 * -2 = not registered. */
int config_workspace_remove(const char *path);

/* Write the config file out, materialising declared defaults when it does not
 * exist yet. Idempotent. */
int config_persist_defaults(void);

/* Enable the loopback /v1 HTTP listener in one disk-based config transaction.
 * Both values must be positive. Reading the file avoids stale live-snapshot
 * writes inside aimee-server, and the successful update republishes the
 * snapshot before returning. */
int config_set_api_http_listener(int http_port, int rate_limit_per_min);

/* Disable the /v1 HTTP listener and persist. Reads the FILE, not the snapshot --
 * see config_save.c for why this one cannot use the generated setter. */
int config_disable_api_http_listener(void);

/* The ensemble/roundtable settings a preset applies, as plain data this module
 * owns. The caller fills it from whatever its own preset format is; config never
 * learns that format, and this is one persisted write rather than ~16. Widths
 * match the corresponding config fields exactly so a copy cannot truncate
 * differently than the parser would. */
#define CONFIG_RT_PRESET_MAX_SEATS 32
typedef struct
{
   char models[CONFIG_RT_PRESET_MAX_SEATS][128];
   char personas[CONFIG_RT_PRESET_MAX_SEATS][64];
   int seat_count;
   int min_successful;
   double max_cost_usd;
   int max_rounds;
   int converge_threshold;
   int deadline_ms;
   char turns[16];             /* "" leaves the current value */
   char pipeline_done_bar[40]; /* "" leaves the current value */
   int pipeline_max_passes;
   int pipeline_max_attempts_per_pass;
   double pipeline_max_cost_usd;
   double pipeline_max_total_cost_usd;
   int pipeline_gate_ttl_h;
   int pipeline_parked_releases_slot;
   int pipeline_unknown_context_tokens;
   char name[64]; /* recorded as roundtable.default */
} config_roundtable_preset_t;

/* Apply the whole group in one persisted write. Returns 0, -1 on failure. */
int config_apply_roundtable_preset(const config_roundtable_preset_t *p);

/* config_set_concurrency: surgically rewrite the `concurrency:` section of the config
 * YAML from cfg (preserving all other keys) and republish. The structured-write partner
 * to config_set, for the concurrency limits (nested object + per-model/provider arrays). */
int config_set_concurrency(const config_t *cfg);

/* Per-model concurrency, by name. The table layout and cap stay inside config.
 * set: 0 / -1 failed / -2 table full. remove: 0 (absent is success). */
int config_set_model_concurrency(const char *model, int limit);
int config_remove_model_concurrency(const char *model);

/* Default config directory: ~/.config/aimee/ */
const char *config_default_dir(void);

/* Default config path: ~/.config/aimee/aimee.yaml */
/* Is cache-bypass requested (AIMEE_NO_CACHE)?
 *
 * One knob, one meaning -- "do not serve anything from a cache" -- was read with
 * a raw getenv() in seven places across three modules and db2, including two
 * file-identity cache loaders on the request path, so a lookup paid a getenv()
 * per call. Reading it here also puts it where the other 200-plus AIMEE_* knobs
 * belong: behind the config module, not scattered through callers.
 *
 * Deliberately NOT cached itself: tests toggle it between cases and expect the
 * next call to observe the change. getenv() is a pointer chase, not I/O. */
int config_cache_disabled(void);

const char *config_default_path(void);

/* Output directory (same as config dir). */
const char *config_output_dir(void);

/* Effective guardrail mode (defaults to "approve"). */
const char *config_guardrail_mode(void);

/* 1 if `s` is a valid delegate_sandbox_package_access mode (proxy/off/gated/governance). */
int config_sandbox_package_access_valid(const char *s);

/* Resolve the embedding command actually used to embed text. Precedence:
 * a per-call `requested` command (e.g. from a request payload), then the
 * EMBEDDER_URL environment variable, then the configured cfg->embedder_command,
 * then "" when nothing is selected. Either argument may be NULL.
 *
 * EMBEDDER_URL outranks config because it is how the RUNNING embedder announces
 * itself: the kb entrypoint exports it when it starts the bundled in-container
 * model, so a bundled model and an operator's external endpoint arrive by the
 * same route. Callers asking "is an embedder available at all" must come through
 * here for that reason -- the stored field alone is empty on every bundled
 * deployment. This is the single point of truth -- do not re-inline the
 * ternary at call sites. */
const char *config_embedder_command(const config_t *cfg, const char *requested);

/* As config_embedder_command, but the caller never holds a config_t. Prefer
 * this: materialising the ~750 KB struct to read one string is what overflowed
 * the stack in the memory-search path. Result is valid until the next call on
 * this thread. */
const char *config_embedder_command_current(const char *requested);

/* The raw configured value: empty when unset, and ALSO empty whenever the
 * embedder came from EMBEDDER_URL rather than config — which is every bundled
 * deployment. Use this only to answer "did the operator write this key", never
 * "is an embedder available"; for that, call the resolving form above and test
 * for empty. */
const char *config_embedder_command_field(void);

/* Copy one element of a config array out. For callers that pass the whole
 * element onward (scheduler callbacks, the cron executor) rather than reading a
 * single member. The element types are shared domain types; config_t is not.
 * Returns 0 on success, -1 on a bad index or unreadable config. */
int config_cron_job_at(int index, cron_job_t *out);

/* One LSP server entry, copied out. See config_cron_job_at. */
int config_lsp_server_at(int index, config_lsp_server_t *out);

/* One per-model concurrency override, copied out. See config_cron_job_at. */
int config_concurrency_per_model_at(int index, config_concurrency_entry_t *out);

/* One aux-task route, copied out. See config_cron_job_at. */
int config_aux_task_at(int index, config_aux_task_t *out);
int config_trigger_rule_at(int index, trigger_rule_t *out);
int config_mcp_client_at(int index, config_mcp_client_t *out);

/* Opaque boolean accessors — read one flag without naming config_t.
 * Fail closed (0) when the config cannot be loaded. */
int config_audit_worm_enabled(void);
int config_bandit_live_decision_enabled(void);
int config_css_style_graph_enabled(void);
int config_delegate_graph_context_enabled(void);
int config_drift_detect_shadow_enabled(void);
int config_guardrails_blast_radius_advisory_enabled(void);
int config_ingress_usage_accounting_enabled(void);
int config_kb_pdf_vector_enabled(void);
int config_memory_derive_facts_enabled(void);
int config_memory_routing_enabled(void);
int config_transport_kb_pool_enabled(void);
int config_typed_facts_enabled(void);
int config_wfe_live_forge_enabled(void);
double config_memory_semantic_floor_scale(void);
int config_ingress_audit_async(void);

/* Disposition source labels for config reporting. */
const char *config_disposition_source_name(config_disposition_source_t source);

/* Conversation directories for the configured provider. */
int config_conversation_dirs(char dirs[][MAX_PATH_LEN], int max_dirs);

/* Session ID for the current process. Reads CLAUDE_SESSION_ID from env,
 * falls back to a random UUID generated once per process. */
const char *session_id(void);

/* Per-thread override for work running on behalf of another session. */
void session_id_set_override(const char *sid);
void session_id_clear_override(void);

/* True when session_id_set_override bound a real per-session id on this thread
 * (vs. the process-wide PPID fallback session_id() returns otherwise). */
int session_id_override_active(void);

/* Drop the per-thread session_id cache so the next session_id() call re-reads
 * ~/.config/aimee/session-ppid-{ppid}. Long-lived MCP / aimee-server request
 * paths should call this at request entry to pick up rotations performed by
 * `aimee session-start` between requests. No-op when an override is active. */
void session_id_refresh(void);

/* Generated per-field accessors (src/gen_config_accessors.py). Callers outside
 * the config module use these and never name config_t. */
#include "config_accessors.h"

#endif /* DEC_CONFIG_H */
