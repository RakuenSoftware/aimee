/* config.c: app configuration loading/saving (~/.config/aimee/aimee.yaml).
 *
 * The on-disk format is YAML; the in-memory model is still cJSON. The
 * yaml.c shim handles parse/emit so this file's schema-extraction code
 * never sees the format change. */
#include <stdarg.h>
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
   static char path[MAX_PATH_LEN];
   static char cached_dir[MAX_PATH_LEN];
   const char *dir = config_default_dir();

   if (path[0] && strcmp(cached_dir, dir) == 0)
      return path;

   snprintf(path, sizeof(path), "%s/aimee.yaml", dir);
   snprintf(cached_dir, sizeof(cached_dir), "%s", dir);
   return path;
}

const char *config_output_dir(void)
{
   static char fallback[MAX_PATH_LEN];
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

   static char path[MAX_PATH_LEN];
   const char *dir = config_default_dir();
   snprintf(path, sizeof(path), "%s/aimee.db", dir ? dir : "/tmp");
   return path;
}

/* AIMEE_STAT_MTIM macro and g_config_cache / g_config_mtime / g_config_cached
 * are declared in config_internal.h so config_save.c can stamp the cache
 * after a write; defined below. */
config_t g_config_cache;
struct timespec g_config_mtime;
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
#include "config_schema.inc"
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
   /* Embedding dimension of the default embedder (pplx-embed-v1-0.6b = 1024).
    * Must match the schema vector(N) columns and the embedder model. Set to
    * 2560 (with the pplx-embed-v1-4b embedder image) for the higher-fidelity tier. */
   cfg->embedding_dim = 1024;
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
   cfg->ingress_preinject_assembly_budget = 6144;
   cfg->ingress_max_raw_scans = 0;
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
   cfg->kb_api_http_port = 0;
   cfg->kb_api_bearer_token[0] = '\0';
   cfg->kb_worker_count = CONFIG_DEFAULT_KB_WORKER_THREADS;
   cfg->kb_connection_workers = 2;
   cfg->kb_bg_ingest_enabled = 1;
   cfg->kb_bg_ingest_interval_hours = 6;
#ifdef __linux__
   cfg->kb_bg_watch_enabled = 1;
#else
   cfg->kb_bg_watch_enabled = 0;
#endif
   cfg->kb_bg_watch_debounce_secs = 30;
   cfg->kb_maintenance_enabled = 0;
   cfg->kb_maintenance_interval_hours = 24;
   cfg->kb_maintenance_lambda = 0.005;
   cfg->kb_maintenance_floor = 0.10;
   cfg->kb_maintenance_min_age_days = 7;
   cfg->kb_maintenance_orphan_days = 90;
   cfg->kb_mining_enabled = 1;
   cfg->kb_mining_min_poll_s = 300;
   cfg->review_scheduler_enabled = 0;
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
   cfg->skills_min_idle_minutes = 30;
   cfg->skills_manage_enabled = 0;
   cfg->skills_dispatch_enabled = 1;
   cfg->skills_dispatch_max_index = 24;
   cfg->skills_dispatch_advisory = 0;
   cfg->skills_capability_autostub = 0;
   cfg->skills_eval_gate_enabled = 0;
   cfg->skills_eval_threshold = 0.01;
   cfg->css_style_graph_enabled = 1; /* default-on: the indexer builds the CSS style
                                        graph so the read-only css signals/report work
                                        out of the box (set false to opt out) */
   snprintf(cfg->css_render_command, sizeof(cfg->css_render_command), "%s",
            CONFIG_DEFAULT_CSS_RENDER_COMMAND); /* default-on render backend (inert
                                                   until the sidecar is up); set empty
                                                   to disable */
   cfg->worktree_gc_enabled = 0;
   cfg->worktree_gc_max_age_days = 14;
   cfg->model_meta_refresh_minutes = 60;
   cfg->model_meta_capability_routing = 0;
   snprintf(cfg->db2_vector_corpus_index, sizeof(cfg->db2_vector_corpus_index), "auto");
   cfg->db2_vector_corpus_diskann_threshold = 1000000;
   cfg->ensemble_min_successful = 2;
   cfg->ensemble_max_cost_usd = 0.0; /* 0 = no cost cap (unlimited) by default */
   cfg->roundtable_max_rounds = 3;
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
   cfg->identity_working_profile_injection_enabled = 0;
   cfg->identity_working_profile_injection_fields_count = 0;
   cfg->memory_recall_lanes_floor_summary = 4;
   cfg->memory_recall_lanes_floor_fact = 4;
   snprintf(cfg->openai_endpoint, sizeof(cfg->openai_endpoint), "https://api.openai.com/v1");
   snprintf(cfg->openai_model, sizeof(cfg->openai_model), "gpt-4o");
   cfg->openai_key_cmd[0] = '\0';
   cfg->workspace_count = 0;
   config_parse_database(cfg, NULL);
}

int config_load(config_t *cfg)
{
   config_set_defaults(cfg);

   const char *path = config_default_path();

   /* Return cached config if mtime unchanged and caching enabled */
   if (!getenv("AIMEE_NO_CACHE") && g_config_cached)
   {
      struct stat st;
      if (stat(path, &st) == 0)
      {
         struct timespec mt = AIMEE_STAT_MTIM(st);
         if (strcmp(g_config_cache_path, path) == 0 && timespec_eq(&mt, &g_config_mtime))
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

   item = cJSON_GetObjectItemCaseSensitive(root, "server_cli_oauth_enabled");
   if (cJSON_IsBool(item))
      cfg->server_cli_oauth_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "verify_cross_project");
   if (cJSON_IsBool(item))
      cfg->verify_cross_project = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "ingress_preinject_enabled");
   if (cJSON_IsBool(item))
      cfg->ingress_preinject_enabled = cJSON_IsTrue(item);

   /* CSS migration assistant style-graph write path (WP-C). The field +
    * descriptor + save existed, but the YAML load parse was missing, so the
    * flag never took effect during indexing. */
   item = cJSON_GetObjectItemCaseSensitive(root, "css_style_graph_enabled");
   if (cJSON_IsBool(item))
      cfg->css_style_graph_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "css_render_command");
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(cfg->css_render_command, sizeof(cfg->css_render_command), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "ingress_preinject_assembly_budget");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->ingress_preinject_assembly_budget = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "ingress_max_raw_scans");
   if (cJSON_IsNumber(item) && item->valuedouble >= 0)
      cfg->ingress_max_raw_scans = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "typed_facts_enabled");
   if (cJSON_IsBool(item))
      cfg->typed_facts_enabled = cJSON_IsTrue(item);

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

   item = cJSON_GetObjectItemCaseSensitive(root, "memory_weight_profile");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->memory_weight_profile, sizeof(cfg->memory_weight_profile), "%s",
               item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "memory_rerank_mode");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->memory_rerank_mode, sizeof(cfg->memory_rerank_mode), "%s", item->valuestring);

   config_parse_memory_rewrite_section(cfg, root);

   config_parse_memory_negation_section(cfg, root);

   config_parse_memory_rerank_section(cfg, root);

   config_parse_memory_query_expansion_section(cfg, root);

   config_parse_memory_recall_lanes_section(cfg, root);

   config_parse_memory_window_section(cfg, root);

   config_parse_kb_section(cfg, root);

   config_parse_memory_maintenance_section(cfg, root);

   config_parse_memory_section(cfg, root);
   config_apply_learning_settings(cfg, root);
   config_apply_calibration_settings(cfg, root);
   config_apply_demotion_settings(cfg, root);
   config_apply_ranking_settings(cfg, root);
   config_apply_reasoning_settings(cfg, root);
   config_apply_bandit_settings(cfg, root);
   config_apply_planner_settings(cfg, root);
   config_apply_mdl_settings(cfg, root);

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
         if (cJSON_IsString(el))
            path_str = el->valuestring;
         else if (cJSON_IsObject(el))
         {
            cJSON *p = cJSON_GetObjectItemCaseSensitive(el, "path");
            cJSON *pr = cJSON_GetObjectItemCaseSensitive(el, "provider");
            cJSON *rm = cJSON_GetObjectItemCaseSensitive(el, "remote");
            cJSON *hd = cJSON_GetObjectItemCaseSensitive(el, "head");
            if (cJSON_IsString(p))
               path_str = p->valuestring;
            if (cJSON_IsString(pr))
               prov_str = pr->valuestring;
            if (cJSON_IsString(rm))
               remote_str = rm->valuestring;
            if (cJSON_IsString(hd))
               head_str = hd->valuestring;
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
         snprintf(g_config_cache_path, sizeof(g_config_cache_path), "%s", path);
         g_config_cached = 1;
      }
   }

   return 0;
}
