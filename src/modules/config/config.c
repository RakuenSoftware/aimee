/* config.c: app configuration loading/saving (~/.config/aimee/aimee.yaml).
 *
 * The on-disk format is YAML; the in-memory model is still cJSON. The
 * yaml.c shim handles parse/emit so this file's schema-extraction code
 * never sees the format change. */
#include <pthread.h>
#include <limits.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include "aimee.h"
#include "json_fluent.h"
#include "aimee_home.h"
#include "config_database.h"
#include "config_fields.h"
#include "config_internal.h"
#include "config_sections.h"
#include "config_learning.h"
#include "config_memory.h"
#include "db1_optional.h"
#include "maintenance.h"
#include "platform_process.h"
#include "platform_path.h"
#include "runtime_secret.h"
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
static config_secret_writer_fn g_secret_writer;

void config_secret_writer_set(config_secret_writer_fn writer)
{
   g_secret_writer = writer;
}

int config_secret_store(const char *name, const char *value)
{
   return g_secret_writer ? g_secret_writer(name, value ? value : "") : -1;
}

int config_migrate_legacy_credentials(config_secret_writer_fn writer,
                                      config_secret_present_fn present)
{
   config_t cfg;
   if (!writer || config_load_file(&cfg) != 0)
      return -1;

   /* These two fields were historically ONE credential, so a legacy file with
    * conflicting plaintext values is ambiguous: refuse rather than guess, unless
    * Vault already holds the authoritative value (both copies then safe to
    * scrub). They are no longer required to match going forward — aimee-server
    * may carry its own scoped `service` token under
    * AIMEE_KB_CLIENT_BEARER_TOKEN — but this migration only knows how to move
    * the single legacy value. */
   if (cfg.kb_api_bearer_token[0] && cfg.kb_client_bearer_token[0] &&
       strcmp(cfg.kb_api_bearer_token, cfg.kb_client_bearer_token) != 0 &&
       (!present || !present("AIMEE_KB_API_BEARER_TOKEN")))
   {
      runtime_secret_wipe(&cfg, sizeof(cfg));
      return -1;
   }

   int scrubbed = 0;
   int failed = 0;
#define MIGRATE_LEGACY_SECRET(name_, field_)                                                       \
   do                                                                                              \
   {                                                                                               \
      if (cfg.field_[0])                                                                           \
      {                                                                                            \
         if ((!present || !present((name_))) && writer((name_), cfg.field_) != 0)                  \
            failed = 1;                                                                            \
         if (!failed)                                                                              \
         {                                                                                         \
            runtime_secret_wipe(cfg.field_, sizeof(cfg.field_));                                   \
            scrubbed = 1;                                                                          \
         }                                                                                         \
      }                                                                                            \
   } while (0)
   MIGRATE_LEGACY_SECRET("AIMEE_DB2_URL", db2_url);
   MIGRATE_LEGACY_SECRET("AIMEE_SEARCH_TAVILY_API_KEY", search_tavily_api_key);
   MIGRATE_LEGACY_SECRET("AIMEE_PROXY_TOKEN", proxy_token);
   MIGRATE_LEGACY_SECRET("AIMEE_INGRESS_PROXY_SECRET", ingress_trusted_proxy_secret);
   MIGRATE_LEGACY_SECRET("AIMEE_KB_API_BEARER_TOKEN", kb_api_bearer_token);
   MIGRATE_LEGACY_SECRET("AIMEE_TELEMETRY_METRICS_TOKEN", telemetry_metrics_token);
   MIGRATE_LEGACY_SECRET("AIMEE_KB_API_BEARER_TOKEN", kb_client_bearer_token);
   MIGRATE_LEGACY_SECRET("AIMEE_TRIGGER_AUTH_TOKEN", trigger_auth_token);
   MIGRATE_LEGACY_SECRET("AIMEE_KB_CURATOR_PROVIDER_API_KEY", kb_curator_provider_api_key);
   MIGRATE_LEGACY_SECRET("AIMEE_API_BEARER_TOKEN", server_api_bearer_token);
#undef MIGRATE_LEGACY_SECRET

   for (int i = 0; i < AIMEE_API_BEARER_EXTRA_MAX && !failed; i++)
   {
      char name[96];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", i);
      if (!cfg.server_api_bearer_extra[i][0])
         continue;
      if ((!present || !present(name)) && writer(name, cfg.server_api_bearer_extra[i]) != 0)
      {
         failed = 1;
         break;
      }
      runtime_secret_wipe(cfg.server_api_bearer_extra[i], sizeof(cfg.server_api_bearer_extra[i]));
      scrubbed = 1;
   }
   if (!failed && cfg.server_api_bearer_extra_count)
   {
      cfg.server_api_bearer_extra_count = 0;
      scrubbed = 1;
   }

   int rc = failed ? -1 : scrubbed;
   if (!failed && scrubbed && config_save(&cfg) != 0)
      rc = -1;
   runtime_secret_wipe(&cfg, sizeof(cfg));
   return rc;
}

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

void config_file_id_from(const struct stat *st, config_file_id_t *out)
{
   if (!st || !out)
      return;
   out->mtime = AIMEE_STAT_MTIM(*st);
   out->size = st->st_size;
   out->ino = st->st_ino;
}

int config_file_id_eq(const config_file_id_t *a, const config_file_id_t *b)
{
   if (!a || !b)
      return 0;
   return a->mtime.tv_sec == b->mtime.tv_sec && a->mtime.tv_nsec == b->mtime.tv_nsec &&
          a->size == b->size && a->ino == b->ino;
}

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

/* Strict mode: errors instead of warnings, exit non-zero on validation failure.
 *
 * OPT-IN, and deliberately so. Nothing in the shipping binaries sets it, which
 * means config_load never returns non-zero in production: config_load_file only
 * fails when `issues > 0 && g_config_strict`. Every `if (config_load(&cfg) != 0)`
 * in the tree is therefore unreachable today, including server_main's own
 * "server startup rejected invalid configuration".
 *
 * Turning it on by default was tried and reverted -- it is not a free tightening.
 * Strict treats an UNKNOWN key as an error, but aimee deliberately tolerates and
 * PRESERVES keys it does not recognise: config_set patches the YAML in place so
 * an operator's own annotations survive a write (test_config_set.c pins
 * "custom_note: keep-me"). Defaulting strict on makes aimee refuse to load a
 * config it just preserved, and test_config.c:963 pins the opposite contract --
 * that strict DOES reject unknown keys. Both hold only while strict is opt-in.
 *
 * Making runtime validation fatal is a product decision, not a refactor: it needs
 * a split between "unknown key" (tolerate, for forward-compat and annotations)
 * and "known key, wrong shape" (refuse to start). See the config-t-encapsulation
 * proposal for the write-up. */
int g_config_strict;

static const config_schema_entry_t config_schema[] = {
    /* config_schema.inc: top-level config-key -> type allowlist, #included into the
     * config_schema[] initializer in config.c. Extracted so config.c stays under
     * the 2000-line cap and new keys have a low-churn home (one line each here). */
    {"db1_path", SCHEMA_STRING, 0},
    {"db2_pool_size", SCHEMA_INT, 0},
    {"kb_mode", SCHEMA_STRING, 0},
    {"synthesis_endpoint", SCHEMA_STRING, 0},
    {"synthesis_model", SCHEMA_STRING, 0},
    {"ingress_cache_placement_enabled", SCHEMA_BOOL, 0},
    {"ingress_compress_min_chars", SCHEMA_INT, 0},
    {"delegates_enabled", SCHEMA_BOOL, 0},
    {"prompt_manager_block_enabled", SCHEMA_BOOL, 0},
    {"prompt_manager_review_enabled", SCHEMA_BOOL, 0},
    {"ingress_preinject_assembly_budget", SCHEMA_INT, 0},
    {"ingress_max_raw_scans", SCHEMA_INT, 0},
    {"code_span_max_lines", SCHEMA_INT, 0},
    {"delegate_sandbox", SCHEMA_BOOL, 0},
    {"delegate_sandbox_image", SCHEMA_STRING, 0},
    {"delegate_sandbox_package_access", SCHEMA_STRING, 0}, /* valid: proxy|off|gated|governance */
    {"guardrails", SCHEMA_OBJECT, 0},
    {"toolsets", SCHEMA_OBJECT, 0},
    {"script", SCHEMA_OBJECT, 0},
    {"use_builtin_cli", SCHEMA_BOOL, 0},
    {"codex_model", SCHEMA_STRING, 0},
    {"model_reasoning_effort", SCHEMA_STRING, 0},
    {"embedder_dims", SCHEMA_INT, 0},
    {"memory_weight_profile", SCHEMA_STRING, 0},
    {"memory_query_expansion", SCHEMA_OBJECT, 0},
    {"memory_recall_lanes", SCHEMA_OBJECT, 0},
    {"memory_maintenance", SCHEMA_OBJECT, 0},
    {"memory", SCHEMA_OBJECT, 0},
    {"workspaces", SCHEMA_ARRAY, 0},
    {"retry", SCHEMA_OBJECT, 0},
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
    {"modules", SCHEMA_OBJECT, 0},    /* memory/governance/delegates/workflows/economizer toggles */
    {"economizer", SCHEMA_OBJECT, 0}, /* {mode: off|safe|aggressive} */
    {"sessions", SCHEMA_OBJECT, 0},
    {"sandbox", SCHEMA_OBJECT, 0},
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
    {"extended_thinking", SCHEMA_OBJECT, 0},
    {"ingress", SCHEMA_OBJECT, 0},
    {"dogfood", SCHEMA_OBJECT, 0},
    {"learning", SCHEMA_OBJECT, 0},
    {"intelligence", SCHEMA_OBJECT, 0},
    {"kb", SCHEMA_OBJECT, 0},
    /* Both of these are REAL, parsed keys that were missing from this allowlist,
     * so an operator config containing either drew "unknown key". That was a
     * warning while strict mode was off; with strict on it is fatal, and
     * worktree_gc is the worse of the two -- config_save WRITES it
     * (config_save.c:610), so aimee emitted a config it would then refuse to
     * load. Parsed at config_sections.c:147 and :808 respectively. */
    {"worktree_gc", SCHEMA_OBJECT, 0},
    {"autonomy", SCHEMA_OBJECT, 0},
    {"context", SCHEMA_OBJECT, 0},
    {"routing", SCHEMA_OBJECT, 0},
    {"telemetry", SCHEMA_OBJECT, 0},
    {"memory_window", SCHEMA_OBJECT, 0},
    {"memory_rewrite", SCHEMA_OBJECT, 0},
    {"memory_negation", SCHEMA_OBJECT, 0},
    {"delegate_max_inflight", SCHEMA_INT, 0},
    {"cross_verify", SCHEMA_BOOL_OR_OBJECT, 0},
    {"charter", SCHEMA_OBJECT, 0},
    {"identity", SCHEMA_OBJECT, 0},
    {"skills", SCHEMA_OBJECT, 0},
    {"auxiliary", SCHEMA_OBJECT, 0},
    {"model_meta", SCHEMA_OBJECT, 0},
    {"db2", SCHEMA_OBJECT, 0},
    {"vault", SCHEMA_OBJECT, 0},
    {"ensemble", SCHEMA_OBJECT, 0},
    {"roundtable", SCHEMA_OBJECT, 0},
    {"cron_jobs", SCHEMA_ARRAY, 0},
    {"aimee", SCHEMA_OBJECT, 0},
    {"trigger", SCHEMA_OBJECT, 0},
    {"trigger_rules", SCHEMA_ARRAY, 0},
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
   case SCHEMA_BOOL_OR_OBJECT:
      return "boolean or object";
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
   case SCHEMA_BOOL_OR_OBJECT:
      return cJSON_IsBool(item) || cJSON_IsObject(item);
   }
   return 0;
}

/* Map a config_fields[] descriptor type to the schema validation type, so a flat
 * scalar can be validated against config_fields[] without a duplicate config_schema[]
 * row (Proposal A, step 2). Returns 0 for types that are not a plain top-level
 * scalar (none such reach here — flat fields are STRING/BOOL/INT). */
static int config_field_schema_type(config_field_type_t t, schema_type_t *out)
{
   switch (t)
   {
   case CFG_STRING:
      *out = SCHEMA_STRING;
      return 1;
   case CFG_BOOL:
      *out = SCHEMA_BOOL;
      return 1;
   case CFG_INT:
   case CFG_FLOAT: /* schema has no float type; a number validates as SCHEMA_INT */
      *out = SCHEMA_INT;
      return 1;
   case CFG_ECON_MODE: /* stored int enum, written as an "off|safe|aggressive" string */
      *out = SCHEMA_STRING;
      return 1;
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
         /* Flat scalar keys are not hand-listed in config_schema[] — they are
          * derived from config_fields[] (Proposal A, step 2), the single source
          * of truth for every get/set-able top-level scalar. A key the schema
          * does not name but config_fields[] does is a known flat scalar; validate
          * its JSON type against the descriptor's type. Only genuinely unknown
          * keys (in neither table) are reported. */
         schema_type_t derived;
         const config_field_t *ff = config_field_lookup(item->string);
         if (ff && config_field_schema_type(ff->type, &derived))
         {
            if (!schema_type_matches(derived, item))
            {
               fprintf(stderr, "aimee: config %s: \"%s\" expected %s, got %s\n", level,
                       item->string, schema_type_name(derived), jo_type_name(item));
               issues++;
            }
            continue;
         }
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

int config_sandbox_package_access_valid(const char *s)
{
   return s && (strcmp(s, "proxy") == 0 || strcmp(s, "off") == 0 || strcmp(s, "gated") == 0 ||
                strcmp(s, "governance") == 0);
}
int config_parse_computer_use(config_t *cfg, const cJSON *root);

/* Drive the 5 kb_pdf_*_enabled stage gates from the kb_pdf_tier preset. Reader-free
 * (consumers keep reading the individual gates). "off" (default) = plain pdftotext,
 * "basic" = ingest + vector, "full" = every stage. Unknown -> "off" (fail-closed to
 * the historical default). config_load_file applies the tier before the per-stage
 * keys, so an explicit gate still overrides. */
static void kb_pdf_apply_tier(config_t *cfg, const char *tier)
{
   int basic = tier && strcmp(tier, "basic") == 0;
   int full = tier && strcmp(tier, "full") == 0;
   cfg->kb_pdf_ingest_enabled = basic || full;
   cfg->kb_pdf_vector_enabled = basic || full;
   cfg->kb_pdf_tsr_enabled = full;
   cfg->kb_pdf_assets_enabled = full;
   cfg->kb_pdf_ocr_enabled = full;
}

static void config_set_defaults(config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));

   /* Flat-field defaults are table-driven (config_flat_defaults[] in config_fields.c),
    * so each lives in exactly one place. Non-flat defaults (side effects, env-derived,
    * or computed) are set explicitly below. */
   config_apply_flat_defaults(cfg);

   /* Defaults */
   snprintf(cfg->db1_path, sizeof(cfg->db1_path), "%s", config_default_db1_path());
   /* Default-ON: co-located shell execution is namespace-isolated to the workspace.
    * This is the mode the delegate shell guard in tool_bash() reads (it refuses a
    * delegated shell when the mode is OFF), so leaving it at the SANDBOX_MODE_OFF
    * zero value made that guard refuse EVERY co-located delegate shell on an
    * unconfigured install — isolation-by-default and delegate-shells-work-by-default
    * cannot both hold while delegates are always containerized and this defaults off.
    * Non-flat (sandbox is a SCHEMA_OBJECT section), so the default lives here.
    * Opt out with `sandbox: {"mode": "off"}` — config_save persists that opt-out. */
   cfg->sandbox.mode = SANDBOX_MODE_WORKSPACE_ONLY;
   snprintf(cfg->delegate_sandbox_package_access, sizeof(cfg->delegate_sandbox_package_access),
            "proxy");
   cfg->compact_enabled = 1;     /* default on; set before no-config early returns */
   cfg->compact_from_record = 0; /* default-off until the quality baseline exists */
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
   /* fold §4: default-ON. The page table is what makes eviction REVERSIBLE — without
    * it a folded coordinate is simply gone, and the agent re-derives it. It only ever
    * ADDS a bounded hint when the newest turn re-touches something already evicted, so
    * the downside is a few lines of text and the upside is not losing the thread. */
   cfg->fold_recall_enabled = 1;
   cfg->fold_recall_ttl_turns = 0;
   /* SAFE is useful without provider-specific pricing guesses: it only compacts
    * strict JSON returned by a local tool before that result's first dispatch. */
   cfg->economizer_mode = ECON_MODE_SAFE;
   /* Pluggable-module toggles default to -1 (unspecified) so the resolver falls back to each
    * module's deprecated env toggle / default-ON until an operator writes the `modules:` block. */
   cfg->module_memory = -1;
   cfg->module_governance = -1;
   cfg->module_delegates = -1;
   cfg->module_workflows = -1;
   cfg->module_roundtable = -1;
   cfg->module_economizer = -1;
   /* Autonomous-dev knobs — defaults match the historical AIMEE_AUTONOMY_* env defaults
    * (adversarial + fan-out tiers OFF; retry/unit caps at their wfe defaults). */
   cfg->autonomy_skeptics = 0;
   cfg->autonomy_fanout = 0;
   cfg->autonomy_unit_retry = 2;
   cfg->autonomy_unit_max = 16;
   cfg->autonomy_ci_retry_max = 2;
   /* Run caps + auto-resume: defaults match the historical AIMEE_AUTONOMY_* env defaults
    * (max_turns 300, max_wall 1800s, stale-abandon 3600s, concurrency 8). Auto-resume of
    * wall-cap parks defaults ON so a long autonomous run drives to completion in fresh
    * wall windows instead of being reaped; bounded by max_resumes. */
   cfg->autonomy_max_turns = 300;
   cfg->autonomy_max_wall_secs = 1800;
   cfg->autonomy_stale_abandon_secs = 3600;
   cfg->autonomy_concurrency = 8;
   cfg->autonomy_auto_resume_cap_parks = 1;
   cfg->autonomy_max_resumes = 50;
   snprintf(cfg->memory_citations_mode, sizeof(cfg->memory_citations_mode), "%s", "off");
   snprintf(cfg->memory_coref_mode, sizeof(cfg->memory_coref_mode), "%s", "off");
   cfg->memory_cognify_async_enabled = 0;
   cfg->memory_scenes_enabled = 0;
   cfg->memory_scenes_global_escape_ratio = 0.2;
   cfg->memory_bm25_weight = 0.0;
   cfg->memory_semantic_weight = 0.0;
   cfg->memory_semantic_floor_scale = 0.0; /* 0 = auto-scale by embedding dim */
   cfg->memory_fetch_budget_enabled = 0;
   cfg->memory_fetch_budget_base = 128;
   cfg->memory_fetch_budget_shape_aware = 1;
   cfg->kb_search_max_results = 50;
   /* -1 = operator did not say. web_search_ex resolves it to the built-in
    * default; 0/1 are explicit off/on. memset-0 above would otherwise read as an
    * explicit "off" and silently disable page fetching. */
   cfg->search_fetch_pages = -1;
   /* structured-pdf Phase C blob reconciliation: default hourly sweep, alarm at 1 GiB of
    * reclaimable orphan bytes (config_t is memset-0 above, so these explicit values are the
    * defaults). The sweep is still a no-op until kb_pdf_assets_enabled is on. */
   /* Embedding dimension. 0 = UNSET (the operator did not pin a dim): readers fall
    * back to 1024 (db2_embedding_dim(), kb_main, kb_ingest_workers), and — crucially
    * — config_embedder_dims_is_pinned() reports NOT-pinned, so §2a's recorded-dim
    * preference and §2b's fresh-DB embedder /health probe can derive the real dim.
    * A non-zero value here means the operator explicitly pinned it (yaml/env), which
    * is authoritative and refuses a mismatch. (Was defaulted to 1024, which made
    * every deployment look "pinned" and silently disabled §2a/§2b.) */
   cfg->embedder_dims = 0;
   cfg->memory_routing_enabled = 1;
   /* Negation-aware retrieval defaults ON: for negatively-polarised queries it
    * promotes memories carrying the same negated concept (overlapping "not_<token>"
    * sets) so a negated fact ranks above its affirmative near-twin. It runs ONLY on
    * negative-polarity queries and is inert otherwise, and the deterministic
    * reranking is store-independent. A/B on the negation validation corpus (real
    * pgvector): negation/discrimination MRR 0.80 -> 0.91, positive controls and the
    * gated golden corpus unchanged. An explicit config value always wins. */
   cfg->memory_negation_enabled = 1;
   /* Typed-fact extraction runs fully OFFLINE (the memory_facts drain), so it costs
    * nothing per turn and defaults ON on every backend -- including the CPU-only
    * Gemma E4B/E2B fallback. HyDE query rewrite is still per-turn LLM work, so it
    * defaults OFF here and config_apply_inference_backend_defaults() flips it ON only
    * for an accelerated backend. An explicit config value always wins. HyDE mode
    * defaults on so the rewrite, once enabled, generates a hypothetical answer. */
   /* structured-PDF pipeline defaults OFF (plain pdftotext); the tier drives the 5
    * stage gates. See kb_pdf_apply_tier. */
   snprintf(cfg->kb_pdf_tier, sizeof(cfg->kb_pdf_tier), "off");
   kb_pdf_apply_tier(cfg, cfg->kb_pdf_tier);
   cfg->memory_rewrite_enabled = 0;
   cfg->memory_rewrite_hyde = 1;
   /* Replayable-evidence roundtable verification (Part A): default-on. config_t
    * is memset-0 above, so this explicit assignment is what makes the contract
    * hold (the config_fields[] row carries is_bool, not a default value). */
   cfg->roundtable_replay_verify_enabled = 1;
   cfg->roundtable_require_evidence = 1; /* evidence gate on: no-evidence findings dropped */
   cfg->roundtable_chair_synthesis = 1;  /* chair reasoning pass on (operator ruling 2026-07-17) */
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
   snprintf(cfg->session_worktree_base, sizeof(cfg->session_worktree_base), "remote_default");
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
   cfg->ingress_cache_placement_enabled = 1;
   /* All three default ON: this change adds off switches, it does not change what
    * a default install does. */
   cfg->delegates_enabled = 1;
   cfg->prompt_manager_block_enabled = 1;
   cfg->prompt_manager_review_enabled = 1;
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
   /* Default ON: agent-authored durable memories go through aimee's memory
    * system, not external per-harness memory files. Explicit
    * `require_aimee_memory: false` bypasses (see cli_attention_guard.c). */
   /* Default ON: a delegate never runs `git`/`gh` in a shell — git and forge
    * actions go through aimee's git_* tools and execute on aimee-server, where
    * the forge credential stays in-process. Explicit `require_aimee_git: false`
    * bypasses (see wfe_native_gate.c). */
   /* Default-on: the primary agent must not spawn its OWN sub-agents (Task/Agent/
    * spawn_agent/RemoteTrigger); delegation goes through `aimee delegate` so the
    * child inherits this session's guardrails. Enforced only when usable delegates
    * are configured; `subagent_ban_enabled: false` opts out. */
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
   /* Live three-node mTLS validation promoted connection reuse to the default:
    * pooled server->KB requests cut warm p50 from 4.762 ms to 1.589 ms, while
    * resident thin-client keep-alive cut p50 from 4.900 ms to 0.109 ms.
    * Both remain independently rollbackable through their transport settings. */
   cfg->transport_kb_pool_enabled = 1;
   cfg->transport_server_keepalive_enabled = 1;
   /* gzip saved wire bytes but increased latency on the measured LAN, so both
    * compression directions remain opt-in pending a qualifying remote profile. */
   cfg->transport_thinclient_gzip_enabled = 0;
   cfg->transport_kb_gzip_enabled = 0;
   cfg->cost_reward_enabled = 0;
   cfg->cost_reward_lambda_pct = 30;
   cfg->cost_reward_ref_usd_milli = 500;
   cfg->reasoning_cap_enabled = 0;
   cfg->dedup_enabled = 1;         /* default-ON: only acts on caller Idempotency-Key requests */
   cfg->cache_shaping_enabled = 1; /* default-ON: cost win, marks aimee's own Anthropic prefix */
   /* default-OFF: thinking tokens are billed, so enabling this changes spend. */
   cfg->extended_thinking_enabled = 0;
   cfg->ingress_usage_accounting_enabled = 1; /* default-ON: begin ingress cost accounting */
   cfg->ingress_audit_async = 1; /* default-ON: keep the cost-row write off the response path */
   cfg->ingress_trusted_proxy_secret[0] = '\0';
   cfg->dedup_window_seconds = 5;
   cfg->cache_min_chars = 0;
   snprintf(cfg->guardrails_semantic_mode, sizeof(cfg->guardrails_semantic_mode), "off");
   cfg->guardrails_semantic_command[0] = '\0';
   cfg->guardrails_semantic_warn_threshold = 0.40;
   cfg->guardrails_semantic_prompt_threshold = 0.70;
   cfg->guardrails_semantic_block_threshold = 0.90;
   cfg->guardrails_blast_radius_advisory_enabled = 0;
   cfg->kb_api_http_port = 0;
   cfg->kb_api_bearer_token[0] = '\0';
   cfg->telemetry_metrics_token[0] = '\0';
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
   cfg->skills_stale_after_days = 30;
   cfg->skills_archive_after_days = 90;
   cfg->skills_dispatch_enabled = 1;
   cfg->skills_dispatch_max_index = 24;
   cfg->skills_dispatch_advisory = 0;
   cfg->skills_capability_autostub = 0;
   cfg->skills_eval_gate_enabled = 0;
   cfg->skills_eval_threshold = 0.01;
   snprintf(cfg->vault_custody, sizeof(cfg->vault_custody), "file"); /* default custody
                                                                        (self-unsealing) */
   cfg->vault_tpm2_blob_path[0] = '\0'; /* empty -> <config>/vault/tpm2-kek.blob at use */
   snprintf(cfg->vault_tpm2_tcti, sizeof(cfg->vault_tpm2_tcti), "%s",
            CONFIG_DEFAULT_VAULT_TPM2_TCTI);
   snprintf(cfg->vault_tpm2_nv_index, sizeof(cfg->vault_tpm2_nv_index), "%s",
            CONFIG_DEFAULT_VAULT_TPM2_NV_INDEX);
   cfg->worktree_gc_enabled = 1;
   cfg->worktree_gc_max_age_days = 14;
   cfg->prefer_local_agents = 0;
   cfg->model_meta_refresh_minutes = 60;
   /* Capability routing ON by default. Routing previously consulted cost_tier
    * and role support only, so a packet could be handed to a model whose context
    * window could not hold it — the failure surfaced as a provider error rather
    * than a routing decision. Safe to default on now that the gate FAILS UPWARD
    * (agent_route_with_caps escalates to the most capable seat instead of
    * returning no route), so enabling it cannot cost an operator a route they
    * had. Set model_meta.capability_routing=false to restore cost-tier-only. */
   cfg->model_meta_capability_routing = 1;
   snprintf(cfg->db2_vector_corpus_index, sizeof(cfg->db2_vector_corpus_index), "auto");
   cfg->db2_vector_corpus_diskann_threshold = 1000000;
   cfg->ensemble_min_successful = 2;
   cfg->ensemble_max_cost_usd = 0.0; /* 0 = no cost cap (unlimited) by default */
   cfg->roundtable_max_rounds = 1;
   cfg->roundtable_converge_threshold = 10;
   /* Ten-minute default safety bound for a complete roundtable. It remains
    * overridable via roundtable.deadline_ms in GUI and CLI configuration. */
   cfg->roundtable_deadline_ms = 600000;
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

static int config_snapshot_live(void);

static void config_apply_runtime_secrets(config_t *cfg)
{
   if (!cfg)
      return;
#define APPLY_RUNTIME_SECRET(name_, field_)                                                        \
   do                                                                                              \
   {                                                                                               \
      char _value[sizeof(cfg->field_)];                                                            \
      if (runtime_secret_get((name_), _value, sizeof(_value)))                                     \
         snprintf(cfg->field_, sizeof(cfg->field_), "%s", _value);                                 \
      runtime_secret_wipe(_value, sizeof(_value));                                                 \
   } while (0)
   APPLY_RUNTIME_SECRET("AIMEE_DB2_URL", db2_url);
   APPLY_RUNTIME_SECRET("AIMEE_SEARCH_TAVILY_API_KEY", search_tavily_api_key);
   APPLY_RUNTIME_SECRET("AIMEE_PROXY_TOKEN", proxy_token);
   APPLY_RUNTIME_SECRET("AIMEE_INGRESS_PROXY_SECRET", ingress_trusted_proxy_secret);
   APPLY_RUNTIME_SECRET("AIMEE_KB_API_BEARER_TOKEN", kb_api_bearer_token);
   APPLY_RUNTIME_SECRET("AIMEE_TELEMETRY_METRICS_TOKEN", telemetry_metrics_token);
   /* What aimee-server PRESENTS to aimee-kb, which is no longer required to be
    * aimee-kb's own inbound token: its own record lets it carry a scoped
    * `service` credential while aimee-kb keeps an unscoped owner token. The
    * shared key is still read as a FALLBACK so an existing deployment that only
    * ever set AIMEE_KB_API_BEARER_TOKEN keeps working unchanged. */
   APPLY_RUNTIME_SECRET("AIMEE_KB_API_BEARER_TOKEN", kb_client_bearer_token);
   APPLY_RUNTIME_SECRET("AIMEE_KB_CLIENT_BEARER_TOKEN", kb_client_bearer_token);
   APPLY_RUNTIME_SECRET("AIMEE_TRIGGER_AUTH_TOKEN", trigger_auth_token);
   APPLY_RUNTIME_SECRET("AIMEE_KB_CURATOR_PROVIDER_API_KEY", kb_curator_provider_api_key);
   APPLY_RUNTIME_SECRET("AIMEE_API_BEARER_TOKEN", server_api_bearer_token);
#undef APPLY_RUNTIME_SECRET
   cfg->server_api_bearer_extra_count = 0;
   for (int i = 0; i < AIMEE_API_BEARER_EXTRA_MAX; i++)
   {
      char name[96];
      char value[sizeof(cfg->server_api_bearer_extra[0])];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", i);
      if (!runtime_secret_get(name, value, sizeof(value)))
      {
         runtime_secret_wipe(value, sizeof(value));
         break;
      }
      snprintf(cfg->server_api_bearer_extra[i], sizeof(cfg->server_api_bearer_extra[i]), "%s",
               value);
      runtime_secret_wipe(value, sizeof(value));
      cfg->server_api_bearer_extra_count++;
   }
}

/* Public config read. In the SERVER (once config_snapshot_init has seeded the live snapshot)
 * this returns the current snapshot — a lock-free POD copy that reflects the last reload
 * IMMEDIATELY (push-driven), with no file I/O or mtime-cache-miss wait. Everywhere else (CLI
 * one-shots, and before startup seeds it) it reads the file. config_reload uses the from-file
 * path directly so a reload always re-reads disk, never the snapshot it is about to replace. */
int config_load(config_t *cfg)
{
   int rc;
   if (config_snapshot_live())
      rc = config_snapshot_get(cfg);
   else
      rc = config_load_file(cfg);
   if (rc == 0)
      config_apply_runtime_secrets(cfg);
   return rc;
}

int config_cache_disabled(void)
{
   return getenv("AIMEE_NO_CACHE") != NULL;
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
   if (!config_cache_disabled() && g_config_cached)
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
      char toolset_err[TOOLSET_ERROR_MAX] = "";
      /* toolset_registry_t is large enough to add over a MiB to this function's
       * frame after LTO. Config loads occur deep inside memory-query call chains,
       * so a stack copy can exhaust the default 8 MiB worker stack. */
      toolset_registry_t *registry = calloc(1, sizeof(*registry));
      if (!registry)
      {
         fprintf(stderr, "aimee: config validation: out of memory loading toolsets\n");
         issues++;
      }
      else
      {
         toolset_registry_init(registry);
         if (toolset_registry_load_file(registry, path, toolset_err, sizeof(toolset_err)) != 0)
         {
            fprintf(stderr, "aimee: config validation: %s\n",
                    toolset_err[0] ? toolset_err : "invalid toolset configuration");
            issues++;
         }
         free(registry);
      }
   }
#else
   if (validate_toolsets)
   {
      char toolset_err[TOOLSET_ERROR_MAX] = "";
      toolset_registry_t *registry = calloc(1, sizeof(*registry));
      if (!registry)
      {
         fprintf(stderr, "aimee: config validation: out of memory loading toolsets\n");
         issues++;
      }
      else
      {
         toolset_registry_init(registry);
         if (toolset_registry_load_file(registry, path, toolset_err, sizeof(toolset_err)) != 0)
         {
            fprintf(stderr, "aimee: config validation: %s\n",
                    toolset_err[0] ? toolset_err : "invalid toolset configuration");
            issues++;
         }
         free(registry);
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

   /* Flat scalar fields are parsed table-driven from config_fields[] (Proposal A,
    * step 3): one loop replaces the per-field inline blocks below. */
   config_parse_flat_fields(cfg, root);

   item = cJSON_GetObjectItemCaseSensitive(root, "codex_model");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->codex_model, sizeof(cfg->codex_model), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "model_reasoning_effort");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->model_reasoning_effort, sizeof(cfg->model_reasoning_effort), "%s",
               item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "ingress_cache_placement_enabled");
   if (cJSON_IsBool(item))
      cfg->ingress_cache_placement_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "ingress_compress_min_chars");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->ingress_compress_min_chars = (int)item->valuedouble;

   item = cJSON_GetObjectItemCaseSensitive(root, "delegates_enabled");
   if (cJSON_IsBool(item))
      cfg->delegates_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "prompt_manager_block_enabled");
   if (cJSON_IsBool(item))
      cfg->prompt_manager_block_enabled = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "prompt_manager_review_enabled");
   if (cJSON_IsBool(item))
      cfg->prompt_manager_review_enabled = cJSON_IsTrue(item);

   /* CSS migration assistant style-graph write path (WP-C). The field +
    * descriptor + save existed, but the YAML load parse was missing, so the
    * flag never took effect during indexing. */

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

   /* require_aimee_git had a config_fields[] row, a schema row, a default, a
    * config_save writer and NO parse — so `require_aimee_git: false` in aimee.yaml
    * never loaded, and `aimee config set require_aimee_git false` persisted a value
    * that silently reverted to ON at the next restart. The operator escape hatch
    * cmd_hooks.c offers ("Operator: require_aimee_git: false ...") could not work.
    * Exactly the failure the comment below this block already warns about. */

   /* Default-on; parse the explicit opt-out so `subagent_ban_enabled: false` loads. */

   /* `delegate_sandbox` no longer selects anything: a delegate runs in a container
    * or not at all. Say so rather than ignore it silently — an operator who set it
    * to false chose an execution model that no longer exists, and their delegates
    * will now refuse to run instead of quietly running on the host. The key stays
    * in config_schema[] so it does not also draw "unknown key". */
   item = cJSON_GetObjectItemCaseSensitive(root, "delegate_sandbox");
   if (cJSON_IsBool(item) && !cJSON_IsTrue(item))
      fprintf(stderr, "aimee: config warning: `delegate_sandbox: false` is no longer honoured — a "
                      "delegate runs in its own container or not at all. Remove the key.\n");
   {
      const char *e = getenv("AIMEE_DELEGATE_SANDBOX");
      if (e && e[0] == '0')
         fprintf(stderr, "aimee: config warning: AIMEE_DELEGATE_SANDBOX=0 is no longer honoured — "
                         "a delegate runs in its own container or not at all.\n");
   }
   item = cJSON_GetObjectItemCaseSensitive(root, "delegate_sandbox_image");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->delegate_sandbox_image, sizeof(cfg->delegate_sandbox_image), "%s",
               item->valuestring);
   item = cJSON_GetObjectItemCaseSensitive(root, "delegate_sandbox_package_access");
   if (cJSON_IsString(item) && config_sandbox_package_access_valid(item->valuestring))
      snprintf(cfg->delegate_sandbox_package_access, sizeof(cfg->delegate_sandbox_package_access),
               "%s", item->valuestring);
   else if (cJSON_IsString(item)) /* present but not a valid mode (incl. "") — warn, keep default */
      fprintf(stderr,
              "aimee: config warning: delegate_sandbox_package_access: unknown value \"%s\" — "
              "keeping default \"proxy\" (valid: proxy, off, gated, governance)\n",
              item->valuestring);

   /* structured-PDF gates. These have config_fields[] rows (CLI/server-settable) but
    * historically lacked a file parse, so a value set in aimee.yaml never loaded back on a
    * fresh process. Parse them here as top-level bools so both the Phase-1/2 ingest gate
    * and the Phase-A vector gate are durably configurable. */
   /* Preset first: kb_pdf_tier drives all 5 gates; the per-stage keys below still
    * override it (back-compat with configs that pin individual stages). */
   item = cJSON_GetObjectItemCaseSensitive(root, "kb_pdf_tier");
   if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
   {
      snprintf(cfg->kb_pdf_tier, sizeof(cfg->kb_pdf_tier), "%s", item->valuestring);
      kb_pdf_apply_tier(cfg, cfg->kb_pdf_tier);
   }

   item = cJSON_GetObjectItemCaseSensitive(root, "embedder_dims");
   if (cJSON_IsNumber(item) && item->valuedouble > 0)
      cfg->embedder_dims = (int)item->valuedouble;

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
          {"synthesis_endpoint", offsetof(config_t, synthesis_endpoint),
           sizeof(((config_t *)0)->synthesis_endpoint)},
          {"synthesis_model", offsetof(config_t, synthesis_model),
           sizeof(((config_t *)0)->synthesis_model)},
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

   config_parse_memory_rewrite_section(cfg, root);

   config_apply_inference_backend_defaults(cfg, root);

   config_parse_memory_negation_section(cfg, root);

   config_parse_memory_query_expansion_section(cfg, root);

   config_parse_memory_recall_lanes_section(cfg, root);

   config_parse_memory_window_section(cfg, root);

   config_parse_kb_section(cfg, root);

   config_parse_memory_maintenance_section(cfg, root);

   config_parse_worktree_gc_section(cfg, root);
   config_parse_fold_section(cfg, root);
   config_parse_modules_section(cfg, root);
   if (config_parse_economizer_section(cfg, root) != 0)
   {
      cJSON_Delete(root);
      return -1;
   }
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

   /* context.engine: active context-compaction engine (re-homed from the
    * removed config_plugin.c; read by context_engine_set_active in server_main) */
   config_parse_context_engine(cfg, root);

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
   /* Vault custody selection (vault.custody) — P10/P7 slice 3b. Stored verbatim;
    * validated against the enum + provider-bound at kb startup (kb_vault_policy). */
   {
      cJSON *vault_cfg = cJSON_GetObjectItemCaseSensitive(root, "vault");
      if (cJSON_IsObject(vault_cfg))
      {
         cJSON *cust = cJSON_GetObjectItemCaseSensitive(vault_cfg, "custody");
         if (cJSON_IsString(cust) && cust->valuestring && cust->valuestring[0])
            snprintf(cfg->vault_custody, sizeof(cfg->vault_custody), "%s", cust->valuestring);
         /* vault.tpm2.{blob_path,tcti} — the P7-tpm2a custody provider's sealed-blob
          * path + tss2 TCTI string (read by the WITH_TPM2 build only). */
         cJSON *tpm2 = cJSON_GetObjectItemCaseSensitive(vault_cfg, "tpm2");
         if (cJSON_IsObject(tpm2))
         {
            cJSON *bp = cJSON_GetObjectItemCaseSensitive(tpm2, "blob_path");
            if (cJSON_IsString(bp) && bp->valuestring)
               snprintf(cfg->vault_tpm2_blob_path, sizeof(cfg->vault_tpm2_blob_path), "%s",
                        bp->valuestring);
            cJSON *tcti = cJSON_GetObjectItemCaseSensitive(tpm2, "tcti");
            if (cJSON_IsString(tcti) && tcti->valuestring && tcti->valuestring[0])
               snprintf(cfg->vault_tpm2_tcti, sizeof(cfg->vault_tpm2_tcti), "%s",
                        tcti->valuestring);
            cJSON *nvi = cJSON_GetObjectItemCaseSensitive(tpm2, "nv_index");
            if (cJSON_IsString(nvi) && nvi->valuestring && nvi->valuestring[0])
               snprintf(cfg->vault_tpm2_nv_index, sizeof(cfg->vault_tpm2_nv_index), "%s",
                        nvi->valuestring);
         }
      }
   }
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

/* ---- live config snapshot: reader-pinned double buffer (live-config-reload P1a) ----
 *
 * A single writer (config_reload, serialized by g_snap_wlock) publishes a fresh config_t
 * into the inactive slot of a two-slot double buffer and flips the active index; readers
 * copy the active slot under a seqlock and retry if a publish raced them. config_t is a
 * flat POD, so the copy is a plain struct assignment. Additive in P1a — NOT yet wired into
 * config_load or any push trigger (that is P1b); the infra is here + unit-tested first. */
static config_t g_snap[2];
static _Atomic unsigned g_snap_seq = 0;    /* seqlock: even = stable, odd = writing */
static _Atomic unsigned g_snap_active = 0; /* index (0/1) of the live slot */
/* One atomic admission word closes the delayed-pin TOCTOU that a separate reader
 * count plus writer zero-check would leave.  The high bit reserves a slot for
 * its writer; the remaining bits count readers that may touch its ordinary
 * config_t payload. */
#define CONFIG_SNAPSHOT_WRITER_RESERVED ((unsigned)(UINT_MAX ^ (UINT_MAX >> 1)))
#define CONFIG_SNAPSHOT_READER_MAX      (CONFIG_SNAPSHOT_WRITER_RESERVED - 1u)
static _Atomic unsigned g_snap_state[2] = {0, 0};
static uint64_t g_snap_token = 0;     /* content-hash of the active snapshot */
static _Atomic int g_snap_inited = 0; /* atomic so the config_load wrapper's read is visible */
static pthread_mutex_t g_snap_wlock = PTHREAD_MUTEX_INITIALIZER;

/* Test-only coordination seam.  The focused test links a separately compiled
 * config object with this macro; production objects contain neither the hook
 * nor the destructive state controls. */
#ifdef AIMEE_CONFIG_SNAPSHOT_TESTING
#include "config_snapshot_test.h"
static config_snapshot_test_hook_fn g_snap_test_hook;
static void *g_snap_test_hook_ctx;

static void config_snapshot_test_event(config_snapshot_test_event_t event, unsigned slot)
{
   if (g_snap_test_hook)
      g_snap_test_hook(event, slot, g_snap_test_hook_ctx);
}

void config_snapshot_test_set_hook(config_snapshot_test_hook_fn hook, void *ctx)
{
   g_snap_test_hook = hook;
   g_snap_test_hook_ctx = ctx;
}

unsigned config_snapshot_test_writer_reserved(void)
{
   return CONFIG_SNAPSHOT_WRITER_RESERVED;
}

unsigned config_snapshot_test_reader_max(void)
{
   return CONFIG_SNAPSHOT_READER_MAX;
}

int config_snapshot_test_set_slot_state(unsigned slot, unsigned state)
{
   if (slot > 1)
      return -1;
   atomic_store_explicit(&g_snap_state[slot], state, memory_order_release);
   return 0;
}

unsigned config_snapshot_test_get_slot_state(unsigned slot)
{
   return slot <= 1 ? atomic_load_explicit(&g_snap_state[slot], memory_order_acquire) : UINT_MAX;
}

unsigned config_snapshot_test_active_slot(void)
{
   return atomic_load_explicit(&g_snap_active, memory_order_acquire);
}
#else
static void config_snapshot_test_event(int event, unsigned slot)
{
   (void)event;
   (void)slot;
}
#define CONFIG_SNAPSHOT_TEST_BEFORE_RESERVE    0
#define CONFIG_SNAPSHOT_TEST_RESERVE_CONTENDED 0
#define CONFIG_SNAPSHOT_TEST_BEFORE_WRITE      0
#define CONFIG_SNAPSHOT_TEST_AFTER_OBSERVE     0
#define CONFIG_SNAPSHOT_TEST_PIN_ACQUIRED      0
#define CONFIG_SNAPSHOT_TEST_PIN_VALIDATED     0
#endif

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
   unsigned current = atomic_load_explicit(&g_snap_active, memory_order_acquire);
   unsigned nxt = current ^ 1u;
   unsigned expected = 0;
   config_snapshot_test_event(CONFIG_SNAPSHOT_TEST_BEFORE_RESERVE, nxt);
   /* Reserve exactly the drained inactive slot.  Readers admit themselves by
    * CAS on this same word, so either their pin wins (and we wait for release)
    * or this reservation wins (and no delayed reader can reach the payload). */
   unsigned contention_spins = 0;
   while (!atomic_compare_exchange_weak_explicit(&g_snap_state[nxt], &expected,
                                                 CONFIG_SNAPSHOT_WRITER_RESERVED,
                                                 memory_order_acquire, memory_order_relaxed))
   {
      config_snapshot_test_event(CONFIG_SNAPSHOT_TEST_RESERVE_CONTENDED, nxt);
      expected = 0;
      /* Readers need no writer-held resource to unpin. Bound CPU spinning and
       * yield the processor so a pinned reader can run its release-decrement. */
      atomic_signal_fence(memory_order_seq_cst);
      if (++contention_spins == 64)
      {
         sched_yield();
         contention_spins = 0;
      }
   }
   config_snapshot_test_event(CONFIG_SNAPSHOT_TEST_BEFORE_WRITE, nxt);
   unsigned s = atomic_load_explicit(&g_snap_seq, memory_order_relaxed);
   atomic_store_explicit(&g_snap_seq, s + 1, memory_order_release); /* -> odd (writing) */
   g_snap[nxt] = *cfg; /* fill the slot no reader is on */
   atomic_store_explicit(&g_snap_active, nxt, memory_order_release);
   g_snap_token = config_snapshot_token(cfg);
   atomic_store_explicit(&g_snap_seq, s + 2, memory_order_release); /* -> even (stable) */
   atomic_store_explicit(&g_snap_state[nxt], 0, memory_order_release);
   atomic_store_explicit(&g_snap_inited, 1, memory_order_release);
}

/* Seed the live snapshot from the config on disk. The daemons' entry point: they
 * were each doing config_load into a stack config_t purely to hand it straight
 * back, which is the whole 750 KB struct crossing the module boundary to travel
 * nowhere. Returns 0 on success. */

int config_snapshot_seed(void)
{
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return -1;
   int rc = config_load(cfg);
   if (rc == 0)
      config_snapshot_init(cfg);
   free(cfg);
   return rc;
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
      config_snapshot_test_event(CONFIG_SNAPSHOT_TEST_AFTER_OBSERVE, act);
      unsigned state = atomic_load_explicit(&g_snap_state[act], memory_order_acquire);
      for (;;)
      {
         if (state & CONFIG_SNAPSHOT_WRITER_RESERVED)
            break;
         if (state == CONFIG_SNAPSHOT_READER_MAX)
            return -1; /* bounded failure: output and admission word stay unchanged */
         if (atomic_compare_exchange_weak_explicit(&g_snap_state[act], &state, state + 1,
                                                   memory_order_acquire, memory_order_relaxed))
            break;
      }
      if (state & CONFIG_SNAPSHOT_WRITER_RESERVED)
         continue;
      config_snapshot_test_event(CONFIG_SNAPSHOT_TEST_PIN_ACQUIRED, act);
      unsigned s1 = atomic_load_explicit(&g_snap_seq, memory_order_acquire);
      unsigned act1 = atomic_load_explicit(&g_snap_active, memory_order_acquire);
      if (s0 != s1 || (s1 & 1u) || act != act1)
      {
         atomic_fetch_sub_explicit(&g_snap_state[act], 1, memory_order_release);
         continue;
      }
      config_snapshot_test_event(CONFIG_SNAPSHOT_TEST_PIN_VALIDATED, act);
      *out = g_snap[act]; /* reservation excludes writers until the release-unpin below */
      atomic_fetch_sub_explicit(&g_snap_state[act], 1, memory_order_release);
      return 0;
   }
}

/* Read ONE field out of the live snapshot under a reader pin, without copying
 * the whole ~750 KB struct.
 *
 * config_snapshot_get copies everything, which is what every accessor would
 * otherwise pay per call. The pin protocol here is identical to that function's
 * — seqlock validate, bounded reader admission, release-unpin — but the payload
 * is memcpy(dst, base + offset, size) instead of a whole-struct assignment.
 *
 * Returns 0 on success, -1 when no snapshot is live (caller falls back to a
 * cached load) or when reader admission is saturated. */
static int config_snapshot_read_field(size_t offset, size_t size, void *dst)
{
   if (!dst || !atomic_load_explicit(&g_snap_inited, memory_order_acquire))
      return -1;
   for (;;)
   {
      unsigned s0 = atomic_load_explicit(&g_snap_seq, memory_order_acquire);
      if (s0 & 1u)
         continue;
      unsigned act = atomic_load_explicit(&g_snap_active, memory_order_acquire);
      unsigned state = atomic_load_explicit(&g_snap_state[act], memory_order_acquire);
      for (;;)
      {
         if (state & CONFIG_SNAPSHOT_WRITER_RESERVED)
            break;
         if (state == CONFIG_SNAPSHOT_READER_MAX)
            return -1;
         if (atomic_compare_exchange_weak_explicit(&g_snap_state[act], &state, state + 1,
                                                   memory_order_acquire, memory_order_relaxed))
            break;
      }
      if (state & CONFIG_SNAPSHOT_WRITER_RESERVED)
         continue;
      unsigned s1 = atomic_load_explicit(&g_snap_seq, memory_order_acquire);
      unsigned act1 = atomic_load_explicit(&g_snap_active, memory_order_acquire);
      if (s0 != s1 || (s1 & 1u) || act != act1)
      {
         atomic_fetch_sub_explicit(&g_snap_state[act], 1, memory_order_release);
         continue;
      }
      memcpy(dst, (const char *)&g_snap[act] + offset, size);
      atomic_fetch_sub_explicit(&g_snap_state[act], 1, memory_order_release);
      return 0;
   }
}

/* Field read with a fallback: prefer the pinned snapshot; if none is live (early
 * startup, or a tool that never called config_snapshot_init) fall back to a
 * heap-loaded config so accessors work everywhere config_load worked. Heap, not
 * stack — a 750 KB frame is what overflowed the stack in the memory-search
 * path. Fails closed by leaving |dst| as the caller zeroed it. */
int config_field_read(size_t offset, size_t size, void *dst)
{
   if (config_snapshot_read_field(offset, size, dst) == 0)
      return 0;
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return -1;
   int rc = config_load(cfg);
   /* Copy even when config_load FAILED. config_load_file applies config_set_defaults
    * before it can return an error, so cfg holds each field's declared default — and the
    * default is the only honest answer for a field we could not read.
    *
    * Copying only on rc == 0 made every generated accessor return its zero seed on a load
    * failure, which silently INVERTS every default-ON dial: config_subagent_ban_enabled()
    * would report "ban disabled", turning a fail-closed guard fail-open exactly when
    * config is broken. Callers that must distinguish "read failed" from "value is 0"
    * still have rc. */
   memcpy(dst, (const char *)cfg + offset, size);
   free(cfg);
   return rc;
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
   else if (strcmp(env_name, "AIMEE_AUTONOMY_MAX_TURNS") == 0)
      snap = have ? c.autonomy_max_turns : 0;
   else if (strcmp(env_name, "AIMEE_AUTONOMY_MAX_WALL_SECS") == 0)
      snap = have ? c.autonomy_max_wall_secs : 0;
   else if (strcmp(env_name, "AIMEE_AUTONOMY_STALE_ABANDON_SECS") == 0)
      snap = have ? c.autonomy_stale_abandon_secs : 0;
   else if (strcmp(env_name, "AIMEE_AUTONOMY_CONCURRENCY") == 0)
      snap = have ? c.autonomy_concurrency : 0;
   else if (strcmp(env_name, "AIMEE_AUTONOMY_AUTO_RESUME_CAP_PARKS") == 0)
      snap = have ? c.autonomy_auto_resume_cap_parks : 0, boolish = 1;
   else if (strcmp(env_name, "AIMEE_AUTONOMY_MAX_RESUMES") == 0)
      snap = have ? c.autonomy_max_resumes : 0;
   else
      is_autonomy = 0;
   if (!is_autonomy)
      return 0; /* not a config-backed autonomy var (e.g. USD_PER_SEC) -> caller falls back */

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

/* Structured-PDF sidecar endpoints. Config first, then the deployment env var, else ""
 * (feature off). The env var is how a compose/container deployment points the KB at a
 * sidecar it just started; the config key is the operator's persistent choice, so an
 * explicitly configured command outranks it. (Deliberately the OPPOSITE precedence to
 * config_embedder_command, which documents why the running embedder's announcement must
 * win there.) Lives here so no KB caller reads the environment. */
static const char *config_sidecar_endpoint(size_t offset, size_t size, const char *env_name,
                                           char *buf, size_t buflen)
{
   buf[0] = '\0';
   config_field_read(offset, size, buf);
   buf[buflen - 1] = '\0';
   if (buf[0])
      return buf;
   const char *env = getenv(env_name);
   if (env && env[0])
   {
      snprintf(buf, buflen, "%s", env);
      return buf;
   }
   buf[0] = '\0';
   return buf;
}

const char *config_tsr_endpoint(void)
{
   static _Thread_local char buf[1024];
   return config_sidecar_endpoint(offsetof(config_t, tsr_command),
                                  sizeof(((config_t *)0)->tsr_command), "AIMEE_TSR_URL", buf,
                                  sizeof(buf));
}

const char *config_ocr_endpoint(void)
{
   static _Thread_local char buf[1024];
   return config_sidecar_endpoint(offsetof(config_t, ocr_command),
                                  sizeof(((config_t *)0)->ocr_command), "AIMEE_OCR_URL", buf,
                                  sizeof(buf));
}

int config_module_roundtable_enabled(void)
{
   /* Moved out of roundtable_activation.c, which read this env var itself and took a
    * config_t to reach the tristate. Same truthy/falsey set and the same warn-then-off
    * behavior for an unrecognized value. */
   int env_default = 0;
   const char *v = getenv("AIMEE_MODULE_ROUNDTABLE");
   if (v && v[0])
   {
      if (strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "on") == 0 ||
          strcasecmp(v, "yes") == 0)
         env_default = 1;
      else if (strcasecmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
               strcasecmp(v, "off") == 0 || strcasecmp(v, "no") == 0)
         env_default = 0;
      else
         fprintf(stderr, "aimee: invalid AIMEE_MODULE_ROUNDTABLE value; defaulting off\n");
   }
   int tristate = -1;
   config_field_read(offsetof(config_t, module_roundtable),
                     sizeof(((config_t *)0)->module_roundtable), &tristate);
   return config_module_enabled(tristate, env_default);
}

int config_antipatterns_bypass(void)
{
   /* See config.h for why this stays env-only. Value-checked and fail-closed. */
   const char *v = getenv("AIMEE_ANTIPATTERNS_BYPASS");
   if (!v || !v[0])
      return 0;
   return strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "on") == 0 ||
          strcasecmp(v, "yes") == 0;
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
   /* The published snapshot must equal what config_load() returns, and config_load
    * is config_load_file + this. Without it a reload publishes a snapshot with every
    * Vault-only secret blanked, because config_load_file may serve the process cache
    * and skip the parse that rehydrates them.
    *
    * That is not hypothetical: the server mints the API primary bearer AFTER its
    * first config_load, so the cache still holds the pre-mint config. The 1s
    * config_reload_if_changed tick then republished that stale copy over the good
    * snapshot seeded by config_snapshot_init, leaving server_api_bearer_token empty
    * and failing first-user bootstrap on every clean install (the wizard's Deploy
    * step returned 500). Applied before the token so an unchanged reload stays a
    * no-op rather than churning the snapshot. */
   config_apply_runtime_secrets(&fresh);
   uint64_t tok = config_snapshot_token(&fresh);
   if (atomic_load_explicit(&g_snap_inited, memory_order_acquire) && tok == g_snap_token)
   {
      pthread_mutex_unlock(&g_snap_wlock);
      return 0; /* self-reload no-op guard: nothing logically changed */
   }
   /* Publish BEFORE running the re-appliers: they read config through accessors,
    * which must already see the new snapshot. */
   config_snapshot_publish(&fresh);
   for (int i = 0; i < g_reapplier_count; i++)
      g_reappliers[i]();
   pthread_mutex_unlock(&g_snap_wlock);
   return 1; /* a new snapshot was published */
}

/* Live-config-reload P4: reload on an OUT-OF-BAND write to the config file — a CLI local
 * `config set` one-shot, a manual edit, or the server's own autonomous config_save (wfe/
 * trigger path) — without a restart or a SIGHUP. The server reads config from the push
 * snapshot (config_load -> config_snapshot_get), which previously only refreshed via
 * config_reload on SIGHUP or the in-server config.set route; a plain file write therefore
 * did not take effect. Call this from the server's 1s main-loop tick.
 *
 * Tracks the file's (mtime,size,inode) across calls (same cheap identity config_load_file
 * caches on; mtime alone is spoofable on the tiered appliance FS). The FIRST call reloads
 * unconditionally to reconcile the snapshot with the on-disk file (covers an edit landed in
 * the <1s window between snapshot-init and the first tick); config_reload's token no-op
 * guard makes that a cheap no-op when nothing changed. Reusing config_reload gives us its
 * validate-or-keep semantics for free, so a bad edit never replaces a good running config,
 * and the server rewriting its own equivalent config is a no-op (no reload churn).
 *
 * Returns config_reload()'s result (1 published / 0 no-op / -1 kept) when a change was
 * observed, else 0. */
/* "Is the config readable?" — the only thing callers were ever asking when they
 * wrote `config_load(&cfg) == 0` as a guard.
 *
 * config_load() does NOT fail for a missing, oversized, or unparsable file; all
 * three return 0 with defaults filled in. It fails on exactly two things: an
 * allocation failure, and strict mode rejecting a validated field. Callers that
 * branched on it were distinguishing "config unavailable" from "field is at its
 * default" — a distinction the accessors deliberately collapse, since they fall
 * back to defaults. This keeps that distinction available without handing out a
 * config_t. */
int config_present(void)
{
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return 0;
   int rc = config_load(cfg);
   free(cfg);
   return rc == 0;
}

int config_reload_if_changed(void)
{
   static struct timespec last_mt;
   static off_t last_size;
   static ino_t last_ino;
   static int seeded = 0;

   const char *path = config_default_path();
   struct stat st;
   if (stat(path, &st) != 0)
      return 0; /* transiently absent (e.g. an atomic rename in flight) — retry next tick */

   struct timespec mt = AIMEE_STAT_MTIM(st);
   int changed =
       !seeded || !timespec_eq(&mt, &last_mt) || st.st_size != last_size || st.st_ino != last_ino;
   if (!changed)
      return 0;

   last_mt = mt;
   last_size = st.st_size;
   last_ino = st.st_ino;
   seeded = 1;
   return config_reload();
}

/* ── opaque boolean accessors ────────────────────────────────────────────────
 *
 * Each of these existed only as a field read behind a caller-declared config_t.
 * A caller that wants one boolean should not have to name the type, include its
 * header, or put ~750 KB on the stack to get it. The load below is served from
 * the config module's snapshot/mtime cache, so this is not a per-call reparse.
 *
 * Heap, not stack: these are called from paths that nest several frames deep,
 * and a 750 KB frame is what overflowed the stack in the memory-search path. */
static int config_flag(size_t offset)
{
   int v = 0; /* fail closed: an unreadable config must not enable a feature */
   config_field_read(offset, sizeof(v), &v);
   return v;
}

int config_audit_worm_enabled(void)
{
   return config_flag(offsetof(config_t, audit_worm_enabled));
}

int config_bandit_live_decision_enabled(void)
{
   return config_flag(offsetof(config_t, bandit_live_decision_enabled));
}

int config_css_style_graph_enabled(void)
{
   return config_flag(offsetof(config_t, css_style_graph_enabled));
}

int config_delegate_graph_context_enabled(void)
{
   return config_flag(offsetof(config_t, delegate_graph_context_enabled));
}

int config_drift_detect_shadow_enabled(void)
{
   return config_flag(offsetof(config_t, drift_detect_shadow_enabled));
}

int config_guardrails_blast_radius_advisory_enabled(void)
{
   return config_flag(offsetof(config_t, guardrails_blast_radius_advisory_enabled));
}

int config_ingress_usage_accounting_enabled(void)
{
   return config_flag(offsetof(config_t, ingress_usage_accounting_enabled));
}

int config_kb_pdf_vector_enabled(void)
{
   return config_flag(offsetof(config_t, kb_pdf_vector_enabled));
}

int config_memory_derive_facts_enabled(void)
{
   return config_flag(offsetof(config_t, memory_derive_facts_enabled));
}

int config_memory_routing_enabled(void)
{
   return config_flag(offsetof(config_t, memory_routing_enabled));
}

int config_transport_kb_pool_enabled(void)
{
   return config_flag(offsetof(config_t, transport_kb_pool_enabled));
}

int config_typed_facts_enabled(void)
{
   return config_flag(offsetof(config_t, typed_facts_enabled));
}

int config_wfe_live_forge_enabled(void)
{
   return config_flag(offsetof(config_t, wfe_live_forge_enabled));
}

/* Non-boolean opaque accessors. Same contract as config_flag: heap-loaded from
 * the config module's cache, fail closed. */
double config_memory_semantic_floor_scale(void)
{
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return 0.0; /* caller treats <= 0 as "unset" and derives from dimension */
   config_load(cfg);
   double v = cfg->memory_semantic_floor_scale;
   free(cfg);
   return v;
}

int config_ingress_audit_async(void)
{
   return config_flag(offsetof(config_t, ingress_audit_async));
}

/* Opaque form: resolve the embedding command without the caller ever holding a
 * config_t. config_t is ~750 KB, so a caller that only wants this one string was
 * paying three quarters of a megabyte of stack for it — nested across the memory
 * search path that overflowed an 8 MB stack. The load is cached inside the
 * config module (config_load consults a snapshot/mtime cache), so this is not a
 * per-call reparse.
 *
 * Returns a pointer into a function-local static, valid until the next call on
 * this thread. Callers copy it if they need to keep it. */
/* The RAW configured embedding command, with no resolution applied.
 *
 * config_embedder_command_current() answers "what should I embed with", which
 * is never empty — it falls back to an endpoint or the builtin. Callers that
 * need "did the operator configure an embedder at all" have to see the empty
 * string, so they get the field itself. The generated accessor for this field
 * is suppressed because the resolving function already owns the name. */
const char *config_embedder_command_field(void)
{
   static _Thread_local char buf[512];
   buf[0] = 0;
   config_field_read(offsetof(config_t, embedder_command), sizeof(buf), buf);
   buf[sizeof(buf) - 1] = 0;
   return buf;
}

const char *config_embedder_command_current(const char *requested)
{
   if (requested && requested[0])
      return requested;
   static _Thread_local char cached[512];
   config_t *cfg = calloc(1, sizeof(*cfg));
   if (!cfg)
      return ""; /* allocation failure must not fabricate an embedder */
   config_load(cfg);
   snprintf(cached, sizeof(cached), "%s", config_embedder_command(cfg, NULL));
   free(cfg);
   return cached;
}
