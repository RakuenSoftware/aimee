#ifndef DEC_CONFIG_H
#define DEC_CONFIG_H 1

#include "sandbox.h"
#include "prompts.h" /* aimee_mode_t */
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

/* Concurrency config: per-model and per-provider overrides */
#define CONFIG_CONCURRENCY_KEY_LEN     128
#define CONFIG_CONCURRENCY_MAX_ENTRIES 16

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

typedef struct
{
   char name[64];
   config_mcp_transport_t transport;
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

/* Trigger rule (from trigger_rules YAML list) */
#define TRIGGER_RULE_MAX_SOURCE   64
#define TRIGGER_RULE_MAX_EVENT    256
#define TRIGGER_RULE_MAX_SCHEDULE 64
#define TRIGGER_RULE_MAX_TEMPLATE 128
#define TRIGGER_RULE_MAX_WS       256
#define TRIGGER_RULES_MAX         32

typedef struct
{
   char source[TRIGGER_RULE_MAX_SOURCE];     /* "github-webhook", "ci-webhook", "cron" */
   char event[TRIGGER_RULE_MAX_EVENT];       /* event pattern to match */
   char schedule[TRIGGER_RULE_MAX_SCHEDULE]; /* cron expression (source=cron only) */
   char pipeline_template[TRIGGER_RULE_MAX_TEMPLATE];
   char workspace[TRIGGER_RULE_MAX_WS];
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
 * + pgvector). See docs/proposals/done/three-db-split-user-shared-vectors.md
 * for the original split; the DB3 vector tier was folded into DB2 via
 * pgvector in #1575. */
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
   char workspace_vcs_head[64][64];
   int workspace_count;
   char guardrail_mode[16];
   char provider[16];

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
   char embedding_command[512];
   char embedding_model[128];
   char embedding_endpoint[512];
   int embedding_dim;
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
   /* wfe_live_forge_enabled: gate (default 0, OFF) for the autonomous live forge
    * (full-autonomous-development F4). When OFF the wfe forge provider is not
    * registered and every forge op fails closed, so an autonomous run can never
    * open or merge a real PR. Turning it ON is a deliberate operator deployment
    * action (branch protection + scoped creds + break-glass) — never a code default,
    * per the security-roundtable deviation. */
   int wfe_live_forge_enabled;
   /* audit_action_enabled: emit a per-tool-call governed-action row (kind=
    * tool_action) to audit.log from pre_tool_check. Default-ON (the
    * trajectory_export reader shipped, so the rows are consumable); audit is
    * passive, fail-open, and never changes an enforcement verdict. Set false to
    * opt out. */
   int audit_action_enabled;
   /* memory_md_retire: retire the agent file-memory surface into aimee. When on,
    * a Write under ~/.claude/projects/<slug>/memory/<name>.md is intercepted into
    * aimee's db1 (kind='archive', non-recallable) and the .md is NEVER
    * materialized; Edit/MultiEdit/MEMORY.md/Bash-writes to the memory dir are
    * rejected with guidance; and session-start skips .md hydration. Retrieval is
    * via `aimee memory search` (the session brief steers there). Fail-open (spill
    * for reconcile) on a store outage. Default-ON: aimee is the single memory of
    * record. Set false to keep the legacy re-materialized .md mirrors. */
   int memory_md_retire;
   /* audit_worm_enabled: dual-write each governed-action audit row into the
    * per-service WORM store (append-only, hash-chained SQLite) alongside the
    * legacy audit.log. Default-OFF (S0 of the WORM audit-store proposal); the
    * WORM store is not yet authoritative, so a failed WORM append is recoverable
    * audit loss, never an enforcement change. */
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
   /* Session-isolation guard (opt-in): when on, the PreToolUse attention-guard
    * fails closed on a mutating tool whose target is NOT inside an aimee-managed
    * worktree (.aimee/worktrees/...), forcing every mutating session into an
    * isolated worktree+branch off the default branch. Default 0 (off). */
   int require_session_worktree;

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
   int typed_facts_enabled;         /* typed-fact knowledge layer master gate (default off) */
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
    *   include when it has a source_session (0 = off, default 0; 1–3 recommended).
    * memory_kb_neighbour_expand: if 1, expand KB hits to include prev/next chunk
    *   under a budget (default 0). */
   int memory_window_radius;
   int memory_kb_neighbour_expand;

   /* Upper bound on results returned by `aimee kb search` / kb_search().
    * Requests with --max N above this value are silently clamped. Default 50;
    * internal buffers allow up to MAX_LEXICAL_RESULTS + MAX_VEC_RESULTS. */
   int kb_search_max_results;

   /* Negation and explicit-absence memory.
    * memory_negation_enabled: 0 = disabled (default), 1 = enabled.
    *   When enabled, insert/update computes not_<token> synthetic terms and
    *   writes them to the negation_tokens column; negated queries also use
    *   negation lexical matching for negative facts. */
   int memory_negation_enabled;

   /* Cross-encoder reranker: second-pass (query, candidate) scoring.
    * memory_rerank_enabled: 0 = disabled (default), 1 = enabled.
    * memory_rerank_command: external command for cross-encoder scores (stdin: JSON pairs,
    *   stdout: JSON float array). Empty = disabled.
    * memory_rerank_top_k: candidate pool size fed to cross-encoder (default 50).
    * memory_rerank_mix: blend weight for cross-encoder vs hybrid (0.0–1.0, default 0.7).
    * memory_query_expansion_mode: "lexical" (default) or "semantic" (embedding-based).
    * memory_query_expansion_k: number of near-neighbour terms to inject (default 5). */
   int memory_rerank_enabled;
   char memory_rerank_command[512];
   int memory_rerank_top_k;
   double memory_rerank_mix;
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
    * memory_scenes_min_cluster_size: minimum turns per scene (default 3).
    * memory_scenes_top_m: top-M scenes to boost during retrieval (default 3).
    * memory_scenes_global_escape_ratio: fraction of results from global pool (default 0.2). */
   int memory_scenes_enabled;
   int memory_scenes_min_cluster_size;
   int memory_scenes_top_m;
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

   /* Allow Claude run via the `claude` CLI / tmux login (authenticated by the
    * interactive Claude subscription login, NOT an API key) to be used as a
    * DELEGATE. Default 0: Claude-via-CLI is primary-only, because driving a
    * personal Claude subscription as an automated delegate may violate
    * Anthropic's terms and risk account action. Set to 1 to opt in (see
    * DELEGATES.md). Does not affect API-key/HTTP agents or other CLI agents. */
   int claude_cli_delegate_enabled;
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
    */
   char search_backend[32];
   int search_max_results;
   char search_searxng_url[512];
   char search_tavily_api_key[256];

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

   /* Unified context economizer (src/context_reduce.c). The single reduction
    * subsystem invoked at every request-assembly seam. All default-off. Knobs are
    * added here as each slice implements them, so an exposed flag is always
    * honored. Currently the measurement engine (delegate seam, measure-only):
    * reduce_measure_enabled: collect baseline/opportunity ledger rows (no
    *   mutation) — the shadow mode that powers the cost numbers.
    * reduce_delegate_seam: enable the economizer at the delegate turn loop.
    * reduce_history_fold: actually fold old history (rolling skeleton + Coordinate
    *   Closet) at the delegate seam — the first real reduction lever (default-off).
    * reduce_compress: boundary-free tool-result BODY compression at the delegate
    *   seam (Slice 4). Shrinks oversized tool-result bodies in place (identifiers
    *   conserved in the Coordinate Closet) WITHOUT needing a clean-user-turn
    *   boundary, so it engages on autonomous tool-loops where history_fold can't.
    *   Provider-native output (valid for all builders incl. chatgpt). Default-off.
    * reduce_gateway_seam: enable the economizer at the inbound /v1 gateway seam.
    *   Shadow-mode only in this slice (measure_only is forced on at the gateway, so
    *   it NEVER mutates the live client request) — collects baselines on live
    *   ingress traffic with zero behavior change.
    * reduce_freeze_guard_enabled: freeze cost guardrail (Slice 5). Pin the fold
    *   boundary (cache-warm reduced prefix) only when the estimated cache-read
    *   savings over reduce_freeze_guard_horizon reuses cover the one-time cache-write
    *   churn; otherwise re-derive without pinning. Default-ON, but only acts when the
    *   (default-off) economizer freeze is live, so no default-path behavior change.
    * reduce_freeze_guard_horizon: expected reuse turns for the break-even estimate
    *   (0 -> 1; clamped to FREEZE_GUARD_MAX_HORIZON).
    * reduce_gateway_mutate: APPLY the reduction to the live inbound /v1 request (the
    *   primary-agent path) instead of only measuring it — compress-only, behind a
    *   per-session circuit breaker (msg_session_disable), buffered 4xx-restore-resend +
    *   streaming disable-subsequent-turns. Default-OFF (the whole gateway-mutation
    *   feature). mutate=1 implies gateway_seam=1: config_load auto-enables the shadow
    *   seam IN MEMORY (never rewriting the file) + logs one WARN, so the shadow baseline
    *   the validation gates compare against always exists.
    * reduce_gateway_session_disable_ttl_ms: how long a circuit-broken session stays
    *   disabled (default 3600000 = 1h). MUST be > 0 — 0/negative is a startup-fatal
    *   config error (a permanent-pin/breaker-off knob on a live path is disproportionate
    *   runtime risk); validated by config_reduce_validate() at server startup.
    * (Other per-lever gates and min_gain land in later slices.) */
   int reduce_measure_enabled;
   int reduce_delegate_seam;
   int reduce_history_fold;
   int reduce_compress;
   int reduce_gateway_seam;
   int reduce_freeze_guard_enabled;
   int reduce_freeze_guard_horizon;
   int reduce_gateway_mutate;
   int reduce_gateway_session_disable_ttl_ms;
   /* reduce_command_filter: deterministic command-aware tool-output condensation lever.
    * DEFAULT-ON (unified-economizer P1c): a safe-tier lever — lossless-on-demand (full
    * output spilled), fail-open, and no-over-reduction (failures/diagnostics + their detail
    * block kept in the condensed view). When on, tool_bash captures up to TOOL_CONDENSE_
    * CEILING (2 MB) so the full output reaches the lever, replacing the old lossy 32 KB
    * read-cap truncation. Set reduce.command_filter=false to opt out. */
   int reduce_command_filter;
   /* Runtime-only (NOT parsed as a key, NOT persisted): 1 iff the operator set the
    * reduce.gateway_seam key explicitly. Lets config_save persist gateway_seam only
    * when the user chose it, never when config_load synthesized it from mutate=1. */
   int reduce_gateway_seam_explicit;

   /* Unified-economizer TWO-TIER switches (P3). These GATE the individual reduce.* levers
    * without mutating them (resolved by the econ_* accessors, never surprising config_save):
    *   economizer.enabled   — MASTER for all reduction ACTIONS. DEFAULT-ON. false = one
    *                          off-switch: every reducer is forced off, but measure/telemetry
    *                          keeps running (reports zero — proving the kill works). The safe
    *                          tier (delegate-seam: command_filter/history_fold/compress) is on
    *                          under it by their own defaults.
    *   economizer.aggressive — opt-in ceiling for the AGGRESSIVE tier (live PRIMARY /v1
    *                          mutation: gateway_mutate). DEFAULT-OFF. gateway_mutate requires
    *                          enabled && aggressive && reduce.gateway_mutate (all three) — the
    *                          aggressive flag alone never activates a live-traffic mutator. */
   int economizer_enabled;
   int economizer_aggressive;

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

   /* Ecomode: prefer lowest-cost agents for all tasks when enabled.
    * 0 = off (default), 1 = on.
    * In ecomode, routing skips the default agent and always picks
    * the cheapest enabled agent capable of the requested role. */
   int ecomode;

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
    * dedup_enabled: 0 = off (default), 1 = on. When on, a re-sent identical
    * buffered, non-streaming, tool-free completion carrying an explicit
    * Idempotency-Key is served from a small TTL cache instead of paying for the
    * provider call again; the avoided call is recorded as usage_kind=avoided
    * (not spend). Strictly per-principal: the cache key carries the account
    * boundary so one caller never reads another's response. */
   int dedup_enabled;

   /* Cache-aware request shaping for the aimee-owned Anthropic path (§3).
    * cache_shaping_enabled: 0 = off (default), 1 = on. When on, the tool-bearing
    * Anthropic request builder marks the aimee-owned system prefix cacheable
    * (cache_control: ephemeral content block), matching the no-tools path, so the
    * provider caches the stable system prompt across calls. Anthropic-only; never
    * alters the stateless /v1/messages proxy or any non-Anthropic provider. */
   int cache_shaping_enabled;

   /* Ingress cost-accounting rollout knobs (ingress.*; §2/§4/#3).
    * ingress_usage_accounting_enabled: 0 = off (default), 1 = on. Master gate for
    *   writing ingress cost rows (OpenAI/Codex + Anthropic /v1/messages + /v1/runs).
    *   Off by default per the flag-rollout program; flip to begin accounting.
    * ingress_audit_async: 0 = write the cost row inline (default), 1 = hand it to
    *   a background writer so the response is not blocked on the DB insert.
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
    * semantic_enabled: 0 = off (default), 1 = on.
    * semantic_dry_run: 1 = shadow mode — score is logged but never changes outcome
    *   (default 1; always start here).
    * semantic_command: external sidecar command (stdin: JSON request, stdout: JSON response).
    *   Empty (default) disables sidecar invocation; no assessment runs when empty.
    * semantic_advisory_only: 1 = semantic prompt/block scores produce warnings only
    *   (default 1; Phase 3 explicit confirmation/blocking remains gated).
    * semantic_warn_threshold, prompt_threshold, block_threshold: score bands.
    * semantic_allow_ml_only_block: false by default; requires positive precision evidence.
    * See docs/proposals/accepted/neural-assisted-guardrails.md. */
   int guardrails_semantic_enabled;
   int guardrails_semantic_dry_run;
   int guardrails_semantic_advisory_only;
   char guardrails_semantic_command[512];
   double guardrails_semantic_warn_threshold;
   double guardrails_semantic_prompt_threshold;
   double guardrails_semantic_block_threshold;
   int guardrails_semantic_allow_ml_only_block;
   /* §7 code-graph actuation: surface a structural blast-radius advisory (the
    * graph-impacted dependent files) before an edit. Advisory + fail-open;
    * default off (opt-in). See docs/proposals/pending/code-graph-intelligence.md §7. */
   int guardrails_blast_radius_advisory_enabled;

   /* aimee-kb public HTTP API (kb.api.*).
    * kb_api_http_port: TCP port for the /v1/... REST API (0 = disabled, default).
    * kb_api_bearer_token: static bearer token for API auth (empty = no auth).
    *   May be self-describing for scoped access:
    *     scope:<kind>:<id>:<secret>   — only authorizes requests in that scope
    *                                     (e.g. scope:project:foo:s3cr3t); cross-
    *                                     scope requests get 403. See kb_scope.h.
    *     <secret>                     — unscoped/admin token (full access). */
   int kb_api_http_port;
   char kb_api_bearer_token[256];

   /* Remote aimee-kb client pointer (used when this host does NOT run a local
    * aimee-kb sidecar). When set, aimee-server exports these into its own
    * environment at startup so the env-based kb_client transport
    * (kb_client_v1_base_url / kb_client_v1_auth_header) reaches the remote kb.
    * Distinct from kb_api_bearer_token above, which is the LOCAL kb server's
    * inbound-auth token. The AIMEE_KB_API_URL / AIMEE_KB_API_BEARER_TOKEN env
    * vars still take precedence over these when set. */
   char kb_client_url[CONFIG_DB2_URL_LEN];
   char kb_client_bearer_token[256];

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
    * kb_synthesize_n_attempts: number of synthesis attempts per evidence bundle (default 3). */
   int kb_mdl_tiebreak_enabled;
   double kb_mdl_bump_drift_alert;
   char kb_synthesize_command[512];
   int kb_synthesize_n_attempts;

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

   /* Learning review / reflection scheduler (learning.review.*).
    * review_scheduler_enabled:      0 = off (default), 1 = run idle reflection.
    * review_idle_trigger_minutes:   idle window before reflection fires (default 30).
    * review_session_cooldown_hours: min age of session artifact before reflection (default 24).
    * review_batch_cap:              max session artifacts per reflection pass (default 10).
    */
   int review_scheduler_enabled;
   int review_idle_trigger_minutes;
   int review_session_cooldown_hours;
   int review_batch_cap;

   /* Deep curator extraction (kb.curator.*).
    * kb_curator_extract_docs_enabled:  0 = off (default), 1 = queue extract_doc jobs post-ingest.
    * kb_curator_extract_code_enabled:  0 = off (default), 1 = queue extract_code_unit jobs.
    * kb_curator_extract_command[512]:  sidecar command (default: scripts/curator-extract.py).
    * kb_curator_extract_max_tokens:    max_tokens per job stdin payload (default 2048).
    * kb_curator_max_attempts:          max drain attempts per job before marking failed (default
    * 3).
    */
   int kb_curator_extract_docs_enabled;
   /* Curator charter versioning (extract prompt + embed model). A bump to
    * either replays the affected pass on kb startup (kb_curator_version). */
   char kb_curator_extract_prompt_version[64];
   char kb_curator_embed_model_version[64];
   /* Outbound invalidation push: server socket kb pushes curator.invalidated
    * events to on doc invalidation. Empty = disabled (poll the feed). */
   char kb_curator_invalidation_notify_socket[512];
   int kb_curator_extract_code_enabled;
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
   char kb_curator_tier_b_base_url[256];
   char kb_curator_tier_b_model[128];
   char kb_curator_tier_b_api_key[256];
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
   int skills_curator_enabled;
   int skills_curator_interval_hours;
   int skills_stale_after_days;
   int skills_archive_after_days;
   int skills_min_idle_minutes;
   int skills_manage_enabled;
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
#define CONFIG_AUX_MAX_TASKS     16
#define CONFIG_AUX_TASK_NAME_LEN 64
   int aux_enabled;
   char aux_default_provider[64];
   char aux_default_model[128];
   int aux_default_max_tokens;
   struct
   {
      char task[CONFIG_AUX_TASK_NAME_LEN];
      char provider[64];
      char model[128];
      int max_tokens;
   } aux_tasks[CONFIG_AUX_MAX_TASKS];
   int aux_task_count;

   /* Model metadata refresh (model_meta.*).
    * model_meta_refresh_minutes: interval for background models.dev cache refresh; default 60.
    * model_meta_capability_routing: 0 = cost-tier only (default), 1 = filter by capability flags.
    */
   int model_meta_refresh_minutes;
   int model_meta_capability_routing;

   /* Vector index strategy (db2.vector.*).
    * corpus_index: "auto"|"hnsw"|"diskann" — default "auto" (behaves as hnsw until threshold).
    * corpus_diskann_threshold: row count per corpus table where auto picks diskann (default 1M). */
   char db2_vector_corpus_index[16];
   int64_t db2_vector_corpus_diskann_threshold;
   /* Mixture-of-Agents ensemble (ensemble.*).
    * ensemble_reference_models: diverse model/agent names for the fan-out.
    * ensemble_aggregator: agent name for the synthesis pass.
    * ensemble_min_successful: min references that must succeed before degrading (default 2).
    * ensemble_max_cost_usd: optional per-run cost cap in USD; 0 (or unset) means
    * no limit, which is the default. Set a positive value to cap a run. */
   /* First dim = ENSEMBLE_MAX_REFS (delegate_ensemble.h); a _Static_assert in
    * delegate_ensemble.c enforces they stay in sync. config.h can't include that
    * header (it would cycle), so the literal is kept here. */
   char ensemble_reference_models[32][128];
   int ensemble_reference_count;
   /* Optional per-participant review persona, paired by index with
    * ensemble_reference_models. Empty entries fall back to the engine's diverse
    * default lineup. Width = PERSONA_NAME_MAX (persona.h); first dim =
    * ENSEMBLE_MAX_REFS. */
   char ensemble_reference_personas[32][64];
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

/* Parse plugin extension config keys (context.engine, etc.) that were
 * excluded from config_load() due to file-size constraints.
 * Call after config_load() in server startup. */
void config_load_plugin_extensions(config_t *cfg);

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
   SCHEMA_OBJECT
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

/* Live config snapshot (live-config-reload P1a) — a double-buffer + seqlock holding the
 * current config for immediate, push-driven reload. config_t is a flat POD, so reads are a
 * lock-free struct copy. Additive in P1a: not yet wired into config_load or a push trigger.
 *   config_snapshot_init  — seed the snapshot from a loaded config (once, at startup).
 *   config_snapshot_get   — copy the live snapshot into `out` (seqlock read). -1 if uninit.
 *   config_reload         — re-read the file, VALIDATE-or-keep, and publish only if the
 *                           content-hash token changed (self-reload no-op guard).
 *                           Returns 1 = published, 0 = no-op (unchanged), -1 = kept (bad). */
void config_snapshot_init(const config_t *cfg);
int config_snapshot_get(config_t *out);
int config_reload(void);

/* Re-applier registry (live-config-reload P3): a hook invoked after config_reload publishes a
 * new snapshot, receiving the OLD and NEW config so it can push bound state (env bridge, log
 * level, TLS, …) live when the section it owns changed. Register once at startup. Re-appliers
 * run under the reload writer lock: they must be quick and must NOT call config_reload. */
typedef void (*config_reapplier_fn)(const config_t *old_cfg, const config_t *new_cfg);
void config_reload_register_reapplier(config_reapplier_fn fn);

/* Live autonomy.* accessor (thread-safe) for wfe: for an AIMEE_AUTONOMY_* env NAME that maps
 * to a config field, write the effective value to *out and return 1 — preferring an operator-
 * exported env var, else the live config snapshot. Returns 0 for a non-config autonomy var
 * (e.g. MAX_TURNS) so the caller falls back to its own getenv default. Replaces the unsafe
 * setenv env-bridge for live reload: no cross-thread setenv, wfe reads the seqlock snapshot. */
int config_autonomy_lookup(const char *env_name, long *out);
/* Force a read from DISK, bypassing the live snapshot. Use for a read-modify-save that must
 * reflect the current on-disk file (e.g. config.set) so it never clobbers an external edit,
 * and internally by config_reload. Ordinary readers should use config_load. */
int config_load_file(config_t *cfg);

/* Two-tier economizer resolution (P3) — compute EFFECTIVE lever states without mutating
 * config_t (so config_save always round-trips the raw user values). Precedence:
 * master-kill (economizer.enabled) > aggressive-tier ceiling > individual reduce.* default. */
int econ_reduction_master_on(const config_t *cfg); /* economizer.enabled (measure is exempt) */
int econ_gateway_mutate_on(const config_t *cfg);   /* enabled && aggressive && gateway_mutate */

/* Validate the economizer gateway-mutation invariants that are STARTUP-FATAL (as
 * opposed to the in-memory normalizations config_load already applied). Currently:
 * reduce_gateway_session_disable_ttl_ms must be > 0. Returns 0 if valid, non-zero
 * if the live gateway-mutation path must not start; on failure writes a human
 * message into err (when err != NULL). Called at server startup so a bad value
 * refuses to bring up the live /v1 path, without breaking unrelated CLI callers
 * of config_load. Pure (reads cfg only). */
int config_reduce_validate(const config_t *cfg, char *err, size_t errlen);

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

/* Default config directory: ~/.config/aimee/ */
const char *config_default_dir(void);

/* Default config path: ~/.config/aimee/aimee.yaml */
const char *config_default_path(void);

/* Output directory (same as config dir). */
const char *config_output_dir(void);

/* Effective guardrail mode (defaults to "approve"). */
const char *config_guardrail_mode(const config_t *cfg);

/* Resolve the embedding command actually used to embed text. Precedence:
 * a per-call `requested` command (e.g. from a request payload), then the
 * configured cfg->embedding_command, then "builtin". Either argument may be
 * NULL. "builtin" is a 384-dim deterministic hash that real halfvec(1024)/
 * (2560) columns reject, so it is only correct in a 384-dim shim/test setup;
 * every production path must carry a real embedder via config or request. This
 * is the single point of truth -- do not re-inline the ternary at call sites. */
const char *config_embedding_command(const config_t *cfg, const char *requested);

/* Disposition source labels for config reporting. */
const char *config_disposition_source_name(config_disposition_source_t source);

/* Conversation directories for the configured provider. */
int config_conversation_dirs(const config_t *cfg, char dirs[][MAX_PATH_LEN], int max_dirs);

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

#endif /* DEC_CONFIG_H */
