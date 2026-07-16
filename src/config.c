/* config.c: app configuration loading/saving (~/.config/aimee/aimee.yaml).
 *
 * The on-disk format is YAML; the in-memory model is still cJSON. The
 * yaml.c shim handles parse/emit so this file's schema-extraction code
 * never sees the format change. */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include "aimee.h"
#include "json_fluent.h"
#include "aimee_home.h"
#include "config_database.h"
#include "config_internal.h"
#include "config_sections.h"
#include "config_learning.h"
#include "config_memory.h"
#include "db1_optional.h"
#include "maintenance.h"
#include "platform_process.h"
#include "platform_path.h"
#include "sandbox.h"
#include "toolset.h"
#include "cJSON.h"
#include "yaml.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__GNUC__)
extern void toolset_registry_init(toolset_registry_t *registry) __attribute__((weak));
extern int toolset_registry_load_file(toolset_registry_t *registry, const char *path, char *err,
                                      size_t err_len) __attribute__((weak));
#endif

__thread int g_aimee_compute_threads_override = 0;

static __thread char g_session_override[64];

static __thread int g_session_id_drop;
void session_id_refresh(void)
{
   if (!g_session_override[0])
      g_session_id_drop = 1;
}

const char *session_id(void)
{
   static __thread char id[64];
   if (g_session_override[0])
      return g_session_override;
   if (g_session_id_drop)
   {
      id[0] = '\0';
      g_session_id_drop = 0;
   }
   if (id[0])
      return id;

   /* Processes in an agent session share a PPID; key session-id by it so hooks,
    * MCP server, and delegates align. ppid<=1 = orphaned; treat as no-session
    * (would otherwise collide across unrelated daemons via session-ppid-1). */
   int ppid = (int)platform_getppid();
   if (ppid > 1)
   {
      char path[512];
      const char *base = aimee_home();
      if (base)
      {
         snprintf(path, sizeof(path), "%s/session-ppid-%d", base, ppid);
         FILE *fp = fopen(path, "r");
         if (fp)
         {
            if (fgets(id, sizeof(id), fp))
            {
               size_t len = strlen(id);
               while (len > 0 && (id[len - 1] == '\n' || id[len - 1] == '\r' || id[len - 1] == ' '))
                  id[--len] = '\0';
               if (id[0])
               {
                  fclose(fp);
                  return id;
               }
            }
            fclose(fp);
         }
      }
   }

   /* Generate new aimee session ID and persist atomically for sibling processes */
   unsigned char buf[16];
   if (platform_random_bytes(buf, sizeof(buf)) != 0)
      memset(buf, 0, sizeof(buf));
   snprintf(id, sizeof(id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10],
            buf[11], buf[12], buf[13], buf[14], buf[15]);

   if (ppid > 1)
   {
      char path[512];
      const char *base = aimee_home();
      if (base)
      {
         snprintf(path, sizeof(path), "%s/session-ppid-%d", base, ppid);
         int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
         if (fd >= 0)
         {
            (void)write(fd, id, strlen(id));
            close(fd);
         }
         else
         {
            /* Another process won the race — read their ID */
            FILE *fp = fopen(path, "r");
            if (fp)
            {
               char tmp[64] = "";
               if (fgets(tmp, sizeof(tmp), fp))
               {
                  size_t len = strlen(tmp);
                  while (len > 0 &&
                         (tmp[len - 1] == '\n' || tmp[len - 1] == '\r' || tmp[len - 1] == ' '))
                     tmp[--len] = '\0';
                  if (tmp[0])
                     snprintf(id, sizeof(id), "%s", tmp);
               }
               fclose(fp);
            }
         }
      }
   }

   return id;
}

void session_id_set_override(const char *sid)
{
   if (!sid || !sid[0])
   {
      g_session_override[0] = '\0';
      return;
   }
   snprintf(g_session_override, sizeof(g_session_override), "%s", sid);
}

void session_id_clear_override(void)
{
   g_session_override[0] = '\0';
}

/* True when a real per-session id has been bound on this thread via
 * session_id_set_override. Callers that key a shared resource on session_id()
 * (e.g. the tmux CLI session pane) use this to tell a genuine per-session id
 * apart from the process-wide PPID fallback, which is the SAME value for every
 * override-less turn in the process and would otherwise collapse them all onto
 * one pane. */
int session_id_override_active(void)
{
   return g_session_override[0] != '\0';
}

const char *config_default_dir(void)
{
   /* Routes through aimee_home() so AIMEE_HOME / AIMEE_PROFILE
    * overrides apply. Falls back to /tmp/aimee when neither is
    * usable (broken environment) so the legacy behaviour of
    * returning a non-NULL path is preserved. */
   const char *base = aimee_home();
   if (base)
      return base;
   return "/tmp/.config/aimee";
}

const char *config_default_path(void)
{
   /* Thread-local: returned-pointer scratch reachable from config_load() on
    * several concurrent kb worker threads (TSan data race otherwise). */
   static __thread char path[MAX_PATH_LEN];
   static __thread char cached_dir[MAX_PATH_LEN];
   const char *dir = config_default_dir();

   if (path[0] && strcmp(cached_dir, dir) == 0)
      return path;

   snprintf(path, sizeof(path), "%s/aimee.yaml", dir);
   snprintf(cached_dir, sizeof(cached_dir), "%s", dir);
   return path;
}

const char *config_output_dir(void)
{
   /* Thread-local returned-pointer scratch; see config_default_path(). */
   static __thread char fallback[MAX_PATH_LEN];
   const char *dir = config_default_dir();

   if (dir && platform_mkdir_p(dir, 0700) == 0 && access(dir, W_OK) == 0)
      return dir;

   const char *tmp = getenv("TMPDIR");
   if (!tmp || !tmp[0])
      tmp = getenv("TEMP");
   if (!tmp || !tmp[0])
      tmp = getenv("TMP");
   if (!tmp || !tmp[0])
      tmp = "/tmp";

   snprintf(fallback, sizeof(fallback), "%s/aimee", tmp);
   platform_mkdir_p(fallback, 0700);
   return fallback;
}

const char *config_default_db1_path(void)
{
   if (db1_default_path)
      return db1_default_path();

   /* Thread-local returned-pointer scratch; see config_default_path(). */
   static __thread char path[MAX_PATH_LEN];
   const char *dir = config_default_dir();
   snprintf(path, sizeof(path), "%s/aimee.db", dir ? dir : "/tmp");
   return path;
}

/* AIMEE_STAT_MTIM macro and g_config_cache / g_config_mtime / g_config_cached
 * are declared in config_internal.h so config_save.c can stamp the cache
 * after a write; defined below. */
config_t g_config_cache;
struct timespec g_config_mtime;
off_t g_config_size;
ino_t g_config_ino;
char g_config_cache_path[MAX_PATH_LEN];
int g_config_cached;

static int timespec_eq(const struct timespec *a, const struct timespec *b)
{
   return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

static int config_path_has_suffix(const char *path, const char *suffix)
{
   size_t path_len, suffix_len;

   if (!path || !suffix)
      return 0;

   path_len = strlen(path);
   suffix_len = strlen(suffix);
   if (path_len < suffix_len)
      return 0;

   return strcmp(path + path_len - suffix_len, suffix) == 0;
}

static int config_has_explicit_database_override(const cJSON *root)
{
   cJSON *item;

   if (!root)
      return 0;

   item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "db2_url");
   if (cJSON_IsString(item) && item->valuestring[0])
      return 1;

   item = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "db2_pool_size");
   if (cJSON_IsNumber(item))
      return 1;

   return 0;
}

/* Strict mode: errors instead of warnings, exit non-zero on validation failure */
int g_config_strict;

static const config_schema_entry_t config_schema[] = {
    /* config_schema.inc: top-level config-key -> type allowlist, #included into the
     * config_schema[] initializer in config.c. Extracted so config.c stays under
     * the 2000-line cap and new keys have a low-churn home (one line each here). */
    {"db1_path", SCHEMA_STRING, 0},
    {"db2_url", SCHEMA_STRING, 0},
    {"db2_pool_size", SCHEMA_INT, 0},
    {"kb_client_url", SCHEMA_STRING, 0},
    {"kb_client_bearer_token", SCHEMA_STRING, 0},
    {"kb_mode", SCHEMA_STRING, 0},
    {"llm_embed_backend", SCHEMA_STRING, 0},
    {"llm_embed_host", SCHEMA_STRING, 0},
    {"llm_embed_gpu", SCHEMA_STRING, 0},
    {"llm_embed_tier", SCHEMA_STRING, 0},
    {"llm_rerank_backend", SCHEMA_STRING, 0},
    {"llm_rerank_host", SCHEMA_STRING, 0},
    {"llm_rerank_gpu", SCHEMA_STRING, 0},
    {"llm_rerank_tier", SCHEMA_STRING, 0},
    {"llm_rerank_endpoint", SCHEMA_STRING, 0},
    {"llm_synth_backend", SCHEMA_STRING, 0},
    {"llm_synth_host", SCHEMA_STRING, 0},
    {"llm_synth_gpu", SCHEMA_STRING, 0},
    {"llm_synth_tier", SCHEMA_STRING, 0},
    {"llm_synth_endpoint", SCHEMA_STRING, 0},
    {"llm_synth_model", SCHEMA_STRING, 0},
    {"guardrail_mode", SCHEMA_STRING, 0},
    {"ingress_preinject_enabled", SCHEMA_BOOL, 0},
    {"ingress_preinject_anthropic_enabled", SCHEMA_BOOL, 0},
    {"ingress_compress_enabled", SCHEMA_BOOL, 0},
    {"ingress_cache_placement_enabled", SCHEMA_BOOL, 0},
    {"ingress_compress_min_chars", SCHEMA_INT, 0},
    {"gateway_prevent_subagents", SCHEMA_BOOL, 0},
    {"gateway_pin_model", SCHEMA_BOOL, 0},
    {"css_style_graph_enabled", SCHEMA_BOOL, 0},
    {"audit_action_enabled", SCHEMA_BOOL, 0},
    {"audit_worm_enabled", SCHEMA_BOOL, 0},
    {"css_render_command", SCHEMA_STRING, 0},
    {"typed_facts_enabled", SCHEMA_BOOL, 0},
    {"kb_pdf_ingest_enabled", SCHEMA_BOOL, 0},
    {"kb_pdf_vector_enabled", SCHEMA_BOOL, 0},
    {"kb_pdf_tsr_enabled", SCHEMA_BOOL, 0},
    {"tsr_command", SCHEMA_STRING, 0},
    {"kb_pdf_assets_enabled", SCHEMA_BOOL, 0},
    {"kb_pdf_blob_dir", SCHEMA_STRING, 0},
    {"kb_pdf_blob_recon_secs", SCHEMA_INT, 0},
    {"kb_pdf_blob_orphan_alarm_mb", SCHEMA_INT, 0},
    {"kb_pdf_ocr_enabled", SCHEMA_BOOL, 0},
    {"ocr_command", SCHEMA_STRING, 0},
    {"ingress_preinject_assembly_budget", SCHEMA_INT, 0},
    {"ingress_max_raw_scans", SCHEMA_INT, 0},
    {"code_span_max_lines", SCHEMA_INT, 0},
    {"require_session_worktree", SCHEMA_BOOL, 0},
    {"require_aimee_memory", SCHEMA_BOOL, 0},
    {"require_aimee_git", SCHEMA_BOOL, 0},
    {"delegate_sandbox", SCHEMA_BOOL, 0},
    {"delegate_sandbox_image", SCHEMA_STRING, 0},
    {"guardrails", SCHEMA_OBJECT, 0},
    {"toolsets", SCHEMA_OBJECT, 0},
    {"script", SCHEMA_OBJECT, 0},
    {"provider", SCHEMA_STRING, 0},
    {"default_persona", SCHEMA_STRING, 0},
    {"use_builtin_cli", SCHEMA_BOOL, 0},
    {"claude_model", SCHEMA_STRING, 0},
    {"codex_model", SCHEMA_STRING, 0},
    {"model_reasoning_effort", SCHEMA_STRING, 0},
    {"openai_endpoint", SCHEMA_STRING, 0},
    {"openai_model", SCHEMA_STRING, 0},
    {"openai_key_cmd", SCHEMA_STRING, 0},
    {"embedding_command", SCHEMA_STRING, 0},
    {"embedding_model", SCHEMA_STRING, 0},
    {"embedding_endpoint", SCHEMA_STRING, 0},
    {"embedding_dim", SCHEMA_INT, 0},
    {"memory_weight_profile", SCHEMA_STRING, 0},
    {"memory_rerank_mode", SCHEMA_STRING, 0},
    {"memory_rerank", SCHEMA_OBJECT, 0},
    {"memory_query_expansion", SCHEMA_OBJECT, 0},
    {"memory_recall_lanes", SCHEMA_OBJECT, 0},
    {"memory_maintenance", SCHEMA_OBJECT, 0},
    {"memory", SCHEMA_OBJECT, 0},
    {"workspaces", SCHEMA_ARRAY, 0},
    {"autonomous", SCHEMA_BOOL, 0},
    {"verify_enabled", SCHEMA_BOOL, 0},
    {"verify_cross_project", SCHEMA_BOOL, 0},
    {"cross_verify", SCHEMA_OBJECT, 0},
    {"retry", SCHEMA_OBJECT, 0},
    {"max_iterations", SCHEMA_INT, 0},
    {"max_iterations_delegate", SCHEMA_INT, 0},
    {"max_delegation_depth", SCHEMA_INT, 0},
    {"max_delegation_spawns", SCHEMA_INT, 0},
    {"max_background_processes", SCHEMA_INT, 0},
    {"background_threads", SCHEMA_INT, 0},
    {"compute_threads", SCHEMA_INT, 0},
    {"session_threads", SCHEMA_INT, 0},
    {"worker_threads", SCHEMA_INT, 0},
    {"concurrency", SCHEMA_OBJECT, 0},
    {"search", SCHEMA_OBJECT, 0},
    {"compact", SCHEMA_OBJECT, 0},
    {"fold", SCHEMA_OBJECT, 0},
    {"reduce", SCHEMA_OBJECT, 0},
    {"economizer", SCHEMA_OBJECT, 0},
    {"sessions", SCHEMA_OBJECT, 0},
    {"sandbox", SCHEMA_OBJECT, 0},
    {"ecomode", SCHEMA_BOOL, 0},
    {"prompt_tier", SCHEMA_STRING, 0},
    {"prompt_file", SCHEMA_STRING, 0},
    {"delegate_prompt_tier", SCHEMA_STRING, 0},
    {"lsp_servers", SCHEMA_ARRAY, 0},
    {"rewind", SCHEMA_OBJECT, 0},
    {"mcp", SCHEMA_OBJECT, 0},
    {"mcp_clients", SCHEMA_ARRAY, 0},
    {"computer_use", SCHEMA_OBJECT, 0},
    {"otel", SCHEMA_OBJECT, 0},
    {"proxy_url", SCHEMA_STRING, 0},
    {"proxy_token", SCHEMA_STRING, 0},
    {"integrity", SCHEMA_OBJECT, 0},
    {"session", SCHEMA_OBJECT, 0},
    {"transport", SCHEMA_OBJECT, 0},
    {"cost_reward", SCHEMA_OBJECT, 0},
    {"reasoning_cap", SCHEMA_OBJECT, 0},
    {"dedup", SCHEMA_OBJECT, 0},
    {"cache_shaping", SCHEMA_OBJECT, 0},
    {"ingress", SCHEMA_OBJECT, 0},
    {"dogfood", SCHEMA_OBJECT, 0},
    {"learning", SCHEMA_OBJECT, 0},
    {"intelligence", SCHEMA_OBJECT, 0},
    {"kb", SCHEMA_OBJECT, 0},
    {"charter", SCHEMA_OBJECT, 0},
    {"identity", SCHEMA_OBJECT, 0},
    {"skills", SCHEMA_OBJECT, 0},
    {"auxiliary", SCHEMA_OBJECT, 0},
    {"model_meta", SCHEMA_OBJECT, 0},
    {"db2", SCHEMA_OBJECT, 0},
    {"ensemble", SCHEMA_OBJECT, 0},
    {"roundtable", SCHEMA_OBJECT, 0},
    {"cron_jobs", SCHEMA_ARRAY, 0},
    {"aimee", SCHEMA_OBJECT, 0},
    {"trigger", SCHEMA_OBJECT, 0},
    {"trigger_rules", SCHEMA_ARRAY, 0},
    {"claude_cli_delegate_enabled", SCHEMA_BOOL, 0},
    {NULL, 0, 0},
};

config_mcp_transport_t config_mcp_transport_from_string(const char *s)
{
   if (!s || !s[0])
      return CONFIG_MCP_TRANSPORT_NONE;
   if (strcmp(s, "stdio") == 0)
      return CONFIG_MCP_TRANSPORT_STDIO;
   if (strcmp(s, "sse") == 0)
      return CONFIG_MCP_TRANSPORT_SSE;
   return CONFIG_MCP_TRANSPORT_NONE;
}

const char *config_mcp_transport_to_string(config_mcp_transport_t transport)
{
   switch (transport)
   {
   case CONFIG_MCP_TRANSPORT_STDIO:
      return "stdio";
   case CONFIG_MCP_TRANSPORT_SSE:
      return "sse";
   default:
      return "";
   }
}

static const char *schema_type_name(schema_type_t t)
{
   switch (t)
   {
   case SCHEMA_STRING:
      return "string";
   case SCHEMA_INT:
      return "integer";
   case SCHEMA_BOOL:
      return "boolean";
   case SCHEMA_ARRAY:
      return "array";
   case SCHEMA_OBJECT:
      return "object";
   }
   return "unknown";
}

static int schema_type_matches(schema_type_t expected, const cJSON *item)
{
   switch (expected)
   {
   case SCHEMA_STRING:
      return cJSON_IsString(item);
   case SCHEMA_INT:
      return cJSON_IsNumber(item);
   case SCHEMA_BOOL:
      return cJSON_IsBool(item);
   case SCHEMA_ARRAY:
      return cJSON_IsArray(item);
   case SCHEMA_OBJECT:
      return cJSON_IsObject(item);
   }
   return 0;
}

static int config_validate(const cJSON *root)
{
   int issues = 0;
   const char *level = g_config_strict ? "error" : "warning";

   /* Check each key in the config against the schema */
   const cJSON *item;
   cJSON_ArrayForEach(item, root)
   {
      const config_schema_entry_t *found = NULL;
      for (const config_schema_entry_t *s = config_schema; s->key; s++)
      {
         if (strcmp(s->key, item->string) == 0)
         {
            found = s;
            break;
         }
      }

      if (!found)
      {
         fprintf(stderr, "aimee: config %s: unknown key \"%s\"\n", level, item->string);
         issues++;
         continue;
      }

      if (!schema_type_matches(found->type, item))
      {
         fprintf(stderr, "aimee: config %s: \"%s\" expected %s, got %s\n", level, item->string,
                 schema_type_name(found->type), jo_type_name(item));
         issues++;
      }
   }

   /* Check required keys */
   for (const config_schema_entry_t *s = config_schema; s->key; s++)
   {
      if (s->required && !cJSON_GetObjectItemCaseSensitive(root, s->key))
      {
         fprintf(stderr, "aimee: config %s: missing required key \"%s\"\n", level, s->key);
         issues++;
      }
   }

   return issues;
}

const char *config_disposition_source_name(config_disposition_source_t source)
{
   switch (source)
   {
   case CONFIG_DISPOSITION_SOURCE_GLOBAL:
      return "global";
   case CONFIG_DISPOSITION_SOURCE_WORKSPACE:
      return "workspace";
   case CONFIG_DISPOSITION_SOURCE_PROJECT:
      return "project";
   default:
      return "unknown";
   }
}

/* Defined in config_charter.c. */
int config_parse_charter(config_t *cfg, const cJSON *root);

/* Defined in config_trigger.c. */
int config_parse_trigger(config_t *cfg, const cJSON *root);

/* Defined in config_kb_maintenance.c. */
int config_parse_kb_maintenance(config_t *cfg, const cJSON *root);
void config_parse_server_api(config_t *cfg, const cJSON *root); /* config_server_api.c */

/* Defined in config_kb_curator.c. */
int config_parse_kb_curator(config_t *cfg, const cJSON *root);
void config_kb_curator_defaults(config_t *cfg);
/* Defined in config_skills.c. */
int config_parse_skills(config_t *cfg, const cJSON *root);
void config_computer_use_defaults(config_t *cfg);
int config_parse_computer_use(config_t *cfg, const cJSON *root);

static void config_set_defaults(config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));

   /* Defaults */
   snprintf(cfg->db1_path, sizeof(cfg->db1_path), "%s", config_default_db1_path());
   snprintf(cfg->guardrail_mode, sizeof(cfg->guardrail_mode), "%s", MODE_APPROVE);
   snprintf(cfg->provider, sizeof(cfg->provider), "claude");
   cfg->compact_enabled = 1; /* default on; set before no-config early returns */
   cfg->coord_closet_enabled =
       1; /* fold §2: default-ON — conserves identifiers elided by the
           * default-on compress/fold so lossy reduction stays recoverable */
   cfg->coord_closet_budget_bytes = 0;
   cfg->coord_closet_max_ratio_pct = 0;
   cfg->fold_enabled = 0; /* fold §1: default-off */
   cfg->fold_retained_msgs = 0;
   cfg->fold_min_fold_msgs = 0;
   cfg->fold_excerpt_bytes = 0;
   cfg->fold_register_enabled = 0; /* fold §6: default-off */
   cfg->fold_freeze_enabled = 0;   /* fold §3: default-off */
   cfg->fold_freeze_tail_cap_msgs = 0;
   cfg->fold_recall_enabled = 0; /* fold §4: default-off */
   cfg->fold_recall_ttl_turns = 0;
   /* Context economizer: DEFAULT-ON on the delegate seam (the implemented, tested
    * reduction path). measure + delegate_seam + history_fold + compress all on.
    * Lossy reduction stays recoverable: both levers only touch messages BEFORE the
    * retained tail (the most recent retained_msgs stay full), and the Coordinate
    * Closet (also default-on) conserves the exact identifiers so the agent can
    * re-issue a tool call to recover an elided body. history_fold is converted to a
    * live fold ONLY at agent_runtime.c (rcfg.history_fold = reduce_history_fold &&
    * !chatgpt) — it reaches the Responses builder on ZERO paths; compress is
    * shape-preserving and runs for all providers. */
   cfg->reduce_measure_enabled = 1;
   cfg->reduce_delegate_seam = 1;
   cfg->reduce_history_fold = 1;
   cfg->reduce_compress = 1;
   /* GATEWAY seam (primary-agent /v1 path) stays off: shadow-only until its
    * request-mutation + 400-retry-from-pristine circuit breaker are built. */
   cfg->reduce_gateway_seam = 0;
   cfg->reduce_freeze_guard_enabled = 1; /* safety: on for the default-on economizer freeze */
   cfg->reduce_freeze_guard_horizon = 1; /* conservative break-even: one reuse pays the write */
   /* Gateway MUTATION (primary-agent reduction) is the whole feature default-OFF; the
    * session-disable TTL is a live-path breaker window (1h) that must stay > 0. */
   cfg->reduce_gateway_mutate = 0;
   cfg->reduce_gateway_session_disable_ttl_ms = 3600000;
   cfg->reduce_gateway_seam_explicit = 0;
   /* Two-tier economizer switches (P3): master ON (measure exempt), aggressive tier OFF. With
    * these defaults every effective lever equals its pre-P3 value (back-compat). */
   cfg->economizer_enabled = 1;
   cfg->economizer_aggressive = 0;
   /* command-aware tool-output condensation: DEFAULT-ON (P1c). Safe-tier lever — it passes
    * the deterministic gate: lossless-on-demand (full output spilled), fail-open (any
    * miss/decline -> raw), and a no-over-reduction audit (failures/diagnostics + their
    * detail block are kept in the condensed view, not just the spill). It replaces the old
    * lossy 32 KB read-cap truncation with lossless-recoverable condensation. */
   cfg->reduce_command_filter = 1;
   /* Autonomous-dev knobs — defaults match the historical AIMEE_AUTONOMY_* env defaults
    * (adversarial + fan-out tiers OFF; retry/unit caps at their wfe defaults). */
   cfg->autonomy_skeptics = 0;
   cfg->autonomy_fanout = 0;
   cfg->autonomy_unit_retry = 2;
   cfg->autonomy_unit_max = 16;
   cfg->autonomy_ci_retry_max = 2;
   snprintf(cfg->memory_citations_mode, sizeof(cfg->memory_citations_mode), "%s", "off");
   snprintf(cfg->memory_coref_mode, sizeof(cfg->memory_coref_mode), "%s", "off");
   cfg->memory_cognify_async_enabled = 0;
   cfg->memory_scenes_enabled = 0;
   cfg->memory_scenes_min_cluster_size = 3;
   cfg->memory_scenes_top_m = 3;
   cfg->memory_scenes_global_escape_ratio = 0.2;
   cfg->memory_bm25_weight = 0.0;
   cfg->memory_semantic_weight = 0.0;
   cfg->memory_semantic_floor_scale = 0.0; /* 0 = auto-scale by embedding dim */
   cfg->memory_fetch_budget_enabled = 0;
   cfg->memory_fetch_budget_base = 128;
   cfg->memory_fetch_budget_shape_aware = 1;
   cfg->kb_search_max_results = 50;
   /* structured-pdf Phase C blob reconciliation: default hourly sweep, alarm at 1 GiB of
    * reclaimable orphan bytes (config_t is memset-0 above, so these explicit values are the
    * defaults). The sweep is still a no-op until kb_pdf_assets_enabled is on. */
   cfg->kb_pdf_blob_recon_secs = 3600;
   cfg->kb_pdf_blob_orphan_alarm_mb = 1024;
   /* Embedding dimension. 0 = UNSET (the operator did not pin a dim): readers fall
    * back to 1024 (db2_embedding_dim(), kb_main, kb_ingest_workers), and — crucially
    * — config_embedding_dim_is_pinned() reports NOT-pinned, so §2a's recorded-dim
    * preference and §2b's fresh-DB embedder /health probe can derive the real dim.
    * A non-zero value here means the operator explicitly pinned it (yaml/env), which
    * is authoritative and refuses a mismatch. (Was defaulted to 1024, which made
    * every deployment look "pinned" and silently disabled §2a/§2b.) */
   cfg->embedding_dim = 0;
   /* The cross-encoder rerank stage (Ettin reranker sized to the embedder tier:
    * 1b with the 4b embedder, 400m with the 0.6b; served by the
    * embedder service /rerank, client scripts/rerank-remote.py) is the third
    * pipeline stage after the embedder, and is default-ON: every retrieval
    * runs a top-K cross-encoder pass over the dense/lexical candidates. It
    * degrades safely to plain hybrid ordering if the reranker service is absent
    * or errors, so default-on is safe even without the embedder container. */
   cfg->memory_rerank_enabled = 1;
   snprintf(cfg->memory_rerank_command, sizeof(cfg->memory_rerank_command), "%s",
            "python3 /opt/aimee/scripts/rerank-remote.py");
   cfg->memory_routing_enabled = 1;
   /* Typed-fact extraction runs fully OFFLINE (the memory_facts drain), so it costs
    * nothing per turn and defaults ON on every backend -- including the CPU-only
    * Gemma E4B/E2B fallback. HyDE query rewrite is still per-turn LLM work, so it
    * defaults OFF here and config_apply_inference_backend_defaults() flips it ON only
    * for an accelerated backend. An explicit config value always wins. HyDE mode
    * defaults on so the rewrite, once enabled, generates a hypothetical answer. */
   cfg->typed_facts_enabled = 1;
   cfg->memory_rewrite_enabled = 0;
   cfg->memory_rewrite_hyde = 1;
   /* Replayable-evidence roundtable verification (Part A): default-on. config_t
    * is memset-0 above, so this explicit assignment is what makes the contract
    * hold (the config_fields[] row carries is_bool, not a default value). */
   cfg->roundtable_replay_verify_enabled = 1;
   /* Default-on to preserve behavior: profile-card refresh ran ungated in the
    * maintenance REPLAY pass before the enable-gate was wired. Maintenance is
    * itself default-off, so this is a no-op until maintenance is enabled. */
   cfg->memory_profile_cards_enabled = 1;
   /* Default-on to preserve behavior: dedupe of duplicate-key memories ran ungated
    * in the maintenance COMPACT pass before the enable-gate was wired. */
   cfg->memory_improve_dedupe_enabled = 1;
   /* Default-on to preserve behavior: the auto-create of a retrieval_failure
    * directive after a confident-failure ran ungated in memory_assemble before
    * this toggle was wired. Off stops auto-creation; manually-created directives
    * still surface. */
   cfg->memory_directives_enabled = 1;
   cfg->memory_hard_negative_log[0] = '\0';
   cfg->dogfood_enabled = 1;
   cfg->dogfood_log_dir[0] = '\0';
   cfg->dogfood_commit_raw = 0;
   cfg->dogfood_inline_tagging = 0;
   cfg->dogfood_autolabel_repair = 0;
   cfg->dogfood_autolabel_continuation = 0;
   cfg->dogfood_autolabel_repeat_question = 0;
   cfg->learning_router_enabled = 1;
   cfg->learning_proposal_ttl_days = 7;
   cfg->learning_max_commits_per_week = 25;
   config_learning_defaults(cfg); /* learning.synthesize.* + learning.embed.* */
   /* Default-on: the citation_then_{repair,continuation} detector is graded PASS
    * (precision/recall 1.0 on the labelled corpus via make learning-citation-eval)
    * and is now wired into the primary chat turn (openai_chat.c). It is
    * self-gating (fires only after a memory-citation moment) and emits operator-
    * reviewed learning proposals, so the blast radius is bounded. The 3 stateful
    * implicit heuristics below stay off (their detectors need session/DB state and
    * are not yet validated). NB: learning_implicit_* lack config-file persistence
    * (a pre-existing systemic gap) — these defaults apply at startup. */
   cfg->learning_implicit_citation_repair = 1;
   cfg->learning_implicit_citation_continuation = 1;
   cfg->learning_implicit_repeat_question = 0;
   cfg->learning_implicit_repeated_correction = 0;
   cfg->learning_implicit_workflow_repetition = 0;
   cfg->integrity_enabled = 0;
   cfg->integrity_dry_run = 1;
   /* Ingress envelope DEFAULT-ON (operator decision 2026-06-28): inject the
    * <aimee-context> memory/code preview on primary ingress turns, fold code hits
    * to recoverable file:line refs (recover via code_span_get), and place the
    * envelope after the stable prefix for cache survival. TURN OFF (per-request
    * `X-Aimee-Compress: 0`, or set these false) for agentic ingress where recovery
    * round-trips can erase the saving. Rationale, metrics + the honest-benchmark
    * framing: proposal §8.0 (docs/proposals/done/ingress-compression-and-cache-
    * alignment.md). The compress<-preinject dependency is enforced by control flow
    * (ingress_preinject_build returns early when neither preinject nor typed-facts
    * is on, before the compress flag is read — so compress alone is a safe no-op).
    * Anthropic injection + failure-mining stay opt-in (separate gates). */
   cfg->ingress_preinject_enabled = 1;
   cfg->ingress_compress_enabled = 1;
   cfg->ingress_cache_placement_enabled = 1;
   cfg->ingress_preinject_assembly_budget = 6144;
   cfg->ingress_max_raw_scans = 0;
   cfg->code_span_max_lines = 400;
   /* 0 = use the built-in default AGENT_TOOL_OUTPUT_MAX (32768) for the
    * per-result model-visible tool-output cap (see agent_tool_output_cap()). */
   cfg->tool_output_max_bytes = 0;
   cfg->ingress_compress_min_chars = 80;
   /* Default ON: each mutating session must run in its own isolated worktree+branch
    * (.aimee/worktrees/...), never the shared primary checkout. Concurrent aimee
    * sessions sharing one checkout collide on a single git HEAD. Explicit
    * `require_session_worktree: false` bypasses (see cli_attention_guard.c). */
   cfg->require_session_worktree = 1;
   /* Default ON: agent-authored durable memories go through aimee's memory
    * system, not external per-harness memory files. Explicit
    * `require_aimee_memory: false` bypasses (see cli_attention_guard.c). */
   cfg->require_aimee_memory = 1;
   /* Default ON: a delegate never runs `git`/`gh` in a shell — git and forge
    * actions go through aimee's git_* tools and execute on aimee-server, where
    * the forge credential stays in-process. Explicit `require_aimee_git: false`
    * bypasses (see wfe_native_gate.c). */
   cfg->require_aimee_git = 1;
   /* Default-on as of the virtual-context rollout: the long-session benchmark
    * gate (make virtual-context-eval-check) passes on synthetic and real
    * tool-heavy session fixtures. Rollback: set session.virtual_context.enabled
    * = false in aimee.yaml; raw turns remain the source of truth (no data loss). */
   cfg->virtual_context_enabled = 1;
   cfg->virtual_context_assembly_budget = 4096;
   cfg->cache_aware_rewrite_enabled = 0;
   cfg->cache_aware_rewrite_min_savings_tokens = 500;
   cfg->cache_aware_rewrite_hard_context_threshold = 0.85;
   cfg->cache_aware_rewrite_max_defer_turns = 20;
   cfg->cache_aware_rewrite_segment_check_turns = 5;
   cfg->cost_reward_enabled = 0;
   cfg->cost_reward_lambda_pct = 30;
   cfg->cost_reward_ref_usd_milli = 500;
   cfg->reasoning_cap_enabled = 0;
   cfg->dedup_enabled = 0;
   cfg->cache_shaping_enabled = 0;
   cfg->ingress_usage_accounting_enabled = 0;
   cfg->ingress_audit_async = 0;
   cfg->ingress_trusted_proxy_secret[0] = '\0';
   cfg->dedup_window_seconds = 5;
   cfg->cache_min_chars = 0;
   cfg->guardrails_semantic_enabled = 0;
   cfg->guardrails_semantic_dry_run = 1;
   cfg->guardrails_semantic_advisory_only = 1;
   cfg->guardrails_semantic_command[0] = '\0';
   cfg->guardrails_semantic_warn_threshold = 0.40;
   cfg->guardrails_semantic_prompt_threshold = 0.70;
   cfg->guardrails_semantic_block_threshold = 0.90;
   cfg->guardrails_semantic_allow_ml_only_block = 0;
   cfg->guardrails_blast_radius_advisory_enabled = 0;
   cfg->kb_api_http_port = 0;
   cfg->kb_api_bearer_token[0] = '\0';
   cfg->kb_worker_count = CONFIG_DEFAULT_KB_WORKER_THREADS;
   cfg->kb_connection_workers = 2;
   cfg->code_hybrid_weight_code = 1.0;
   cfg->code_hybrid_weight_graph = 1.0;
   cfg->code_hybrid_weight_vector = 1.0;
   cfg->code_hybrid_weight_memory = 1.0;
   cfg->code_hybrid_rrf_k = 60.0;              /* KB_RRF_DEFAULT_K */
   cfg->code_surprising_precision_floor = 0.0; /* §4 self-suppress off by default */
   cfg->kb_bg_ingest_enabled = 1;
   cfg->kb_bg_ingest_interval_hours = 6;
#ifdef __linux__
   cfg->kb_bg_watch_enabled = 1;
#else
   cfg->kb_bg_watch_enabled = 0;
#endif
   cfg->kb_bg_watch_debounce_secs = 30;
   cfg->kb_reembed_on_dim_change = 0; /* §2c: refuse-and-instruct by default */
   cfg->kb_purge_fence_ttl_s = 900;   /* project-purge fence staleness bound */
   cfg->kb_maintenance_enabled = 0;
   cfg->kb_maintenance_interval_hours = 24;
   cfg->kb_maintenance_lambda = 0.005;
   cfg->kb_maintenance_floor = 0.10;
   cfg->kb_maintenance_min_age_days = 7;
   cfg->kb_maintenance_orphan_days = 90;
   cfg->kb_mining_enabled = 1;
   cfg->kb_mining_min_poll_s = 300;
   cfg->kb_mining_failure_learning_enabled = 0;
   cfg->review_scheduler_enabled = 1; /* default on: idle reflection over session_summary
                                       * artifacts. Cheap when idle; the LLM synthesis pass only
                                       * runs where a Tier-B provider/command is configured. */
   cfg->review_idle_trigger_minutes = 30;
   cfg->review_session_cooldown_hours = 24;
   cfg->concurrency_preempt_single_slot_only = 1;
   cfg->concurrency_preempt_requeue_max = CONFIG_DEFAULT_CONCURRENCY_PREEMPT_REQUEUE_MAX;
   cfg->review_batch_cap = 10;
   config_kb_curator_defaults(cfg); /* kb.curator.* + kb.evidence.embed.* */
   cfg->skills_review_enabled = 0;
   cfg->skills_review_nudge_interval = 10;
   cfg->skills_curator_enabled = 0;
   cfg->skills_curator_interval_hours = 168;
   cfg->skills_stale_after_days = 30;
   cfg->skills_archive_after_days = 90;
   cfg->skills_dispatch_enabled = 1;
   cfg->skills_dispatch_max_index = 24;
   cfg->skills_dispatch_advisory = 0;
   cfg->skills_capability_autostub = 0;
   cfg->skills_eval_gate_enabled = 0;
   cfg->skills_eval_threshold = 0.01;
   cfg->css_style_graph_enabled = 1; /* default-on: the indexer builds the CSS style
                                        graph so the read-only css signals/report work
                                        out of the box (set false to opt out) */
   cfg->wfe_live_forge_enabled = 1;  /* default-ON (operator ruling 2026-07-13,
                                        restoring the plan's default): the live forge
                                        registers at standup; set false to opt out.
                                        The merge-target rail still bounds every op --
                                        PRs open-only against the resolved trunk,
                                        merges only to the unprotected autonomous
                                        base -- and each op re-checks flag + rail. */
   cfg->audit_action_enabled = 1;    /* default-ON: the trajectory_export reader (S3)
                                        shipped, so the passive per-action audit row
                                        is on by default; set false to opt out */
   snprintf(cfg->css_render_command, sizeof(cfg->css_render_command), "%s",
            CONFIG_DEFAULT_CSS_RENDER_COMMAND); /* default-on render backend (inert
                                                   until the sidecar is up); set empty
                                                   to disable */
   cfg->worktree_gc_enabled = 1;
   cfg->worktree_gc_max_age_days = 14;
   cfg->model_meta_refresh_minutes = 60;
   cfg->model_meta_capability_routing = 0;
   snprintf(cfg->db2_vector_corpus_index, sizeof(cfg->db2_vector_corpus_index), "auto");
   cfg->db2_vector_corpus_diskann_threshold = 1000000;
   cfg->ensemble_min_successful = 2;
   cfg->ensemble_max_cost_usd = 0.0; /* 0 = no cost cap (unlimited) by default */
   snprintf(cfg->default_persona, sizeof(cfg->default_persona), "engineer");
   cfg->roundtable_max_rounds = 1;
   cfg->roundtable_converge_threshold = 10;
   /* Saner default: 6 min (was 10). Long enough for a multi-round reasoning-model
    * ensemble, short enough that a wedged run fails fast instead of a 10-min
    * silent block. Overridable via roundtable.deadline_ms. Paired with the
    * round-boundary progress logging so an in-flight run is observably advancing. */
   cfg->roundtable_deadline_ms = 360000;
   snprintf(cfg->roundtable_turns, sizeof(cfg->roundtable_turns), "parallel");
   snprintf(cfg->roundtable_pipeline_done_bar, sizeof(cfg->roundtable_pipeline_done_bar),
            "zero_blocking");
   cfg->roundtable_pipeline_max_passes = 0;            /* unbounded: correctness over budget */
   cfg->roundtable_pipeline_max_attempts_per_pass = 2; /* infra-retry ceiling */
   cfg->roundtable_pipeline_max_cost_usd = 0.0;
   cfg->roundtable_pipeline_max_total_cost_usd = 0.0;
   cfg->roundtable_pipeline_gate_ttl_h = 0;
   cfg->roundtable_pipeline_parked_releases_slot = 1;
   cfg->roundtable_pipeline_unknown_context_tokens = 8000;
   cfg->mcp_osv_enabled = 1;
   cfg->mcp_osv_offline = 0;
   cfg->mcp_osv_enforce = 1;
   cfg->mcp_osv_cache_ttl_hours = 24;
   snprintf(cfg->mcp_osv_endpoint, sizeof(cfg->mcp_osv_endpoint), "https://api.osv.dev/v1/query");
   cfg->mcp_osv_allow_count = 0;
   config_computer_use_defaults(cfg);
   cfg->trigger_max_concurrent = 2;
   /* Master switch for DB1-local per-user interaction learning: observe the
    * user's own turns into the working profile (memory_recall_handler) AND inject
    * the learned profile into the session context (build_session_context) so the
    * primary adapts to how they work. Default on; empty until something is
    * learned, so it is a no-op for a fresh user. An empty field allow-list means
    * all learned fields inject. */
   cfg->identity_working_profile_injection_enabled = 1;
   cfg->identity_working_profile_injection_fields_count = 0;
   cfg->memory_recall_lanes_floor_summary = 4;
   cfg->memory_recall_lanes_floor_fact = 4;
   snprintf(cfg->openai_endpoint, sizeof(cfg->openai_endpoint), "https://api.openai.com/v1");
   snprintf(cfg->openai_model, sizeof(cfg->openai_model), "gpt-4o");
   cfg->openai_key_cmd[0] = '\0';
   cfg->workspace_count = 0;
   config_parse_database(cfg, NULL);
}

/* Default the LLM-backed memory features (typed-fact extract/inject + HyDE query
 * rewrite) ON or OFF based on the active inference backend. The backend is the
 * curator provider model (the external model or the llama-llm container the
 * rewrite/extraction call). The CPU-only fallback runs Gemma E4B/E2B, which is
 * too slow for per-turn LLM work, so on it (or with no model configured) the
 * features default OFF; any other model — an external model, or a larger local
 * model on an accelerated llama-llm — defaults them ON. An explicit value in the
 * config is never overridden. */
static int model_is_cpu_only(const char *model)
{
   if (!model || !model[0])
      return 1; /* no inference backend -> can't run the LLM features */
   return strstr(model, "E4B") || strstr(model, "e4b") || strstr(model, "E2B") ||
          strstr(model, "e2b");
}

static void config_apply_inference_backend_defaults(config_t *cfg, const cJSON *root)
{
   int accel = !model_is_cpu_only(cfg->kb_curator_provider_model);
   /* typed_facts_enabled now defaults ON unconditionally (offline extraction, see
    * config_reset); the backend gate below governs only the per-turn HyDE rewrite.
    * An explicit kb.typed_facts.enabled / typed_facts_enabled still wins via parse. */
   if (!cJSON_GetObjectItemCaseSensitive((cJSON *)root, "memory_rewrite") &&
       !cJSON_GetObjectItemCaseSensitive((cJSON *)root, "memory_rewrite_enabled"))
      cfg->memory_rewrite_enabled = accel;
}

int econ_reduction_master_on(const config_t *cfg)
{
   return cfg && cfg->economizer_enabled ? 1 : 0;
}

int econ_gateway_mutate_on(const config_t *cfg)
{
   /* the live-primary mutator needs the master ON, the aggressive tier opted IN, AND the
    * lever itself set — the aggressive flag alone never activates a live-traffic mutator. */
   return cfg && cfg->economizer_enabled && cfg->economizer_aggressive && cfg->reduce_gateway_mutate
              ? 1
              : 0;
}

static int config_snapshot_live(void);

/* Public config read. In the SERVER (once config_snapshot_init has seeded the live snapshot)
 * this returns the current snapshot — a lock-free POD copy that reflects the last reload
 * IMMEDIATELY (push-driven), with no file I/O or mtime-cache-miss wait. Everywhere else (CLI
 * one-shots, and before startup seeds it) it reads the file. config_reload uses the from-file
 * path directly so a reload always re-reads disk, never the snapshot it is about to replace. */
int config_load(config_t *cfg)
{
   if (config_snapshot_live())
      return config_snapshot_get(cfg);
   return config_load_file(cfg);
}

int config_load_file(config_t *cfg)
{
   config_set_defaults(cfg);

   const char *path = config_default_path();

   /* Return cached config only if the file looks identical on every cheap axis
    * stat() gives us: same mtime, size and inode. mtime alone is spoofable by a
    * same-timestamp (or clock-skewed) rewrite — observed on the tiered appliance
    * filesystem, where an in-place `aimee workspace add` rewrite kept serving the
    * stale (empty-workspaces) snapshot, which config_save then re-serialised. */
   if (!getenv("AIMEE_NO_CACHE") && g_config_cached)
   {
      struct stat st;
      if (stat(path, &st) == 0)
      {
         struct timespec mt = AIMEE_STAT_MTIM(st);
         if (strcmp(g_config_cache_path, path) == 0 && timespec_eq(&mt, &g_config_mtime) &&
             st.st_size == g_config_size && st.st_ino == g_config_ino)
         {
            memcpy(cfg, &g_config_cache, sizeof(*cfg));
            return 0;
         }
      }
   }

   FILE *fp = fopen(path, "r");
   if (!fp)
      return 0; /* defaults are fine */

   fseek(fp, 0, SEEK_END);
   long len = ftell(fp);
   fseek(fp, 0, SEEK_SET);

   if (len <= 0 || len > MAX_FILE_SIZE)
   {
      fclose(fp);
      return 0;
   }

   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(fp);
      return -1;
   }

   size_t nread = fread(buf, 1, (size_t)len, fp);
   fclose(fp);
   buf[nread] = '\0';

   cJSON *root = yaml_parse(buf);
   free(buf);
   if (!root)
      return 0;

   /* Validate config against schema */
   int issues = config_validate(root);
   issues += config_parse_dispositions(cfg, root);
   issues += config_parse_charter(cfg, root);
   issues += config_parse_trigger(cfg, root);
   issues += config_parse_kb_maintenance(cfg, root);
   issues += config_parse_kb_curator(cfg, root);
   issues += config_parse_skills(cfg, root);
   cJSON *toolsets_node = cJSON_GetObjectItemCaseSensitive(root, "toolsets");
   cJSON *script_node = cJSON_GetObjectItemCaseSensitive(root, "script");
   cJSON *script_allowed_node = cJSON_IsObject(script_node)
                                    ? cJSON_GetObjectItemCaseSensitive(script_node, "allowed_tools")
                                    : NULL;
   int validate_toolsets = toolsets_node || script_allowed_node;
#if defined(__GNUC__)
   if (validate_toolsets && toolset_registry_init && toolset_registry_load_file)
   {
      toolset_registry_t registry;
      char toolset_err[TOOLSET_ERROR_MAX] = "";
      toolset_registry_init(&registry);
      if (toolset_registry_load_file(&registry, path, toolset_err, sizeof(toolset_err)) != 0)
      {
         fprintf(stderr, "aimee: config validation: %s\n",
                 toolset_err[0] ? toolset_err : "invalid toolset configuration");
         issues++;
      }
   }
#else
   if (validate_toolsets)
   {
      toolset_registry_t registry;
      char toolset_err[TOOLSET_ERROR_MAX] = "";
      toolset_registry_init(&registry);
      if (toolset_registry_load_file(&registry, path, toolset_err, sizeof(toolset_err)) != 0)
      {
         fprintf(stderr, "aimee: config validation: %s\n",
                 toolset_err[0] ? toolset_err : "invalid toolset configuration");
         issues++;
      }
   }
#endif
   issues += config_parse_memory_cognify(cfg, root);
   issues += config_parse_memory_citations(cfg, root);
   issues += config_parse_memory_briefing(cfg, root);
   issues += config_parse_memory_aggregation(cfg, root);
   issues += config_parse_memory_prospective(cfg, root);
   issues += config_parse_memory_lifecycle(cfg, root);
   issues += config_parse_memory_recall(cfg, root);
   issues += config_parse_memory_directives(cfg, root);
   if (issues > 0 && g_config_strict)
   {
      fprintf(stderr, "aimee: strict mode: %d config validation error(s), aborting\n", issues);
      cJSON_Delete(root);
      return -1;
   }

   cJSON *item;

   item = cJSON_GetObjectItemCaseSensitive(root, "db1_path");
   if (cJSON_IsString(item) && item->valuestring[0])
   {
      const char *default_db_path = config_default_db1_path();

      if (strcmp(item->valuestring, default_db_path) == 0)
         snprintf(cfg->db1_path, sizeof(cfg->db1_path), "%s", item->valuestring);
      else if (!config_has_explicit_database_override(root) &&
               config_path_has_suffix(item->valuestring, "/.config/aimee/aimee.db"))
      {
         /* The derived default DB1 path is HOME-specific; keep using the
          * current HOME-derived default instead of pinning a copied config. */
      }
      else
         snprintf(cfg->db1_path, sizeof(cfg->db1_path), "%s", item->valuestring);
   }

   config_parse_database(cfg, root);

   item = cJSON_GetObjectItemCaseSensitive(root, "guardrail_mode");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->guardrail_mode, sizeof(cfg->guardrail_mode), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "provider");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->provider, sizeof(cfg->provider), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "default_persona");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->default_persona, sizeof(cfg->default_persona), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "openai_endpoint");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->openai_endpoint, sizeof(cfg->openai_endpoint), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "openai_model");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->openai_model, sizeof(cfg->openai_model), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "openai_key_cmd");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->openai_key_cmd, sizeof(cfg->openai_key_cmd), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "claude_model");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->claude_model, sizeof(cfg->claude_model), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "codex_model");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->codex_model, sizeof(cfg->codex_model), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "model_reasoning_effort");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->model_reasoning_effort, sizeof(cfg->model_reasoning_effort), "%s",
               item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "autonomous");
   if (cJSON_IsBool(item))
      cfg->autonomous = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "verify_enabled");
   if (cJSON_IsBool(item))
      cfg->verify_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "claude_cli_delegate_enabled");
   if (cJSON_IsBool(item))
      cfg->claude_cli_delegate_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "delegate_graph_context_enabled");
   if (cJSON_IsBool(item))
      cfg->delegate_graph_context_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "verify_cross_project");
   if (cJSON_IsBool(item))
      cfg->verify_cross_project = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "ingress_preinject_enabled");
   if (cJSON_IsBool(item))
      cfg->ingress_preinject_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "ingress_preinject_anthropic_enabled");
   if (cJSON_IsBool(item))
      cfg->ingress_preinject_anthropic_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "ingress_compress_enabled");
   if (cJSON_IsBool(item))
      cfg->ingress_compress_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "ingress_cache_placement_enabled");
   if (cJSON_IsBool(item))
      cfg->ingress_cache_placement_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "ingress_compress_min_chars");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->ingress_compress_min_chars = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "gateway_prevent_subagents");
   if (cJSON_IsBool(item))
      cfg->gateway_prevent_subagents = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "gateway_pin_model");
   if (cJSON_IsBool(item))
      cfg->gateway_pin_model = cJSON_IsTrue(item);

   /* CSS migration assistant style-graph write path (WP-C). The field +
    * descriptor + save existed, but the YAML load parse was missing, so the
    * flag never took effect during indexing. */
   item = cJSON_GetObjectItemCaseSensitive(root, "css_style_graph_enabled");
   if (cJSON_IsBool(item))
      cfg->css_style_graph_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "wfe_live_forge_enabled");
   if (cJSON_IsBool(item))
      cfg->wfe_live_forge_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "audit_action_enabled");
   if (cJSON_IsBool(item))
      cfg->audit_action_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "audit_worm_enabled");
   if (cJSON_IsBool(item))
      cfg->audit_worm_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "css_render_command");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->css_render_command, sizeof(cfg->css_render_command), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "ingress_preinject_assembly_budget");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->ingress_preinject_assembly_budget = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "ingress_max_raw_scans");
   if (cJSON_IsNumber(item) && item->valuedouble >= 0)
      cfg->ingress_max_raw_scans = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "code_span_max_lines");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->code_span_max_lines = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "tool_output_max_bytes");
   if (cJSON_IsNumber(item) && item->valuedouble >= 0)
      cfg->tool_output_max_bytes = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "require_session_worktree");
   if (cJSON_IsBool(item))
      cfg->require_session_worktree = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "require_aimee_memory");
   if (cJSON_IsBool(item))
      cfg->require_aimee_memory = cJSON_IsTrue(item);

   /* require_aimee_git had a config_fields[] row, a schema row, a default, a
    * config_save writer and NO parse — so `require_aimee_git: false` in aimee.yaml
    * never loaded, and `aimee config set require_aimee_git false` persisted a value
    * that silently reverted to ON at the next restart. The operator escape hatch
    * cmd_hooks.c offers ("Operator: require_aimee_git: false ...") could not work.
    * Exactly the failure the comment below this block already warns about. */
   item = cJSON_GetObjectItemCaseSensitive(root, "require_aimee_git");
   if (cJSON_IsBool(item))
      cfg->require_aimee_git = cJSON_IsTrue(item);

   /* Delegate sandbox: default 0 (off) from the zeroed config_t, so only an
    * explicit `delegate_sandbox: true` turns it on. */
   item = cJSON_GetObjectItemCaseSensitive(root, "delegate_sandbox");
   if (cJSON_IsBool(item))
      cfg->delegate_sandbox = cJSON_IsTrue(item);
   item = cJSON_GetObjectItemCaseSensitive(root, "delegate_sandbox_image");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->delegate_sandbox_image, sizeof(cfg->delegate_sandbox_image), "%s",
               item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "typed_facts_enabled");
   if (cJSON_IsBool(item))
      cfg->typed_facts_enabled = cJSON_IsTrue(item);

   /* structured-PDF gates. These have config_fields[] rows (CLI/server-settable) but
    * historically lacked a file parse, so a value set in aimee.yaml never loaded back on a
    * fresh process. Parse them here as top-level bools so both the Phase-1/2 ingest gate
    * and the Phase-A vector gate are durably configurable. */
   item = cJSON_GetObjectItemCaseSensitive(root, "kb_pdf_ingest_enabled");
   if (cJSON_IsBool(item))
      cfg->kb_pdf_ingest_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "kb_pdf_vector_enabled");
   if (cJSON_IsBool(item))
      cfg->kb_pdf_vector_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "kb_pdf_tsr_enabled");
   if (cJSON_IsBool(item))
      cfg->kb_pdf_tsr_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "tsr_command");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->tsr_command, sizeof(cfg->tsr_command), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "kb_pdf_assets_enabled");
   if (cJSON_IsBool(item))
      cfg->kb_pdf_assets_enabled = cJSON_IsTrue(item);
   item = cJSON_GetObjectItemCaseSensitive(root, "kb_pdf_blob_dir");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->kb_pdf_blob_dir, sizeof(cfg->kb_pdf_blob_dir), "%s", item->valuestring);
   item = cJSON_GetObjectItemCaseSensitive(root, "kb_pdf_blob_recon_secs");
   if (cJSON_IsNumber(item))
      cfg->kb_pdf_blob_recon_secs = (int)item->valuedouble;
   item = cJSON_GetObjectItemCaseSensitive(root, "kb_pdf_blob_orphan_alarm_mb");
   if (cJSON_IsNumber(item))
      cfg->kb_pdf_blob_orphan_alarm_mb = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "kb_pdf_ocr_enabled");
   if (cJSON_IsBool(item))
      cfg->kb_pdf_ocr_enabled = cJSON_IsTrue(item);
   item = cJSON_GetObjectItemCaseSensitive(root, "ocr_command");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->ocr_command, sizeof(cfg->ocr_command), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "kb_evidence_emit_enabled");
   if (cJSON_IsBool(item))
      cfg->kb_evidence_emit_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "fidelity_check_enabled");
   if (cJSON_IsBool(item))
      cfg->fidelity_check_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "embedding_command");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->embedding_command, sizeof(cfg->embedding_command), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "embedding_model");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->embedding_model, sizeof(cfg->embedding_model), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "embedding_endpoint");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->embedding_endpoint, sizeof(cfg->embedding_endpoint), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "embedding_dim");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->embedding_dim = (int)item->valuedouble;

   /* Setup-wizard page-2 backend record (kb_mode + per-role llm_* fields). All are
    * string fields; parse them from a compact table that mirrors config_fields.c.
    * (kb_client_url/bearer are parsed with the DB/KB block in config_database.c.) */
   {
      static const struct
      {
         const char *key;
         size_t off, sz;
      } page2[] = {
          {"kb_mode", offsetof(config_t, kb_mode), sizeof(((config_t *)0)->kb_mode)},
          {"llm_embed_backend", offsetof(config_t, llm_embed_backend),
           sizeof(((config_t *)0)->llm_embed_backend)},
          {"llm_embed_host", offsetof(config_t, llm_embed_host),
           sizeof(((config_t *)0)->llm_embed_host)},
          {"llm_embed_gpu", offsetof(config_t, llm_embed_gpu),
           sizeof(((config_t *)0)->llm_embed_gpu)},
          {"llm_embed_tier", offsetof(config_t, llm_embed_tier),
           sizeof(((config_t *)0)->llm_embed_tier)},
          {"llm_rerank_backend", offsetof(config_t, llm_rerank_backend),
           sizeof(((config_t *)0)->llm_rerank_backend)},
          {"llm_rerank_host", offsetof(config_t, llm_rerank_host),
           sizeof(((config_t *)0)->llm_rerank_host)},
          {"llm_rerank_gpu", offsetof(config_t, llm_rerank_gpu),
           sizeof(((config_t *)0)->llm_rerank_gpu)},
          {"llm_rerank_tier", offsetof(config_t, llm_rerank_tier),
           sizeof(((config_t *)0)->llm_rerank_tier)},
          {"llm_rerank_endpoint", offsetof(config_t, llm_rerank_endpoint),
           sizeof(((config_t *)0)->llm_rerank_endpoint)},
          {"llm_synth_backend", offsetof(config_t, llm_synth_backend),
           sizeof(((config_t *)0)->llm_synth_backend)},
          {"llm_synth_host", offsetof(config_t, llm_synth_host),
           sizeof(((config_t *)0)->llm_synth_host)},
          {"llm_synth_gpu", offsetof(config_t, llm_synth_gpu),
           sizeof(((config_t *)0)->llm_synth_gpu)},
          {"llm_synth_tier", offsetof(config_t, llm_synth_tier),
           sizeof(((config_t *)0)->llm_synth_tier)},
          {"llm_synth_endpoint", offsetof(config_t, llm_synth_endpoint),
           sizeof(((config_t *)0)->llm_synth_endpoint)},
          {"llm_synth_model", offsetof(config_t, llm_synth_model),
           sizeof(((config_t *)0)->llm_synth_model)},
      };
      for (size_t i = 0; i < sizeof(page2) / sizeof(page2[0]); i++)
      {
         cJSON *pit = cJSON_GetObjectItemCaseSensitive(root, page2[i].key);
         if (cJSON_IsString(pit) && pit->valuestring[0])
            snprintf((char *)cfg + page2[i].off, page2[i].sz, "%s", pit->valuestring);
      }
   }

   item = cJSON_GetObjectItemCaseSensitive(root, "memory_weight_profile");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->memory_weight_profile, sizeof(cfg->memory_weight_profile), "%s",
               item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "memory_rerank_mode");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->memory_rerank_mode, sizeof(cfg->memory_rerank_mode), "%s", item->valuestring);

   config_parse_memory_rewrite_section(cfg, root);

   config_apply_inference_backend_defaults(cfg, root);

   config_parse_memory_negation_section(cfg, root);

   config_parse_memory_rerank_section(cfg, root);

   config_parse_memory_query_expansion_section(cfg, root);

   config_parse_memory_recall_lanes_section(cfg, root);

   config_parse_memory_window_section(cfg, root);

   config_parse_kb_section(cfg, root);

   config_parse_memory_maintenance_section(cfg, root);

   config_parse_worktree_gc_section(cfg, root);
   config_parse_fold_section(cfg, root);
   config_parse_reduce_section(cfg, root);
   config_apply_reduce_consistency(cfg); /* mutate=1 -> auto-enable shadow seam in memory + WARN */
   config_parse_autonomy_section(cfg, root);

   config_parse_memory_section(cfg, root);
   config_apply_learning_settings(cfg, root);
   config_apply_calibration_settings(cfg, root);
   config_apply_demotion_settings(cfg, root);
   config_apply_ranking_settings(cfg, root);
   config_apply_reasoning_settings(cfg, root);
   config_apply_bandit_settings(cfg, root);
   config_apply_planner_settings(cfg, root);
   config_apply_mdl_settings(cfg, root);
   config_apply_review_settings(cfg, root);

   cJSON *ws = cJSON_GetObjectItemCaseSensitive(root, "workspaces");
   if (cJSON_IsArray(ws))
   {
      int i = 0;
      cJSON *el;
      cJSON_ArrayForEach(el, ws)
      {
         if (i >= 64)
            break;
         /* Each entry is either a bare path string (provider defaults to the
          * shared co-located provider) or a {path, provider} object. */
         const char *path_str = NULL;
         const char *prov_str = NULL;
         const char *remote_str = NULL;
         const char *head_str = NULL;
         const char *sandbox_image_str = NULL;
         if (cJSON_IsString(el))
            path_str = el->valuestring;
         else if (cJSON_IsObject(el))
         {
            cJSON *p = cJSON_GetObjectItemCaseSensitive(el, "path");
            cJSON *pr = cJSON_GetObjectItemCaseSensitive(el, "provider");
            cJSON *rm = cJSON_GetObjectItemCaseSensitive(el, "remote");
            cJSON *hd = cJSON_GetObjectItemCaseSensitive(el, "head");
            cJSON *si = cJSON_GetObjectItemCaseSensitive(el, "sandbox_image");
            if (cJSON_IsString(p))
               path_str = p->valuestring;
            if (cJSON_IsString(pr))
               prov_str = pr->valuestring;
            if (cJSON_IsString(rm))
               remote_str = rm->valuestring;
            if (cJSON_IsString(hd))
               head_str = hd->valuestring;
            if (cJSON_IsString(si))
               sandbox_image_str = si->valuestring;
         }
         if (path_str && path_str[0])
         {
            /* A Windows-absolute path (C:\... / C:/...) is already rooted — a
             * detached workspace registered by a Windows thin client — so store
             * it verbatim rather than resolving it against the server's CWD. */
            int win_abs = (((path_str[0] >= 'A' && path_str[0] <= 'Z') ||
                            (path_str[0] >= 'a' && path_str[0] <= 'z')) &&
                           path_str[1] == ':' && (path_str[2] == '\\' || path_str[2] == '/'));
            /* Resolve relative workspace paths against CWD. */
            if (path_str[0] != '/' && !win_abs)
            {
               const char *base = NULL;
               char cwd_buf[MAX_PATH_LEN];
               if (getcwd(cwd_buf, sizeof(cwd_buf)))
                  base = cwd_buf;
               else
                  base = "/tmp";
               if (strcmp(path_str, ".") == 0)
                  snprintf(cfg->workspaces[i], MAX_PATH_LEN, "%s", base);
               else
                  snprintf(cfg->workspaces[i], MAX_PATH_LEN, "%s/%s", base, path_str);
            }
            else
               snprintf(cfg->workspaces[i], MAX_PATH_LEN, "%s", path_str);
            if (prov_str && prov_str[0])
               snprintf(cfg->workspace_providers[i], sizeof(cfg->workspace_providers[i]), "%s",
                        prov_str);
            else
               cfg->workspace_providers[i][0] = '\0';
            if (remote_str && remote_str[0])
               snprintf(cfg->workspace_vcs_remote[i], sizeof(cfg->workspace_vcs_remote[i]), "%s",
                        remote_str);
            else
               cfg->workspace_vcs_remote[i][0] = '\0';
            if (head_str && head_str[0])
               snprintf(cfg->workspace_vcs_head[i], sizeof(cfg->workspace_vcs_head[i]), "%s",
                        head_str);
            else
               cfg->workspace_vcs_head[i][0] = '\0';
            if (sandbox_image_str && sandbox_image_str[0])
               snprintf(cfg->workspace_sandbox_image[i], sizeof(cfg->workspace_sandbox_image[i]),
                        "%s", sandbox_image_str);
            else
               cfg->workspace_sandbox_image[i][0] = '\0';
            i++;
         }
      }
      cfg->workspace_count = i;
   }

   /* Cross-verification */
   config_parse_cross_verify_section(cfg, root);

   /* API retry settings */
   config_parse_retry_section(cfg, root);

   /* Agent iteration limits */
   item = cJSON_GetObjectItemCaseSensitive(root, "max_iterations");
   if (cJSON_IsNumber(item))
      cfg->max_iterations = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "max_iterations_delegate");
   if (cJSON_IsNumber(item))
      cfg->max_iterations_delegate = (int)item->valuedouble;

   /* Delegation depth/spawn limits */
   item = cJSON_GetObjectItemCaseSensitive(root, "max_delegation_depth");
   if (cJSON_IsNumber(item))
      cfg->max_delegation_depth = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "max_delegation_spawns");
   if (cJSON_IsNumber(item))
      cfg->max_delegation_spawns = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "max_background_processes");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->max_background_processes = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "background_threads");
   if (!cJSON_IsNumber(item))
      item = cJSON_GetObjectItemCaseSensitive(root, "compute_threads");
   if (!cJSON_IsNumber(item))
      item = cJSON_GetObjectItemCaseSensitive(root, "worker_threads");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->compute_threads = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "session_threads");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->session_threads = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "delegate_max_inflight");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->delegate_max_inflight = (int)item->valuedouble;

   /* Per-model/provider concurrency limits */
   config_parse_concurrency_section(cfg, root);

   /* Web search backend */
   config_parse_search_section(cfg, root);

   /* Dogfood logger */
   config_parse_dogfood_section(cfg, root);

   /* Identity / working-profile injection. Nested under `identity` so
    * future items (e.g. a working_profile_observation.enabled flag) can
    * land in the same block. */
   config_parse_identity_section(cfg, root);

   /* Tool result compaction (default set above; config file overrides). */
   config_parse_compact_section(cfg, root);

   /* Session/worktree cleanup policy */
   config_parse_sessions_section(cfg, root);

   /* Sandbox configuration */
   config_parse_sandbox_section(cfg, root);

   item = cJSON_GetObjectItemCaseSensitive(root, "prompt_tier");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->prompt_tier, sizeof(cfg->prompt_tier), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "prompt_file");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->prompt_file, sizeof(cfg->prompt_file), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "delegate_prompt_tier");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->delegate_prompt_tier, sizeof(cfg->delegate_prompt_tier), "%s",
               item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "ecomode");
   if (cJSON_IsBool(item))
      cfg->ecomode = cJSON_IsTrue(item) ? 1 : 0;

   /* LSP server configuration */
   config_parse_lsp_servers_section(cfg, root);

   config_parse_mcp_clients_section(cfg, root);

   config_parse_mcp_section(cfg, root);

   /* Rewind settings */
   config_parse_rewind_section(cfg, root);

   /* OpenTelemetry export */
   config_parse_otel_section(cfg, root);

   /* Integrity gate */
   config_parse_integrity_section(cfg, root);

   /* Virtual context assembly (session.virtual_context.*) */
   {
      config_parse_session_section(cfg, root);
   }

   /* Prompt-cache-aware deferred payload rewrite (transport.cache_aware_rewrite.*) */
   {
      config_parse_transport_section(cfg, root);
   }

   config_parse_computer_use(cfg, root);

   /* Neural-assisted semantic guardrails (guardrails.semantic.*) */
   {
      config_parse_guardrails_section(cfg, root);
   }

   config_parse_server_api(cfg, root); /* aimee.api.* (config_server_api.c) */

   /* aimee-kb public HTTP API and background ingest config (kb.*) */
   {
      config_parse_kb_section2(cfg, root);
   }

   /* Team API key proxy */
   cfg->proxy_url[0] = '\0';
   cfg->proxy_token[0] = '\0';
   item = cJSON_GetObjectItemCaseSensitive(root, "proxy_url");
   if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
      strncpy(cfg->proxy_url, item->valuestring, sizeof(cfg->proxy_url) - 1);
   item = cJSON_GetObjectItemCaseSensitive(root, "proxy_token");
   if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
      strncpy(cfg->proxy_token, item->valuestring, sizeof(cfg->proxy_token) - 1);

   /* Auxiliary model routing */
   config_parse_auxiliary_section(cfg, root);
   /* Model metadata refresh */
   config_parse_model_meta_section(cfg, root);
   /* Vector index strategy ([db2.vector]) */
   {
      config_parse_db2_section(cfg, root);
   }
   config_parse_ensemble_section(cfg, root);
   config_parse_roundtable_section(cfg, root);
   cJSON_Delete(root);
   /* Update mtime cache */
   {
      struct stat st;
      if (stat(path, &st) == 0)
      {
         memcpy(&g_config_cache, cfg, sizeof(g_config_cache));
         g_config_mtime = AIMEE_STAT_MTIM(st);
         g_config_size = st.st_size;
         g_config_ino = st.st_ino;
         snprintf(g_config_cache_path, sizeof(g_config_cache_path), "%s", path);
         g_config_cached = 1;
      }
   }

   return 0;
}

/* ---- live config snapshot: double-buffer + seqlock (live-config-reload P1a) ----
 *
 * A single writer (config_reload, serialized by g_snap_wlock) publishes a fresh config_t
 * into the inactive slot of a two-slot double buffer and flips the active index; readers
 * copy the active slot under a seqlock and retry if a publish raced them. config_t is a
 * flat POD, so the copy is a plain struct assignment. Additive in P1a — NOT yet wired into
 * config_load or any push trigger (that is P1b); the infra is here + unit-tested first. */
static config_t g_snap[2];
static _Atomic unsigned g_snap_seq = 0;    /* seqlock: even = stable, odd = writing */
static _Atomic unsigned g_snap_active = 0; /* index (0/1) of the live slot */
static uint64_t g_snap_token = 0;          /* content-hash of the active snapshot */
static _Atomic int g_snap_inited = 0;      /* atomic so the config_load wrapper's read is visible */
static pthread_mutex_t g_snap_wlock = PTHREAD_MUTEX_INITIALIZER;

/* Re-applier registry (P3): hooks run after a reload publishes, under g_snap_wlock. */
#define CONFIG_MAX_REAPPLIERS 16
static config_reapplier_fn g_reappliers[CONFIG_MAX_REAPPLIERS];
static int g_reapplier_count = 0;

void config_reload_register_reapplier(config_reapplier_fn fn)
{
   pthread_mutex_lock(&g_snap_wlock);
   if (fn && g_reapplier_count < CONFIG_MAX_REAPPLIERS)
      g_reappliers[g_reapplier_count++] = fn;
   pthread_mutex_unlock(&g_snap_wlock);
}

/* 1 once config_snapshot_init has seeded the live snapshot (server context). Read by the
 * config_load wrapper to decide snapshot-vs-file; only ever transitions 0 -> 1. */
static int config_snapshot_live(void)
{
   return atomic_load_explicit(&g_snap_inited, memory_order_acquire);
}

/* FNV-1a over the POD bytes. config_t is memset to 0 before every load (below) so padding
 * is deterministic and the token is stable for a given logical config. */
static uint64_t config_snapshot_token(const config_t *c)
{
   const unsigned char *p = (const unsigned char *)c;
   uint64_t h = 1469598103934665603ULL;
   for (size_t i = 0; i < sizeof *c; i++)
   {
      h ^= p[i];
      h *= 1099511628211ULL;
   }
   return h;
}

/* Publish `cfg` into the inactive slot and flip. Caller holds g_snap_wlock (single writer). */
static void config_snapshot_publish(const config_t *cfg)
{
   unsigned s = atomic_load_explicit(&g_snap_seq, memory_order_relaxed);
   atomic_store_explicit(&g_snap_seq, s + 1, memory_order_release); /* -> odd (writing) */
   unsigned nxt = atomic_load_explicit(&g_snap_active, memory_order_relaxed) ^ 1u;
   g_snap[nxt] = *cfg; /* fill the slot no reader is on */
   atomic_store_explicit(&g_snap_active, nxt, memory_order_release);
   g_snap_token = config_snapshot_token(cfg);
   atomic_store_explicit(&g_snap_seq, s + 2, memory_order_release); /* -> even (stable) */
   atomic_store_explicit(&g_snap_inited, 1, memory_order_release);
}

void config_snapshot_init(const config_t *cfg)
{
   if (!cfg)
      return;
   pthread_mutex_lock(&g_snap_wlock);
   g_snap_token = 0; /* force the first publish */
   config_snapshot_publish(cfg);
   pthread_mutex_unlock(&g_snap_wlock);
}

int config_snapshot_get(config_t *out)
{
   if (!out || !atomic_load_explicit(&g_snap_inited, memory_order_acquire))
      return -1;
   for (;;)
   {
      unsigned s0 = atomic_load_explicit(&g_snap_seq, memory_order_acquire);
      if (s0 & 1u)
         continue; /* a publish is in progress */
      unsigned act = atomic_load_explicit(&g_snap_active, memory_order_acquire);
      *out = g_snap[act]; /* POD copy */
      unsigned s1 = atomic_load_explicit(&g_snap_seq, memory_order_acquire);
      if (s0 == s1)
         return 0; /* stable — no publish raced the copy */
   }
}

int config_autonomy_lookup(const char *env_name, long *out)
{
   if (!env_name || !out)
      return 0;
   /* Operator override wins: an explicitly-exported env var (getenv is a safe read now that
    * nothing setenv's these). Otherwise the LIVE snapshot — so a config.set on autonomy.*
    * takes effect on the next workflow with no restart and no cross-thread setenv. */
   config_t c;
   int have = config_snapshot_get(&c) == 0;
   long snap = 0;
   int boolish = 0, is_autonomy = 1;
   if (strcmp(env_name, "AIMEE_AUTONOMY_SKEPTICS") == 0)
      snap = have ? c.autonomy_skeptics : 0;
   else if (strcmp(env_name, "AIMEE_AUTONOMY_FANOUT") == 0)
      snap = have ? c.autonomy_fanout : 0, boolish = 1;
   else if (strcmp(env_name, "AIMEE_AUTONOMY_UNIT_RETRY") == 0)
      snap = have ? c.autonomy_unit_retry : 0;
   else if (strcmp(env_name, "AIMEE_AUTONOMY_UNIT_MAX") == 0)
      snap = have ? c.autonomy_unit_max : 0;
   else if (strcmp(env_name, "AIMEE_AUTONOMY_CI_RETRY_MAX") == 0)
      snap = have ? c.autonomy_ci_retry_max : 0;
   else
      is_autonomy = 0;
   if (!is_autonomy)
      return 0; /* not a config-backed autonomy var (e.g. MAX_TURNS) -> caller falls back */

   const char *e = getenv(env_name);
   if (e && e[0]) /* operator override — VALIDATED (a garbage value falls through to snapshot) */
   {
      if (boolish)
      {
         *out = (e[0] == '1') ? 1 : 0;
         return 1;
      }
      char *end = NULL;
      long v = strtol(e, &end, 10);
      if (end && *end == '\0')
      {
         *out = v;
         return 1;
      }
   }
   if (have)
   {
      *out = snap; /* live snapshot value */
      return 1;
   }
   return 0;
}

int config_reload(void)
{
   /* Hold the writer lock across the WHOLE reload (load + validate + token + publish) so two
    * concurrent config_reload callers cannot race each other's config_load on the shared
    * g_config_cache. NOTE: config_load's g_config_cache is a pre-existing benign racy cache
    * shared with per-request config_load callers that are NOT under this lock; P1b removes
    * that exposure by moving the server's hot readers to config_snapshot_get (this seqlock
    * snapshot), after which config_load is only reached here (serialized) + by CLI one-shots. */
   pthread_mutex_lock(&g_snap_wlock);
   config_t fresh;
   memset(&fresh, 0, sizeof fresh);   /* zero padding so the token is stable */
   if (config_load_file(&fresh) != 0) /* always re-read DISK, never the snapshot we replace */
   {
      pthread_mutex_unlock(&g_snap_wlock);
      return -1; /* parse failure -> keep the running snapshot */
   }
   char err[256];
   if (config_reduce_validate(&fresh, err, sizeof err) != 0)
   {
      pthread_mutex_unlock(&g_snap_wlock);
      return -1; /* invalid -> keep the running snapshot (validate-or-keep) */
   }
   uint64_t tok = config_snapshot_token(&fresh);
   if (atomic_load_explicit(&g_snap_inited, memory_order_acquire) && tok == g_snap_token)
   {
      pthread_mutex_unlock(&g_snap_wlock);
      return 0; /* self-reload no-op guard: nothing logically changed */
   }
   /* capture the OLD snapshot (if any) so re-appliers can diff their section, then publish. */
   config_t old;
   int have_old =
       atomic_load_explicit(&g_snap_inited, memory_order_acquire) && config_snapshot_get(&old) == 0;
   config_snapshot_publish(&fresh);
   for (int i = 0; i < g_reapplier_count; i++)
      g_reappliers[i](have_old ? &old : &fresh, &fresh);
   pthread_mutex_unlock(&g_snap_wlock);
   return 1; /* a new snapshot was published */
}
