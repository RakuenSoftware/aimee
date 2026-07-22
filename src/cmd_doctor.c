/* cmd_doctor.c: diagnostic command that checks all critical subsystems */
#define _XOPEN_SOURCE 700
#include "aimee.h"
#include "agent_config.h"
#include "commands.h"
#include "db1.h"
#include "db2/code_index.h"
#include "config_database.h"
#include "db2/memory_payload.h"
#include "db2/memory_query.h"
#include "hardware_probe.h"
#include "kb_client.h"
#include "lifecycle.h"
#include "provider_catalog.h"
#include "shutdown_forensics.h"
#include "workspace.h"
#include "cJSON.h"
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

/* Check result status */
typedef enum
{
   CHECK_OK = 0,
   CHECK_WARN,
   CHECK_ERROR
} check_status_t;

typedef struct
{
   const char *name;
   check_status_t status;
   char message[256];
   char remediation[256];
} check_result_t;

#define MAX_CHECKS 25

typedef struct
{
   int ready;
   int owned;
} doctor_db2_session_t;

/* --- Individual check functions --- */

static doctor_db2_session_t check_database(check_result_t *r, config_t *cfg)
{
   doctor_db2_session_t session = {0, 0};
   r->name = "Knowledge Store";

   /* Open shared knowledge storage once for the doctor run. Later checks reuse
    * the same connection instead of closing it between probes. */
   if (!cfg->db2_url[0])
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "shared knowledge URL not configured");
      snprintf(r->remediation, sizeof(r->remediation),
               "Set the shared knowledge connection URL in config");
      return session;
   }

   if (db2_is_initialized())
   {
      session.ready = 1;
   }
   /* §2a: resolve the dim + pin the same way cmd_core/kb_main do — via
    * config_resolve_embedding_dim (so doctor now honors AIMEE_EMBEDDING_DIM too,
    * intentionally uniform) plus the pin flag, so an unpinned doctor run derives
    * the recorded dim rather than refusing on the 1024 default. A genuine
    * pin/recorded mismatch still surfaces via #337's record_or_check guard. */
   else if ((db2_set_embedding_dim(config_resolve_embedding_dim(cfg)),
             db2_set_embedding_dim_pinned(config_embedding_dim_is_pinned(cfg)),
             db2_set_embedder_model_id(cfg->embedding_model), /* unified-llm §2 drift guard */
             db2_init(cfg->db2_url)) == 0)
   {
      session.ready = 1;
      session.owned = 1;
   }
   else
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "shared knowledge connection failed");
      snprintf(r->remediation, sizeof(r->remediation),
               "Verify the shared knowledge connection URL and service reachability");
      return session;
   }
   int schema_ok = 0;
   int have_fuzzy_extension = 0;
   if (db2_health_probe(&schema_ok, &have_fuzzy_extension) != 0)
   {
      snprintf(r->message, sizeof(r->message), "shared knowledge health probe failed");
      snprintf(r->remediation, sizeof(r->remediation), "Check knowledge service logs");
      r->status = CHECK_ERROR;
      if (session.owned)
         db2_shutdown();
      session.ready = 0;
      session.owned = 0;
      return session;
   }

   if (!schema_ok)
   {
      snprintf(r->message, sizeof(r->message), "shared knowledge schema not initialized");
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee init' after configuring storage");
      r->status = CHECK_ERROR;
      if (session.owned)
         db2_shutdown();
      session.ready = 0;
      session.owned = 0;
      return session;
   }

   if (!have_fuzzy_extension)
   {
      /* db2_init enforces pg_trgm; reaching this branch implies the
       * extension was dropped after init or db2 was opened without
       * going through db2_init. Either way, fail visibly. */
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message),
               "shared knowledge store reachable but pg_trgm is not installed");
      snprintf(r->remediation, sizeof(r->remediation),
               "Run: CREATE EXTENSION pg_trgm;  in the shared knowledge store");
      session.ready = 0;
      return session;
   }

   /* Postgres-native diagnostics: connection budget + replica role. */
   int active = -1, max_conns = -1, is_replica = -1;
   int64_t replica_lag = -1;
   (void)db2_pg_stat_summary(&active, &max_conns, &is_replica, &replica_lag);

   /* Default OK message; override for warn states below. */
   r->status = CHECK_OK;

   if (active >= 0 && max_conns > 0 && active * 100 >= max_conns * 80)
   {
      /* Connection pressure: active utilization ≥ 80% of max. */
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message),
               "shared knowledge store reachable; %d/%d connections in use (>=80%%)", active,
               max_conns);
      snprintf(r->remediation, sizeof(r->remediation),
               "Raise max_connections, add a pooler, or close idle clients");
   }
   else if (is_replica == 1 && replica_lag >= 0)
   {
      /* Standby with measurable lag: surface but don't fail the run. */
      const long long mb = (long long)(replica_lag / (1024 * 1024));
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message),
               "shared knowledge store on read replica; replication lag %lld MB (%lld bytes)", mb,
               (long long)replica_lag);
      snprintf(r->remediation, sizeof(r->remediation),
               "Reads will reflect upstream lag; writes need a primary connection URL");
   }
   else if (is_replica == 1)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "shared knowledge store on read replica");
      snprintf(r->remediation, sizeof(r->remediation),
               "Reads only; writes need a primary connection URL");
   }
   else if (active >= 0 && max_conns > 0)
   {
      snprintf(r->message, sizeof(r->message),
               "shared knowledge store reachable; %d/%d connections in use", active, max_conns);
   }
   else
   {
      snprintf(r->message, sizeof(r->message), "shared knowledge store reachable");
   }
   return session;
}

static void doctor_db2_session_close(doctor_db2_session_t *session)
{
   if (!session || !session->owned)
      return;
   db2_shutdown();
   session->ready = 0;
   session->owned = 0;
}

static void check_server(check_result_t *r)
{
   r->name = "Server";

   /* Check if aimee-server process is running */
   FILE *fp = popen("pgrep -x aimee-server 2>/dev/null", "r");
   if (!fp)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "cannot check (pgrep unavailable)");
      return;
   }

   char buf[64];
   int found = 0;
   pid_t pid = 0;
   if (fgets(buf, sizeof(buf), fp))
   {
      found = 1;
      pid = (pid_t)atoi(buf);
   }
   pclose(fp);

   if (!found)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "not running");
      snprintf(r->remediation, sizeof(r->remediation),
               "Server starts automatically on first aimee command");
      return;
   }

   /* Check uptime from /proc if available */
   char proc_path[128];
   snprintf(proc_path, sizeof(proc_path), "/proc/%d/stat", (int)pid);
   struct stat st;
   if (stat(proc_path, &st) == 0)
   {
      time_t uptime_secs = time(NULL) - st.st_mtime;
      if (uptime_secs < 60)
         snprintf(r->message, sizeof(r->message), "pid %d, uptime %lds", (int)pid,
                  (long)uptime_secs);
      else if (uptime_secs < 3600)
         snprintf(r->message, sizeof(r->message), "pid %d, uptime %ldm", (int)pid,
                  (long)(uptime_secs / 60));
      else
         snprintf(r->message, sizeof(r->message), "pid %d, uptime %ldh%ldm", (int)pid,
                  (long)(uptime_secs / 3600), (long)((uptime_secs % 3600) / 60));
   }
   else
   {
      snprintf(r->message, sizeof(r->message), "pid %d", (int)pid);
   }
   r->status = CHECK_OK;
}

static void check_config(check_result_t *r, config_t *cfg)
{
   r->name = "Config";

   const char *path = config_default_path();
   struct stat st;
   if (stat(path, &st) != 0)
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "not found");
      snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee init' to create config");
      return;
   }

   if (cfg->workspace_count == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "no workspaces configured");
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee workspace add <path>' or 'aimee setup'");
      return;
   }

   /* Verify workspace paths exist */
   int missing = 0;
   for (int i = 0; i < cfg->workspace_count; i++)
   {
      if (stat(cfg->workspaces[i], &st) != 0 || !S_ISDIR(st.st_mode))
         missing++;
   }

   if (missing > 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "%d workspace(s), %d missing", cfg->workspace_count,
               missing);
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee workspace list' and remove stale entries");
   }
   else
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "%d workspace(s)", cfg->workspace_count);
   }
}

static void check_agents(check_result_t *r)
{
   r->name = "Agents";

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "agents.json not found or invalid");
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee agent local <name> <endpoint> --model MODEL' to configure a delegate");
      return;
   }

   int enabled = 0;
   for (int i = 0; i < acfg.agent_count; i++)
   {
      if (acfg.agents[i].enabled)
         enabled++;
   }

   if (enabled == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "%d agents configured, none enabled",
               acfg.agent_count);
      snprintf(r->remediation, sizeof(r->remediation),
               "Enable an agent or run 'aimee agent local <name> <endpoint> --model MODEL'");
   }
   else
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "%d configured, %d enabled", acfg.agent_count,
               enabled);
   }
}

static void check_hardware(check_result_t *r)
{
   r->name = "Hardware";

   hardware_probe_result_t hw;
   if (hardware_probe_detect(&hw) != 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "hardware probe failed");
      snprintf(r->remediation, sizeof(r->remediation), "Check DB1 and GPU probe access");
      return;
   }
   (void)hardware_probe_cache_result(&hw);

   if (!hw.detected)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "%s",
               hw.error[0] ? hw.error : "no discrete GPU detected");
      return;
   }

   r->status = CHECK_OK;
   snprintf(r->message, sizeof(r->message), "%s %d MB %s", hw.name[0] ? hw.name : hw.vendor,
            hw.vram_mb, hw.memory_kind[0] ? hw.memory_kind : "VRAM");
}

static void check_hooks(check_result_t *r)
{
   r->name = "Hooks";

   const char *home = getenv("HOME");
   if (!home)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "HOME not set");
      return;
   }

   int found = 0;
   char path[MAX_PATH_LEN];
   struct stat st;

   /* Claude Code settings */
   snprintf(path, sizeof(path), "%s/.claude/settings.json", home);
   if (stat(path, &st) == 0)
      found++;

   /* Codex config */
   snprintf(path, sizeof(path), "%s/.codex/config.json", home);
   if (stat(path, &st) == 0)
      found++;

   if (found == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "no AI tool hooks detected");
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee init' in a project directory to register hooks");
   }
   else
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "%d tool integration(s) found", found);
   }
}

static void check_mcp(check_result_t *r, config_t *cfg)
{
   r->name = "MCP";

   int found = 0;
   struct stat st;
   char path[MAX_PATH_LEN];

   for (int i = 0; i < cfg->workspace_count; i++)
   {
      snprintf(path, sizeof(path), "%s/.mcp.json", cfg->workspaces[i]);
      if (stat(path, &st) == 0)
         found++;
   }

   if (cfg->workspace_count == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "no workspaces to check");
      snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee setup' to configure workspaces");
   }
   else if (found == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "no .mcp.json in any workspace");
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee init' in workspace directories to create .mcp.json");
   }
   else
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "%d/%d workspace(s)", found, cfg->workspace_count);
   }
}

static void check_secrets(check_result_t *r)
{
   r->name = "Secrets";

   if (db1_secret_store("__doctor_backend_probe__", "ok") != 0)
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "no secret backend available");
      snprintf(r->remediation, sizeof(r->remediation),
               "Install libsecret or check file-based secret store");
      return;
   }
   (void)db1_secret_remove("__doctor_backend_probe__");

   /* Try loading common keys to see if any are stored */
   char buf[256];
   int count = 0;
   static const char *keys[] = {"anthropic_api_key", "openai_api_key", "gemini_api_key",
                                "openrouter_api_key"};
   for (int i = 0; i < 4; i++)
   {
      if (db1_secret_load(keys[i], buf, sizeof(buf)) == 0 && buf[0])
         count++;
      memset(buf, 0, sizeof(buf));
   }

   r->status = CHECK_OK;
   snprintf(r->message, sizeof(r->message), "secret storage available, %d key(s) stored", count);
}

static void check_index(check_result_t *r, config_t *cfg, int db2_ready)
{
   r->name = "Index";
   (void)cfg;

   if (!db2_ready)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "skipped; shared knowledge unavailable");
      snprintf(r->remediation, sizeof(r->remediation), "Resolve the Knowledge Store check first");
      return;
   }

   /* The indexer populates `projects`/`files`/`terms` in shared storage. The
    * unused `symbols` table is defined in migrations, so don't query it here. */
   int project_count = db2_code_index_project_count();

   if (project_count == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "no projects indexed");
      snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee setup' to index projects");
      return;
   }

   /* Check staleness: most recent project scan timestamp */
   char ts_buf[64] = {0};
   db2_code_index_project_last_scan(ts_buf, sizeof(ts_buf));

   /* Parse timestamp and check if older than 24h */
   int stale = 0;
   if (ts_buf[0])
   {
      struct tm tm_parsed;
      memset(&tm_parsed, 0, sizeof(tm_parsed));
      if (strptime(ts_buf, "%Y-%m-%dT%H:%M:%S", &tm_parsed))
      {
         time_t indexed_time = mktime(&tm_parsed);
         time_t now = time(NULL);
         double hours = difftime(now, indexed_time) / 3600.0;
         if (hours > 24.0)
         {
            stale = 1;
            r->status = CHECK_WARN;
            snprintf(r->message, sizeof(r->message),
                     "%d project(s), stale (%.0fh since last index)", project_count, hours);
            snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee setup' to re-index");
         }
      }
   }

   if (!stale)
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "%d project(s)", project_count);
   }
}

static void check_memory(check_result_t *r, config_t *cfg, int db2_ready)
{
   r->name = "Memory";
   (void)cfg;

   if (!db2_ready)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "skipped; shared knowledge unavailable");
      snprintf(r->remediation, sizeof(r->remediation), "Resolve the Knowledge Store check first");
      return;
   }

   int total = (int)db2_memory_count();
   int l2_count = db2_memory_count_l2();
   int l3_count = db2_memory_count_l3();
   int orphaned_l0 = db2_memory_count_orphaned_l0();

   if (total == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "empty");
      snprintf(r->remediation, sizeof(r->remediation),
               "Memory populates as you use aimee in sessions");
   }
   else if (orphaned_l0 > 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "L2: %d, L3: %d, %d orphaned L0 entries", l2_count,
               l3_count, orphaned_l0);
      snprintf(r->remediation, sizeof(r->remediation),
               "Run this diagnostic with --fix to prune orphaned L0 memories");
   }
   else
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "L2: %d facts, L3: %d episodes", l2_count, l3_count);
   }
}

/* Call kb_client_health() once and pass the result to all KB sub-checks. */
static int check_kb_gather(kb_health_t *out)
{
   return kb_client_health(out);
}

static void check_kb_process(check_result_t *r, int kb_rc)
{
   r->name = "KB: process";
   if (kb_rc == 0)
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "aimee-kb responding");
   }
   else
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "aimee-kb not running");
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee kb build' to start aimee-kb; check 'aimee kb status'");
   }
}

static void check_kb_vector_store(check_result_t *r, int kb_rc, const kb_health_t *h)
{
   r->name = "KB: vector store";
   if (kb_rc != 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "skipped (aimee-kb not running)");
      return;
   }
   if (!h->pgvec_ok)
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "pgvector extension not loaded in DB2");
      snprintf(r->remediation, sizeof(r->remediation),
               "psql -d aimee_shared -c 'CREATE EXTENSION IF NOT EXISTS vector;'");
   }
   else if (!h->pgvec_collection_ok)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "vector table missing (run 'aimee kb build')");
      snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee kb build --path DIR'");
   }
   else
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "ok (%d vectors, %d indexed)", h->pgvec_vectors,
               h->pgvec_indexed);
   }
}

static void check_kb_freshness(check_result_t *r, int kb_rc, const kb_health_t *h)
{
   r->name = "KB: freshness";
   if (kb_rc != 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "skipped (aimee-kb not running)");
      return;
   }
   if (h->freshness_days < 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "never ingested");
      snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee kb ingest all'");
   }
   else if (h->freshness_days > 30)
   {
      r->status = CHECK_ERROR;
      snprintf(r->message, sizeof(r->message), "last ingest %d days ago", h->freshness_days);
      snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee kb ingest all'");
   }
   else if (h->freshness_days > 7)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "last ingest %d days ago", h->freshness_days);
      snprintf(r->remediation, sizeof(r->remediation), "Run 'aimee kb ingest all'");
   }
   else
   {
      r->status = CHECK_OK;
      if (h->freshness_days == 0)
         snprintf(r->message, sizeof(r->message), "ingested today");
      else
         snprintf(r->message, sizeof(r->message), "last ingest %d day(s) ago", h->freshness_days);
   }
}

static void check_kb_maintenance(check_result_t *r, int kb_rc, const kb_health_t *h)
{
   r->name = "KB: maintenance";
   if (kb_rc != 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "skipped (aimee-kb not running)");
      return;
   }
   if (!h->maintenance_enabled)
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message),
               "maintenance disabled (kb.maintenance.enabled = false)");
      return;
   }
   if (!h->last_maintenance_at[0])
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "maintenance enabled but has never run");
      return;
   }
   /* Warn if last run was more than 48 hours ago */
   struct tm t;
   memset(&t, 0, sizeof(t));
   if (sscanf(h->last_maintenance_at, "%d-%d-%d %d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday,
              &t.tm_hour, &t.tm_min, &t.tm_sec) >= 3)
   {
      t.tm_year -= 1900;
      t.tm_mon -= 1;
      time_t then = mktime(&t);
      time_t now = time(NULL);
      double hours_since = difftime(now, then) / 3600.0;
      if (hours_since > 48.0)
      {
         r->status = CHECK_WARN;
         snprintf(r->message, sizeof(r->message),
                  "last maintenance %.0f hours ago (threshold: 48h)", hours_since);
         return;
      }
   }
   r->status = CHECK_OK;
   snprintf(r->message, sizeof(r->message), "last run: %s (decayed=%d pruned=%d)",
            h->last_maintenance_at, h->last_maintenance_rows_decayed,
            h->last_maintenance_orphans_pruned);
}

static void check_guardrails_semantic(check_result_t *r, config_t *cfg)
{
   r->name = "guardrails.semantic";
   r->status = CHECK_OK;

   int gmode = guardrails_semantic_mode_parse(cfg->guardrails_semantic_mode);
   const char *mode_name = guardrails_semantic_mode_name(gmode);
   if (gmode == GSEM_MODE_OFF)
   {
      snprintf(r->message, sizeof(r->message), "disabled (mode=off)");
      return;
   }

   guardrail_event_counts_t counts;
   if (db1_guardrail_event_counts_7d(&counts) != 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "mode=%s; counts unavailable", mode_name);
      snprintf(r->remediation, sizeof(r->remediation),
               "Initialize DB1 or check guardrail_events table health");
      return;
   }

   if (gmode == GSEM_MODE_DRY_RUN)
   {
      snprintf(r->message, sizeof(r->message), "mode=dry_run 7d dry_run events: %d",
               counts.dry_run);
   }
   else
   {
      snprintf(r->message, sizeof(r->message), "mode=%s 7d warns: %d prompts: %d blocks: %d",
               mode_name, counts.warn, counts.prompt, counts.block);
   }
}

/* --- Fix functions --- */

static int fix_orphaned_l0(config_t *cfg)
{
   (void)cfg;
   int changes = db2_memory_prune_orphaned_l0();
   if (changes < 0)
   {
      fprintf(stderr, "  fix: L0 prune failed\n");
      return -1;
   }
   if (changes > 0)
      fprintf(stderr, "  fix: pruned %d orphaned L0 memories\n", changes);
   return 0;
}

static int fix_reindex(config_t *cfg)
{
   (void)cfg;

   /* The canonical index is owned by the knowledge service. The doctor's
    * reindex fix asks it to scan all configured workspaces; if unavailable,
    * report that and skip rather than touching local state. */
   kb_client_index_scan_result_t res;
   if (kb_client_index_scan(NULL, NULL, 0, &res) != 0)
   {
      if (strcmp(res.reason, "error") == 0 && res.message[0])
         fprintf(stderr, "  fix: %s\n", res.message);
      else
         fprintf(stderr,
                 "  fix: knowledge service unavailable — canonical index cannot be rebuilt\n");
      return -1;
   }
   if (res.skipped)
      fprintf(stderr, "  fix: scan skipped (%s)\n", res.reason[0] ? res.reason : "unknown");
   else
      fprintf(stderr, "  fix: re-indexed %d project(s), %d file(s)\n", res.projects, res.files);
   return 0;
}

static int fix_hooks(void)
{
   ensure_client_integrations();
   fprintf(stderr, "  fix: re-registered client integrations\n");
   return 0;
}

static int fix_vector_store(config_t *cfg)
{
   (void)cfg;
   fprintf(stderr, "  fix: run 'aimee kb build' to trigger a rebuild via the knowledge service\n");
   return 0;
}

static void check_provider_catalog(check_result_t *r)
{
   r->name = "Provider Catalog";

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0 || acfg.agent_count == 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "no agents configured; catalog empty");
      snprintf(r->remediation, sizeof(r->remediation),
               "Run 'aimee agent list' to check agent configuration");
      return;
   }

   provider_catalog_init(acfg.agents, acfg.agent_count);

   int down_count = 0, stale_count = 0, degraded_count = 0;
   int total = acfg.agent_count;
   char detail[192];
   size_t dpos = 0;

   for (int i = 0; i < total && i < 8; i++)
   {
      const agent_t *ag = &acfg.agents[i];
      if (!ag->enabled)
         continue;
      catalog_health_t health = provider_catalog_get_health(ag->name);
      provider_locality_t loc = provider_catalog_get_locality(ag->name);
      int conc = provider_catalog_inferred_concurrency(ag->name);

      if (health == CATALOG_HEALTH_DOWN)
         down_count++;
      else if (health == CATALOG_HEALTH_STALE)
         stale_count++;
      else if (health == CATALOG_HEALTH_DEGRADED)
         degraded_count++;

      const char *conc_label = conc > 0 ? "" : "";
      char conc_buf[16];
      if (conc > 0)
         snprintf(conc_buf, sizeof(conc_buf), " conc=%d", conc);
      else
         conc_buf[0] = '\0';

      dpos += (size_t)snprintf(detail + dpos, sizeof(detail) - dpos, "%s%s(%s/%s%s)",
                               i > 0 ? " " : "", ag->name, provider_locality_label(loc),
                               catalog_health_label(health), conc_buf);
      (void)conc_label;
      if (dpos >= sizeof(detail) - 4)
         break;
   }

   if (down_count > 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "%d/%d agents; %d down, %d stale, %d degraded — %s",
               total, total, down_count, stale_count, degraded_count, detail);
      snprintf(r->remediation, sizeof(r->remediation),
               "Check agent endpoints; run 'aimee delegate' to refresh health state");
   }
   else if (stale_count > 0 || degraded_count > 0)
   {
      r->status = CHECK_WARN;
      snprintf(r->message, sizeof(r->message), "%d/%d agents; %d stale, %d degraded — %s", total,
               total, stale_count, degraded_count, detail);
      snprintf(r->remediation, sizeof(r->remediation),
               "Run a delegate task to refresh stale provider health");
   }
   else
   {
      r->status = CHECK_OK;
      snprintf(r->message, sizeof(r->message), "%d/%d agents healthy — %s", total, total, detail);
   }
}

/* --- Status formatting helpers --- */

static const char *status_label(check_status_t s)
{
   switch (s)
   {
   case CHECK_OK:
      return "OK";
   case CHECK_WARN:
      return "WARN";
   case CHECK_ERROR:
      return "ERROR";
   }
   return "?";
}

static int gather_shutdown_forensics(check_result_t *r, shutdown_ctx_t *rows, size_t cap)
{
   r->name = "shutdown forensics";
   (void)shutdown_forensics_record_unclean_exits();
   int count = shutdown_forensics_list_recent(rows, cap);
   r->status = CHECK_OK;
   if (count > 0)
      snprintf(r->message, sizeof(r->message), "%d recent shutdown record(s)", count);
   else
      snprintf(r->message, sizeof(r->message), "no recent shutdown records");
   return count;
}

static void add_forensics_json(cJSON *root, const shutdown_ctx_t *rows, int count)
{
   cJSON *arr = cJSON_AddArrayToObject(root, "recent_shutdowns");
   for (int i = 0; i < count; i++)
   {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddNumberToObject(item, "at", (double)rows[i].at);
      cJSON_AddStringToObject(item, "daemon", rows[i].daemon);
      cJSON_AddNumberToObject(item, "daemon_pid", rows[i].daemon_pid);
      cJSON_AddStringToObject(item, "signal", rows[i].signal_name);
      cJSON_AddNumberToObject(item, "sender_pid", rows[i].sender_pid);
      cJSON_AddStringToObject(item, "sender_comm", rows[i].sender_comm);
      cJSON_AddNumberToObject(item, "uptime_s", rows[i].uptime_s);
      cJSON_AddNumberToObject(item, "inflight_turns", rows[i].inflight_turns);
      cJSON_AddNumberToObject(item, "inflight_jobs", rows[i].inflight_jobs);
      cJSON_AddNumberToObject(item, "inflight_workers", rows[i].inflight_workers);
      cJSON_AddNumberToObject(item, "rss_kb", rows[i].rss_kb);
      cJSON_AddBoolToObject(item, "unclean_exit", rows[i].unclean_exit ? 1 : 0);
      cJSON_AddStringToObject(item, "process_tree", rows[i].process_tree);
      cJSON_AddItemToArray(arr, item);
   }
}

static void print_forensics_text(FILE *out, const shutdown_ctx_t *rows, int count)
{
   fprintf(out, "Recent shutdowns:\n");
   if (count <= 0)
   {
      fprintf(out, "  none\n");
      return;
   }
   for (int i = 0; i < count; i++)
   {
      char line[256];
      shutdown_forensics_format_summary(&rows[i], line, sizeof(line));
      fprintf(out, "  %s\n", line);
   }
}

/* --- Public JSON runner (used by dashboard/webchat API) --- */

char *doctor_checks_json(void)
{
   config_t cfg;
   config_load(&cfg);

   check_result_t checks[MAX_CHECKS];
   memset(checks, 0, sizeof(checks));

   int n = 0;
   doctor_db2_session_t db2_session = check_database(&checks[n++], &cfg);
   check_server(&checks[n++]);
   check_config(&checks[n++], &cfg);
   check_agents(&checks[n++]);
   check_hardware(&checks[n++]);
   check_provider_catalog(&checks[n++]);
   check_hooks(&checks[n++]);
   check_mcp(&checks[n++], &cfg);
   check_secrets(&checks[n++]);
   check_index(&checks[n++], &cfg, db2_session.ready);
   check_memory(&checks[n++], &cfg, db2_session.ready);
   kb_health_t kb_health;
   int kb_rc = check_kb_gather(&kb_health);
   check_kb_process(&checks[n++], kb_rc);
   check_kb_vector_store(&checks[n++], kb_rc, &kb_health);
   check_kb_freshness(&checks[n++], kb_rc, &kb_health);
   check_kb_maintenance(&checks[n++], kb_rc, &kb_health);
   check_guardrails_semantic(&checks[n++], &cfg);
   shutdown_ctx_t forensics[5];
   int forensic_count = gather_shutdown_forensics(&checks[n++], forensics, 5);

   int warnings = 0, errors = 0;
   for (int i = 0; i < n; i++)
   {
      if (checks[i].status == CHECK_WARN)
         warnings++;
      else if (checks[i].status == CHECK_ERROR)
         errors++;
   }

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "version", AIMEE_VERSION);
   cJSON *arr = cJSON_AddArrayToObject(root, "checks");
   for (int i = 0; i < n; i++)
   {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", checks[i].name);
      cJSON_AddStringToObject(item, "status", status_label(checks[i].status));
      cJSON_AddStringToObject(item, "message", checks[i].message);
      if (checks[i].remediation[0])
         cJSON_AddStringToObject(item, "remediation", checks[i].remediation);
      cJSON_AddItemToArray(arr, item);
   }
   cJSON_AddNumberToObject(root, "warnings", warnings);
   cJSON_AddNumberToObject(root, "errors", errors);
   add_forensics_json(root, forensics, forensic_count);

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   doctor_db2_session_close(&db2_session);
   return json ? json : strdup("{}");
}

/* --- Main command handler --- */

void cmd_doctor(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   int do_fix = 0;
   const char *subcheck = NULL;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--fix") == 0)
         do_fix = 1;
      else if (argv[i][0] != '-' && !subcheck)
         subcheck = argv[i];
   }

   config_t cfg;
   config_load(&cfg);

   /* `aimee doctor storage` runs just the Knowledge Store check. The legacy
    * `db` spelling remains accepted for compatibility. Exit codes
    * mirror the full-suite semantics so CI can gate on a single
    * check. */
   if (subcheck && (strcmp(subcheck, "storage") == 0 || strcmp(subcheck, "db") == 0))
   {
      check_result_t r;
      memset(&r, 0, sizeof(r));
      doctor_db2_session_t db2_session = check_database(&r, &cfg);
      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "name", r.name);
         cJSON_AddStringToObject(obj, "status", status_label(r.status));
         cJSON_AddStringToObject(obj, "message", r.message);
         if (r.remediation[0])
            cJSON_AddStringToObject(obj, "remediation", r.remediation);
         cJSON_AddStringToObject(obj, "backend", "knowledge-service");
         emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         fprintf(stderr, "aimee doctor storage — backend=knowledge-service\n");
         fprintf(stderr, "  %s .......... %-5s", r.name, status_label(r.status));
         if (r.message[0])
            fprintf(stderr, " (%s)", r.message);
         fprintf(stderr, "\n");
         if (r.remediation[0])
            fprintf(stderr, "  remediation: %s\n", r.remediation);
      }
      doctor_db2_session_close(&db2_session);
      if (r.status == CHECK_ERROR)
         exit(2);
      if (r.status == CHECK_WARN)
         exit(1);
      return;
   }

   if (subcheck && strcmp(subcheck, "forensics") == 0)
   {
      shutdown_ctx_t rows[50];
      (void)shutdown_forensics_record_unclean_exits();
      int count = shutdown_forensics_list_recent(rows, 50);
      if (ctx->json_output)
      {
         cJSON *root = cJSON_CreateObject();
         add_forensics_json(root, rows, count);
         emit_json_ctx(root, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         print_forensics_text(stderr, rows, count);
      }
      return;
   }

   check_result_t checks[MAX_CHECKS];
   memset(checks, 0, sizeof(checks));

   int n = 0;
   doctor_db2_session_t db2_session = check_database(&checks[n++], &cfg);
   check_server(&checks[n++]);
   check_config(&checks[n++], &cfg);
   check_agents(&checks[n++]);
   check_hardware(&checks[n++]);
   check_provider_catalog(&checks[n++]);
   check_hooks(&checks[n++]);
   check_mcp(&checks[n++], &cfg);
   check_secrets(&checks[n++]);
   check_index(&checks[n++], &cfg, db2_session.ready);
   check_memory(&checks[n++], &cfg, db2_session.ready);
   kb_health_t kb_health;
   int kb_rc = check_kb_gather(&kb_health);
   check_kb_process(&checks[n++], kb_rc);
   check_kb_vector_store(&checks[n++], kb_rc, &kb_health);
   check_kb_freshness(&checks[n++], kb_rc, &kb_health);
   check_kb_maintenance(&checks[n++], kb_rc, &kb_health);
   check_guardrails_semantic(&checks[n++], &cfg);
   shutdown_ctx_t forensics[5];
   int forensic_count = gather_shutdown_forensics(&checks[n++], forensics, 5);

   int warnings = 0, errors = 0;
   for (int i = 0; i < n; i++)
   {
      if (checks[i].status == CHECK_WARN)
         warnings++;
      else if (checks[i].status == CHECK_ERROR)
         errors++;
   }

   if (ctx->json_output)
   {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddStringToObject(root, "version", AIMEE_VERSION);
      cJSON *arr = cJSON_AddArrayToObject(root, "checks");

      for (int i = 0; i < n; i++)
      {
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "name", checks[i].name);
         cJSON_AddStringToObject(item, "status", status_label(checks[i].status));
         cJSON_AddStringToObject(item, "message", checks[i].message);
         if (checks[i].remediation[0])
            cJSON_AddStringToObject(item, "remediation", checks[i].remediation);
         cJSON_AddItemToArray(arr, item);
      }

      cJSON_AddNumberToObject(root, "warnings", warnings);
      cJSON_AddNumberToObject(root, "errors", errors);
      add_forensics_json(root, forensics, forensic_count);

      emit_json_ctx(root, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      fprintf(stderr, "aimee doctor v%s\n\n", AIMEE_VERSION);

      for (int i = 0; i < n; i++)
      {
         int name_len = (int)strlen(checks[i].name);
         int dots = 20 - name_len;
         if (dots < 2)
            dots = 2;

         fprintf(stderr, "  %s ", checks[i].name);
         for (int d = 0; d < dots; d++)
            fputc('.', stderr);
         fprintf(stderr, " %-5s", status_label(checks[i].status));
         if (checks[i].message[0])
            fprintf(stderr, " (%s)", checks[i].message);
         fprintf(stderr, "\n");
      }

      fprintf(stderr, "\n%d warning(s), %d error(s)\n", warnings, errors);
      if (forensic_count > 0)
      {
         fprintf(stderr, "\n");
         print_forensics_text(stderr, forensics, forensic_count);
      }

      if ((warnings > 0 || errors > 0) && !do_fix)
         fprintf(stderr, "Run with --fix to auto-repair detected issues.\n");
   }

   if (do_fix)
   {
      fprintf(stderr, "\nApplying fixes...\n");
      if (db2_session.ready)
         fix_orphaned_l0(&cfg);
      else
         fprintf(stderr, "  fix: skipped L0 prune; shared knowledge unavailable\n");
      fix_reindex(&cfg);
      fix_hooks();
      fix_vector_store(&cfg);
      fprintf(stderr, "Done.\n");
   }

   /* Exit code: 0 = all pass, 1 = warnings, 2 = errors */
   doctor_db2_session_close(&db2_session);
   if (errors > 0)
      exit(2);
   else if (warnings > 0)
      exit(1);
}
