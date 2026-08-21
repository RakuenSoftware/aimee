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
 * consumer can check its own limit against config's WITHOUT naming the configuration
 * implementation: delegate_ensemble.c used to assert on sizeof(((the configuration implementation
 * *)0)->...), which made the struct's layout part of its interface just to catch dimension drift.
 */
#define CONFIG_ENSEMBLE_MAX_REFS 32

#define AIMEE_API_BEARER_EXTRA_MAX        7
#define CONFIG_DEFAULT_STALE_SESSION_SECS 14400

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
 * caller can be handed one element without holding a the configuration implementation -- see
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

#define CONFIG_LSP_MAX_SERVERS    8
#define CONFIG_LSP_MAX_ARGS       16
#define CONFIG_LSP_MAX_EXTENSIONS 8
#define CONFIG_AUX_MAX_TASKS      16

typedef struct config_lsp_server
{
   char name[64];
   char command[512];
   char args[CONFIG_LSP_MAX_ARGS][256];
   int arg_count;
   char extensions[CONFIG_LSP_MAX_EXTENSIONS][16];
   int extension_count;
} config_lsp_server_t;

typedef void (*config_reapplier_fn)(void);
typedef int (*config_secret_writer_fn)(const char *name, const char *value);

typedef enum
{
   ECON_MODE_OFF = 0,
   ECON_MODE_SAFE = 1,
   ECON_MODE_AGGRESSIVE = 2
} econ_mode_t;

enum
{
   GSEM_MODE_OFF = 0,
   GSEM_MODE_DRY_RUN,
   GSEM_MODE_ADVISORY,
   GSEM_MODE_ENFORCE
};

typedef struct
{
   int json_compact;
   int history_fold;
   int compress;
   int command_filter;
   int freeze_guard_horizon;
   int gateway_seam;
   int gateway_session_disable_ttl_ms;
} econ_preset_t;

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
   char turns[16];
   char pipeline_done_bar[40];
   int pipeline_max_passes;
   int pipeline_max_attempts_per_pass;
   double pipeline_max_cost_usd;
   double pipeline_max_total_cost_usd;
   int pipeline_gate_ttl_h;
   int pipeline_parked_releases_slot;
   int pipeline_unknown_context_tokens;
   char name[64];
} config_roundtable_preset_t;

int config_server_api_bearer_extra_snapshot(char out[][256], int max);
int config_snapshot_seed(void);
int config_reload(void);
int config_present(void);
/* Wait for the required config process to attach to the daemon event bus and
 * answer a valid snapshot. Intended for daemon startup only; ordinary request
 * paths use config_present() without introducing a retry delay. */
int config_wait_ready(unsigned int timeout_ms);
void config_sandbox(sandbox_config_t *out);
int config_reload_if_changed(void);
void config_reload_register_reapplier(config_reapplier_fn fn);
int config_autonomy_lookup(const char *env_name, long *out);
int config_antipatterns_bypass(void);
const char *config_tsr_endpoint(void);
const char *config_ocr_endpoint(void);
int config_module_roundtable_enabled(void);
void config_secret_writer_set(config_secret_writer_fn writer);
int config_secret_store(const char *name, const char *value);
int econ_mode_current(void);
int econ_gateway_mutate_on_current(void);
const char *econ_mode_name(int mode);
int econ_mode_parse(const char *s);
const char *guardrails_semantic_mode_name(int mode);
int guardrails_semantic_mode_parse(const char *s);
int config_module_enabled(int config_tristate, int env_default);
void econ_preset_current(econ_preset_t *out);
aimee_mode_t config_current_mode(void);
void config_current_persona(char *out, size_t n);
int config_persist_mode(const char *mode);
int config_set(const char *key, const char *value);
int config_set_typed_facts(int enabled, int auto_promote, int promote_threshold);
int config_workspace_add(const char *path, const char *provider, const char *remote,
                         const char *head);
int config_workspace_remove(const char *path);
int config_persist_defaults(void);
int config_set_api_http_listener(int http_port, int rate_limit_per_min);
int config_disable_api_http_listener(void);
int config_apply_roundtable_preset(const config_roundtable_preset_t *p);
int config_set_model_concurrency(const char *model, int limit);
int config_remove_model_concurrency(const char *model);
const char *config_default_dir(void);
int config_cache_disabled(void);
const char *config_output_dir(void);
const char *config_guardrail_mode(void);
int config_sandbox_package_access_valid(const char *s);
const char *config_embedder_command_current(const char *requested);
const char *config_embedder_command_field(void);
int config_cron_job_at(int index, cron_job_t *out);
int config_lsp_server_at(int index, config_lsp_server_t *out);
int config_concurrency_per_model_at(int index, config_concurrency_entry_t *out);
int config_aux_task_at(int index, config_aux_task_t *out);
int config_trigger_rule_at(int index, trigger_rule_t *out);
int config_mcp_client_at(int index, config_mcp_client_t *out);
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
const char *config_disposition_source_name(config_disposition_source_t source);
int config_disposition_source(int index);
int config_conversation_dirs(char dirs[][MAX_PATH_LEN], int max_dirs);

const char *session_id(void);
void session_id_set_override(const char *sid);
void session_id_clear_override(void);
int session_id_override_active(void);
void session_id_refresh(void);

#include "config_accessors.h"

#endif /* DEC_CONFIG_H */
