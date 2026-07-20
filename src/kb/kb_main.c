#include "aimee.h"
#include "agent_exec.h"
#include "config.h"
#include "config_database.h"
#include "css_render_cmd.h"
#include "db2/code_index.h"
#include "kb_auth_oidc.h"
#include "kb_oidc_jwks_fleet.h"
#include "kb_identity.h"
#include "db2_tenant.h"
#include "team.h"
#include "membership.h"
#include "kb_insights_util.h"
#include "org_budget.h"
#include "org_rate.h"
#include "org_model_catalog.h"
#include "org_spend.h"
#include "project.h"
#include "kb_enroll.h"
#include "kb_http.h"
#include "kb_tls.h"
#include "kb_paths.h"
#include "kb_service.h"
#include "log.h"
#include "lifecycle.h"
#include "embedder_probe.h"
#include "shutdown_forensics.h"
#include "util.h"
#include "cJSON.h"
#include "memory.h"
#include "memory_graph_fusion.h"
#include "db2/memory_vectors.h"
#include "db2/rel_types_store.h" /* db2_rel_types_ensure_seed (typed-fact ontology) */
#include "db2/vault_pg.h"    /* vault_pg_backend + vault_store_set_backend (kb vault bind) */
#include "kb/kb_vault_policy.h" /* kb_vault_policy_select (custody selection, P7 §3) */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#endif

static kb_service_ctx_t g_ctx;

#define AIMEE_DB2_BOOTSTRAP_DB  "aimee_shared"
#define AIMEE_DB2_BOOTSTRAP_URL "postgres:///aimee_shared"

#ifndef _WIN32
static void kb_signal_handler_info(int sig, siginfo_t *info, void *ucontext)
{
   (void)ucontext;
   (void)shutdown_forensics_record_signal("kb", sig, info, (time_t)g_ctx.start_time, 0, 0,
                                          g_ctx.worker_count);
   g_ctx.running = 0;
}

static void kb_install_signal_handlers(void)
{
   struct sigaction sa;
   memset(&sa, 0, sizeof(sa));
   sa.sa_sigaction = kb_signal_handler_info;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_SIGINFO;
   sigaction(SIGINT, &sa, NULL);
   sigaction(SIGTERM, &sa, NULL);
#ifdef SIGHUP
   sigaction(SIGHUP, &sa, NULL);
#endif
   /* Ignore SIGPIPE: long-lived /v1 WebSocket streams write to client sockets
    * that may have gone away; a write to a closed peer must yield EPIPE, not
    * kill the process. (The request/response REST path never hit this because
    * the client reads the whole response before closing.) */
   signal(SIGPIPE, SIG_IGN);
}
#else
static void kb_signal_handler(int sig)
{
   (void)sig;
   g_ctx.running = 0;
}

static void kb_install_signal_handlers(void)
{
   signal(SIGINT, kb_signal_handler);
   signal(SIGTERM, kb_signal_handler);
}
#endif

static void bootstrap_add_step(cJSON *steps, const char *step, int rc, const char *output)
{
   if (!steps)
      return;
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return;
   cJSON_AddStringToObject(obj, "step", step ? step : "");
   cJSON_AddNumberToObject(obj, "exit_code", rc);
   if (output && output[0])
   {
      char snippet[512];
      snprintf(snippet, sizeof(snippet), "%s", output);
      cJSON_AddStringToObject(obj, "output", snippet);
   }
   cJSON_AddItemToArray(steps, obj);
}

static int bootstrap_run_cmd(cJSON *steps, const char *step, const char *cmd)
{
   int rc = -1;
   /* Run with stdin from /dev/null: createdb/psql/sudo must never block waiting
    * on a tty prompt. A blocked child orphans, and the setuid-root `sudo` steps
    * cannot be reaped by this non-root process, so they accumulate. */
   char guarded[1152];
   snprintf(guarded, sizeof(guarded), "%s </dev/null", cmd);
   char *out = run_cmd(guarded, &rc);
   bootstrap_add_step(steps, step, rc, out);
   free(out);
   return rc;
}

/* Single-flight + cooldown guard for the local-tools DB2 bootstrap (the sudo
 * createdb/createuser/psql steps). Those steps connect to Postgres and can
 * block on catalog locks; without a guard, every kb autostart re-issues them
 * and they pile up as orphaned, un-killable setuid-root `sudo` children
 * (observed: 4000+ stuck `sudo -n -u postgres createdb` processes exhausting PG
 * connection slots). Returns a held lock fd (>=0; caller releases it via
 * bootstrap_local_tools_end) to proceed; -1 to skip because another attempt
 * holds the lock or one ran within the cooldown window; -2 on guard-infra
 * failure (proceed once, unguarded) so a missing config dir never permanently
 * blocks provisioning. */
#define DB2_BOOTSTRAP_COOLDOWN_SECS 300

/* Shell preamble that bounds each provisioning step with coreutils `timeout`
 * when available. The single-flight guard caps concurrent attempts to one, but
 * a `createdb`/`psql` can still block server-side on a catalog lock; without a
 * bound that one attempt holds the lock indefinitely (and, pre-guard, piled up).
 * Sets $TMO; place "$TMO " immediately before the binary so `timeout` is its
 * direct parent (for sudo steps, sudo relays the signal to the child). */
#define DB2_BOOTSTRAP_TMO "TMO=$(command -v timeout >/dev/null 2>&1 && echo 'timeout -k 5 30'); "
#ifndef _WIN32
static int bootstrap_local_tools_begin(void)
{
   /* The lock must be HOST-GLOBAL per user, not per-AIMEE_HOME: the local
    * tools provision the same shared Postgres database (aimee_shared) on the
    * host regardless of which config dir the process runs under. Keying the
    * lock to config_default_dir() let processes with different homes — notably
    * the many short-lived aimee-kb instances tests spin up under /tmp temp
    * homes — each take their own lock and hammer the same DB concurrently, the
    * exact runaway this guard exists to prevent. Key it to the uid + target DB
    * in a host-global temp dir so every aimee process for this user serializes. */
   const char *tmp = getenv("TMPDIR");
   if (!tmp || !tmp[0])
      tmp = "/tmp";
   char path[1024];
   snprintf(path, sizeof(path), "%s/aimee-db2-bootstrap-%u-%s.lock", tmp, (unsigned)getuid(),
            AIMEE_DB2_BOOTSTRAP_DB);
   int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
   if (fd < 0)
      return -2;
   if (flock(fd, LOCK_EX | LOCK_NB) != 0)
   {
      close(fd); /* another bootstrap is in flight */
      return -1;
   }
   struct stat st;
   time_t now = time(NULL);
   if (fstat(fd, &st) == 0 && st.st_mtime > 0 && now - st.st_mtime < DB2_BOOTSTRAP_COOLDOWN_SECS)
   {
      flock(fd, LOCK_UN); /* attempted within the cooldown window — skip */
      close(fd);
      return -1;
   }
   (void)futimens(fd, NULL); /* stamp the attempt time; keep the lock held */
   return fd;
}
static void bootstrap_local_tools_end(int lockfd)
{
   if (lockfd >= 0)
   {
      flock(lockfd, LOCK_UN);
      close(lockfd);
   }
}
#else
static int bootstrap_local_tools_begin(void)
{
   return -2;
}
static void bootstrap_local_tools_end(int lockfd)
{
   (void)lockfd;
}
#endif

static int bootstrap_db2_try_url(config_t *cfg, const char *url, int save_config, cJSON *resp)
{
   if (!url || !url[0])
      return -1;

   if (db2_init(url) != 0)
      return -1;

   int schema_ok = 0;
   int have_pg_trgm = 0;
   int ok = (db2_health_probe(&schema_ok, &have_pg_trgm) == 0 && schema_ok && have_pg_trgm);
   db2_shutdown();
   if (!ok)
      return -1;

   if (save_config)
   {
      snprintf(cfg->db2_url, sizeof(cfg->db2_url), "%s", url);
      if (config_save(cfg) != 0)
      {
         /* DB2 is already proven healthy (db2_init + health probe above). The
          * config persist is only a fast-path cache for later starts — the URL
          * is re-resolved from AIMEE_DB2_URL / db2_url every boot regardless — so
          * a failed save (e.g. a read-only aimee.yaml, as the remote-writes
          * compose override bind-mounts it :ro) must NOT abort an otherwise
          * healthy kb. Record that the save was skipped and continue. */
         fprintf(stderr, "aimee-kb: warning: could not persist db2_url to config "
                         "(continuing; DB2 is reachable via the resolved URL)\n");
         save_config = 0;
      }
   }

   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "knowledge_ready", 1);
   cJSON_AddStringToObject(resp, "db2_url", url);
   cJSON_AddBoolToObject(resp, "config_saved", save_config ? 1 : 0);
   return 0;
}

static int bootstrap_db2_with_local_tools(cJSON *steps)
{
   int lockfd = bootstrap_local_tools_begin();
   if (lockfd == -1)
   {
      bootstrap_add_step(
          steps, "local_tools_guard", 0,
          "skipped: a DB2 bootstrap is in progress or ran within the cooldown window");
      return -1;
   }

   char *db = shell_escape(AIMEE_DB2_BOOTSTRAP_DB);
   const char *user_env = getenv("USER");
   if (!user_env || !user_env[0])
      user_env = getenv("USERNAME");
   if (!user_env || !user_env[0])
      user_env = "aimee";
   char *user = shell_escape(user_env);

   char cmd[1024];

   snprintf(cmd, sizeof(cmd), DB2_BOOTSTRAP_TMO "$TMO createdb '%s' 2>&1", db);
   (void)bootstrap_run_cmd(steps, "createdb", cmd);

   snprintf(
       cmd, sizeof(cmd),
       DB2_BOOTSTRAP_TMO
       "$TMO psql -d '%s' -v ON_ERROR_STOP=1 -c 'CREATE EXTENSION IF NOT EXISTS pg_trgm;' 2>&1",
       db);
   int rc = bootstrap_run_cmd(steps, "create_extension", cmd);
   if (rc == 0)
   {
      free(db);
      free(user);
      bootstrap_local_tools_end(lockfd);
      return 0;
   }

   snprintf(cmd, sizeof(cmd),
            DB2_BOOTSTRAP_TMO "command -v sudo >/dev/null 2>&1 && "
                              "$TMO sudo -n -u postgres createuser --createdb '%s' 2>/dev/null "
                              "|| true",
            user);
   (void)bootstrap_run_cmd(steps, "sudo_create_role", cmd);

   snprintf(cmd, sizeof(cmd),
            DB2_BOOTSTRAP_TMO "command -v sudo >/dev/null 2>&1 && "
                              "$TMO sudo -n -u postgres createdb -O '%s' '%s' 2>&1",
            user, db);
   (void)bootstrap_run_cmd(steps, "sudo_createdb", cmd);

   snprintf(cmd, sizeof(cmd),
            DB2_BOOTSTRAP_TMO "command -v sudo >/dev/null 2>&1 && "
                              "$TMO sudo -n -u postgres psql -d '%s' -v ON_ERROR_STOP=1 "
                              "-c 'CREATE EXTENSION IF NOT EXISTS pg_trgm;' 2>&1",
            db);
   rc = bootstrap_run_cmd(steps, "sudo_create_extension", cmd);

   free(db);
   free(user);
   bootstrap_local_tools_end(lockfd);
   return rc == 0 ? 0 : -1;
}

/* Resolve and bootstrap DB2 for `cfg`. Mutates cfg.db2_url to the URL that
 * succeeded and persists it via config_save. `resp` collects step-level
 * details (used by the init RPC; pass a throwaway object when calling from
 * startup). Returns 0 on success, 1 on failure. */
static int kb_bootstrap_db2_resolve(config_t *cfg, cJSON *resp)
{
   cJSON *steps = cJSON_AddArrayToObject(resp, "steps");

   /* AIMEE_DB2_URL, when set, is the source of truth and overrides any db2_url
    * cached in aimee.yaml from a previous boot. In a container deploy the
    * runtime injects the current Postgres address every start; a service it
    * depends on can be recreated onto a NEW bridge IP, so a db2_url persisted on
    * an earlier boot goes stale. Preferring the cached value (as before) made
    * the kb connect to the old/wrong address forever — even though the correct
    * URL was right there in the environment. The successful bootstrap below
    * re-persists this URL, refreshing the cache. The cached value is used only
    * as a fallback when AIMEE_DB2_URL is unset (manual / non-container setups). */
   config_apply_db2_url_env_override(cfg);

   if (cfg->db2_url[0])
   {
      /* Pass a stable copy: bootstrap_db2_try_url writes the winning URL back
       * into cfg->db2_url via snprintf, and snprintf'ing a buffer onto itself
       * (src == dst) is undefined — on glibc it truncates the destination to
       * empty. That left cfg->db2_url blank, so the real db2_init(cfg->db2_url)
       * below failed with an empty URL. Only triggered when db2_url came from
       * AIMEE_DB2_URL with no configured value (e.g. the container deploy). */
      char url[sizeof(cfg->db2_url)];
      snprintf(url, sizeof(url), "%s", cfg->db2_url);
      if (bootstrap_db2_try_url(cfg, url, 1, resp) == 0)
         return 0;
   }

   if (!cfg->db2_url[0] && bootstrap_db2_try_url(cfg, AIMEE_DB2_BOOTSTRAP_URL, 1, resp) == 0)
      return 0;

   if (!cfg->db2_url[0])
   {
      (void)bootstrap_db2_with_local_tools(steps);
      if (bootstrap_db2_try_url(cfg, AIMEE_DB2_BOOTSTRAP_URL, 1, resp) == 0)
         return 0;
   }

   cJSON_AddStringToObject(resp, "status", "error");
   cJSON_AddBoolToObject(resp, "knowledge_ready", 0);
   cJSON_AddStringToObject(
       resp, "message",
       "DB2 bootstrap failed; install/start Postgres or set AIMEE_DB2_URL or db2_url");
   cJSON_AddStringToObject(
       resp, "remediation",
       "Install PostgreSQL, start the service, then run: createdb " AIMEE_DB2_BOOTSTRAP_DB
       " && psql -d " AIMEE_DB2_BOOTSTRAP_DB " -c 'CREATE EXTENSION IF NOT EXISTS pg_trgm;'");
   return 1;
}

static int kb_bootstrap_db2(int json_output)
{
   config_t cfg;
   config_load(&cfg);
   cJSON *resp = cJSON_CreateObject();

   (void)kb_bootstrap_db2_resolve(&cfg, resp);

   int ok = 0;
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0)
      ok = 1;

   char *out = json_output ? cJSON_PrintUnformatted(resp) : cJSON_Print(resp);
   if (out)
   {
      puts(out);
      free(out);
   }
   cJSON_Delete(resp);
   return ok ? 0 : 1;
}

/* `aimee-kb enroll --host H --port N [--scope S]` — mint a one-time enrollment:
 * persist/reuse the internal CA, issue a single-use token, and print the
 * `aimee://` connection string an operator hands to a client. The CA + token
 * store live under the kb config dir, so the running server can later redeem the
 * token and existing enrollments survive a restart. */
static int kb_cmd_enroll(int argc, char **argv)
{
   const char *host = NULL;
   const char *scope = "global";
   int port = 0;
   for (int i = 2; i < argc; i++)
   {
      if (strncmp(argv[i], "--host=", 7) == 0)
         host = argv[i] + 7;
      else if (strncmp(argv[i], "--port=", 7) == 0)
         port = atoi(argv[i] + 7);
      else if (strncmp(argv[i], "--scope=", 8) == 0)
         scope = argv[i] + 8;
      else
      {
         fprintf(stderr, "aimee-kb enroll: unknown argument: %s\n", argv[i]);
         return 1;
      }
   }
   if (!host || !host[0] || port <= 0 || port > 65535)
   {
      fprintf(stderr, "Usage: aimee-kb enroll --host=HOST --port=N [--scope=SCOPE]\n"
                      "  Mints a single-use enrollment token and prints the aimee:// "
                      "connection string\n"
                      "  for a client. HOST/PORT are the kb's externally reachable "
                      "address. SCOPE defaults to 'global'.\n");
      return 1;
   }

   char conn[1024];
   if (kb_enroll_mint(kb_default_config_dir(), host, port, scope, conn, sizeof(conn)) != 0)
   {
      fprintf(stderr, "aimee-kb enroll: failed to mint enrollment (check CA / token store under "
                      "the kb config dir)\n");
      return 1;
   }
   puts(conn);
   return 0;
}

/* --fusion-probe=<query>: a DB2-linked diagnostic that runs the same recall
 * query twice — graph_code_fusion_state off then on — against the live store and
 * prints both result sets, flagging entries the fusion expansion newly surfaces
 * through the code graph. The thin CLI forwards every `memory` subcommand to the
 * server (no route), so this is the only way to exercise memory_find_facts +
 * the fusion rerank against a populated DB2 without a session. Runs after
 * db2_init and exits; does not start the service. */
static int kb_run_fusion_probe(const char *query)
{
   config_t cfg;
   config_load(&cfg);

   /* memory_find_facts takes the lexical-fallback path (which skips the fusion
    * block) unless the pgvector memory collection exists, so ensure it. */
   if (pgvec_memory_vector_collection_exists() <= 0)
   {
      int dim = cfg.embedding_dim > 0 ? cfg.embedding_dim : 1024;
      (void)pgvec_memory_vector_collection_recreate(dim);
   }

   memory_t off[20];
   memory_t on[20];
   memory_fusion_state_clear();
   int n_off = memory_find_facts(query, 20, off, 20);
   memory_fusion_state_set("on");
   int n_on = memory_find_facts(query, 20, on, 20);
   memory_fusion_state_clear();

   printf("=== fusion probe: \"%s\" ===\n", query);
   printf("vector_ready=%d\n", pgvec_memory_vector_collection_exists() > 0 ? 1 : 0);
   printf("--- fusion OFF (%d results) ---\n", n_off < 0 ? 0 : n_off);
   for (int i = 0; i < n_off; i++)
      printf("  #%-2d id=%-8lld %s\n", i + 1, (long long)off[i].id, off[i].key);
   printf("--- fusion ON  (%d results) ---\n", n_on < 0 ? 0 : n_on);
   int newly = 0;
   for (int i = 0; i < n_on; i++)
   {
      int in_off = 0;
      for (int j = 0; j < n_off; j++)
         if (on[i].id == off[j].id)
         {
            in_off = 1;
            break;
         }
      if (!in_off)
         newly++;
      printf("  #%-2d id=%-8lld %s%s\n", i + 1, (long long)on[i].id, on[i].key,
             in_off ? "" : "   <-- graph-bridged (new under fusion)");
   }
   printf("=== fusion surfaced %d memories not in the baseline result set ===\n", newly);
   return 0;
}

/* Operator-facing tenancy CLI on the kb host (P1 slice 4):
 *   aimee-kb team create <name>
 *   aimee-kb team list
 *   aimee-kb team add-member <team_id> <identity_key> [--default]
 *   aimee-kb team remove-member <team_id> <identity_key>
 *   aimee-kb project create <team_id> <name> [team-open|restricted]
 *   aimee-kb project list [team_id]
 * Runs in-process against DB2 as the 'owner' (bootstrap) principal, so an operator
 * with the kb DB credential can manage tenancy without a running listener. (The
 * remote thin-client `aimee team` needs human-actor forwarding to kb — P5.) */
static int kb_cmd_tenancy_init_db2(void)
{
   config_t cfg;
   config_load(&cfg);
   config_apply_db2_url_env_override(&cfg);
   if (!cfg.db2_url[0])
   {
      fprintf(stderr, "aimee-kb: db2_url not configured (set AIMEE_DB2_URL or run `aimee init`)\n");
      return -1;
   }
   db2_set_embedding_dim(cfg.embedding_dim > 0 ? cfg.embedding_dim : 1024);
   if (db2_init(cfg.db2_url) != 0)
   {
      fprintf(stderr, "aimee-kb: DB2 not reachable at %s\n", cfg.db2_url);
      return -1;
   }
   return 0;
}

/* Operator-facing spend reporting CLI (P3b):
 *   aimee-kb spend --team X [--project Y] [--since YYYY-MM-DD] [--until YYYY-MM-DD] [--json]
 * Runs in-process against DB2 as the install owner principal (an org-admin, so the
 * SECURITY DEFINER org_spend_query()'s admin gate passes and --team may be omitted for
 * the org-wide report). Read-only. cost_usd is a NUMERIC string, never a float. */
static int kb_cmd_spend(int argc, char **argv)
{
   int has_team = 0, has_project = 0, want_json = 0;
   int64_t team = 0, project = 0;
   const char *since = NULL, *until = NULL;
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--team") == 0 && i + 1 < argc)
      {
         team = strtoll(argv[++i], NULL, 10);
         has_team = 1;
      }
      else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc)
      {
         project = strtoll(argv[++i], NULL, 10);
         has_project = 1;
      }
      else if (strcmp(argv[i], "--since") == 0 && i + 1 < argc)
         since = argv[++i];
      else if (strcmp(argv[i], "--until") == 0 && i + 1 < argc)
         until = argv[++i];
      else if (strcmp(argv[i], "--json") == 0)
         want_json = 1;
   }
   /* Default to a wide bounded window when unspecified (finance callers usually pass a
    * range; the reporting surface stays usable without one). */
   if (!since)
      since = "0001-01-01";
   if (!until)
      until = "9999-12-31";
   if (!kb_insights_date_valid(since) || !kb_insights_date_valid(until))
   {
      fprintf(stderr, "aimee-kb: --since/--until must be valid YYYY-MM-DD dates\n");
      return 1;
   }
   if (strcmp(since, until) > 0)
   {
      fprintf(stderr, "aimee-kb: --since must be <= --until\n");
      return 1;
   }

   if (kb_cmd_tenancy_init_db2() != 0)
      return 1;
   kb_principal_t owner;
   kb_verify_result_t ovr;
   memset(&ovr, 0, sizeof(ovr));
   if (kb_principal_from_verify(&ovr, "", &owner) != 0)
   {
      db2_shutdown();
      return 1;
   }
   if (db2_tenant_scope_begin(&owner, 0) != 0)
   {
      fprintf(stderr, "aimee-kb: tenant scope failed (is this a hardened tier? run migrations)\n");
      db2_shutdown();
      return 1;
   }

   db2_org_spend_row_t rows[DB2_SPEND_MAX_ROWS];
   int n = db2_org_spend_query(has_team, team, has_project, project, since, until, rows,
                               (int)(sizeof(rows) / sizeof(rows[0])));
   if (n < 0)
      db2_tenant_scope_rollback(); /* the definer RAISEd (or a client-side TOOBIG) */
   else
      db2_tenant_scope_commit();

   int rc = 0;
   if (n < 0)
   {
      if (n == DB2_SPEND_ERR_DENIED)
         fprintf(stderr, "aimee-kb: not authorized (org-admin or team-lead required)\n");
      else if (n == DB2_SPEND_ERR_BADDATE)
         fprintf(stderr, "aimee-kb: invalid date range\n");
      else if (n == DB2_SPEND_ERR_TOOBIG)
         fprintf(stderr, "aimee-kb: report too large (>%d rows); narrow --team/--project/"
                         "--since/--until\n",
                 DB2_SPEND_MAX_ROWS);
      else
         fprintf(stderr, "aimee-kb: spend query failed\n");
      rc = 1;
   }
   else if (want_json)
   {
      char *json = kb_insights_spend_json(has_team, (long long)team, has_project,
                                          (long long)project, since, until, rows, n);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
      else
      {
         fprintf(stderr, "aimee-kb: response build failed\n");
         rc = 1;
      }
   }
   else
   {
      printf("team\tproject\tmodel\tprompt\tcompletion\tcache_read\tcache_write\tcost_usd\tcalls\n");
      for (int i = 0; i < n; i++)
      {
         printf("%lld\t", (long long)rows[i].team_id);
         if (rows[i].has_project)
            printf("%lld", (long long)rows[i].project_id);
         else
            printf("-");
         printf("\t%s\t%lld\t%lld\t%lld\t%lld\t%s\t%lld\n", rows[i].billable_model,
                (long long)rows[i].prompt_tokens, (long long)rows[i].completion_tokens,
                (long long)rows[i].cache_read_tokens, (long long)rows[i].cache_write_tokens,
                rows[i].cost_usd, (long long)rows[i].calls);
      }
   }
   db2_shutdown();
   return rc;
}

/* Operator-facing budget admin CLI (P4a):
 *   aimee-kb budget set --team X [--project Y] --period day|month --limit USD [--soft USD]
 *   aimee-kb budget show --team X [--project Y]
 * Runs in-process against DB2 as the install owner principal (an org-admin, so the
 * SECURITY DEFINER org_budget_set/show admin gate passes). Money is a NUMERIC string,
 * never a float. BUDGET ONLY (the rate limiter is P4b; the egress wiring is P2b). */
static int kb_cmd_budget(int argc, char **argv)
{
   const char *sub = argc > 2 ? argv[2] : "";
   int has_project = 0;
   int64_t team = 0, project = 0;
   const char *period = NULL, *limit = NULL, *soft = NULL;
   int has_team = 0;
   for (int i = 3; i < argc; i++)
   {
      if (strcmp(argv[i], "--team") == 0 && i + 1 < argc)
      {
         team = strtoll(argv[++i], NULL, 10);
         has_team = 1;
      }
      else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc)
      {
         project = strtoll(argv[++i], NULL, 10);
         has_project = 1;
      }
      else if (strcmp(argv[i], "--period") == 0 && i + 1 < argc)
         period = argv[++i];
      else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
         limit = argv[++i];
      else if (strcmp(argv[i], "--soft") == 0 && i + 1 < argc)
         soft = argv[++i];
   }
   if (strcmp(sub, "set") != 0 && strcmp(sub, "show") != 0)
   {
      fprintf(stderr,
              "Usage: aimee-kb budget set --team X [--project Y] --period day|month "
              "--limit USD [--soft USD]\n"
              "       aimee-kb budget show --team X [--project Y]\n");
      return 1;
   }
   if (!has_team || team <= 0)
   {
      fprintf(stderr, "aimee-kb: --team (positive integer) is required\n");
      return 1;
   }

   if (kb_cmd_tenancy_init_db2() != 0)
      return 1;
   kb_principal_t owner;
   kb_verify_result_t ovr;
   memset(&ovr, 0, sizeof(ovr));
   if (kb_principal_from_verify(&ovr, "", &owner) != 0)
   {
      db2_shutdown();
      return 1;
   }
   if (db2_tenant_scope_begin(&owner, 0) != 0)
   {
      fprintf(stderr, "aimee-kb: tenant scope failed (is this a hardened tier? run migrations)\n");
      db2_shutdown();
      return 1;
   }

   int rc = 1;
   if (strcmp(sub, "set") == 0)
   {
      if (!period || (strcmp(period, "day") != 0 && strcmp(period, "month") != 0) || !limit)
      {
         fprintf(stderr, "aimee-kb: budget set needs --period day|month and --limit USD\n");
         db2_tenant_scope_rollback();
         db2_shutdown();
         return 1;
      }
      int64_t id = 0;
      int r = db2_org_budget_set(team, has_project, project, period, limit, soft, &id);
      if (r == 0)
      {
         printf("{\"id\":%lld,\"team\":%lld,", (long long)id, (long long)team);
         if (has_project)
            printf("\"project\":%lld,", (long long)project);
         printf("\"period\":\"%s\",\"limit_usd\":\"%s\"}\n", period, limit);
         rc = 0;
      }
      else if (r == DB2_BUDGET_ERR_DENIED)
         fprintf(stderr, "budget set failed (not authorized — org-admin required)\n");
      else if (r == DB2_BUDGET_ERR_RETRO)
         fprintf(stderr, "budget set failed (retroactive reduction below committed spend+reserved)\n");
      else
         fprintf(stderr, "budget set failed\n");
   }
   else /* show */
   {
      db2_org_budget_row_t rows[DB2_BUDGET_MAX_ROWS];
      int n = db2_org_budget_show(team, has_project, project, rows, DB2_BUDGET_MAX_ROWS);
      if (n >= 0)
      {
         printf("team\tproject\tperiod\tperiod_id\tlimit_usd\tsoft_usd\tspend_usd\treserved_usd\tremaining_usd\n");
         for (int i = 0; i < n; i++)
         {
            printf("%lld\t", (long long)rows[i].team_id);
            if (rows[i].has_project)
               printf("%lld", (long long)rows[i].project_id);
            else
               printf("-");
            printf("\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", rows[i].period, rows[i].period_id,
                   rows[i].limit_usd, rows[i].soft_limit_usd[0] ? rows[i].soft_limit_usd : "-",
                   rows[i].spend_usd, rows[i].reserved_usd, rows[i].remaining_usd);
         }
         rc = 0;
      }
      else if (n == DB2_BUDGET_ERR_DENIED)
         fprintf(stderr, "budget show failed (not authorized — org-admin or team-lead required)\n");
      else
         fprintf(stderr, "budget show failed\n");
   }

   if (rc == 0)
   {
      if (db2_tenant_scope_commit() != 0)
      {
         fprintf(stderr, "aimee-kb: commit failed — the change was NOT persisted\n");
         rc = 1;
      }
   }
   else
      db2_tenant_scope_rollback();
   db2_shutdown();
   return rc;
}

/* Operator-facing rate-policy admin CLI (P4b):
 *   aimee-kb rate set --dim D --scope S --window SECS --max N
 *   aimee-kb rate show --dim D --scope S
 * Runs in-process against DB2 as the install owner principal (an org-admin, so the
 * SECURITY DEFINER org_rate_policy_set/show admin gate passes). RATE ONLY (the budget
 * core is P4a; the org_rate_check egress enforcement is P2b). */
static int kb_cmd_rate(int argc, char **argv)
{
   const char *sub = argc > 2 ? argv[2] : "";
   const char *dim = NULL, *scope = NULL;
   int64_t window = 0, maxc = -1;
   int has_window = 0, has_max = 0;
   for (int i = 3; i < argc; i++)
   {
      if (strcmp(argv[i], "--dim") == 0 && i + 1 < argc)
         dim = argv[++i];
      else if (strcmp(argv[i], "--scope") == 0 && i + 1 < argc)
         scope = argv[++i];
      else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc)
      {
         window = strtoll(argv[++i], NULL, 10);
         has_window = 1;
      }
      else if (strcmp(argv[i], "--max") == 0 && i + 1 < argc)
      {
         maxc = strtoll(argv[++i], NULL, 10);
         has_max = 1;
      }
   }
   if ((strcmp(sub, "set") != 0 && strcmp(sub, "show") != 0) || !dim || !scope)
   {
      fprintf(stderr,
              "Usage: aimee-kb rate set --dim team|project|cert|model|cred_slot --scope S "
              "--window SECS --max N\n"
              "       aimee-kb rate show --dim D --scope S\n");
      return 1;
   }

   if (kb_cmd_tenancy_init_db2() != 0)
      return 1;
   kb_principal_t owner;
   kb_verify_result_t ovr;
   memset(&ovr, 0, sizeof(ovr));
   if (kb_principal_from_verify(&ovr, "", &owner) != 0)
   {
      db2_shutdown();
      return 1;
   }
   if (db2_tenant_scope_begin(&owner, 0) != 0)
   {
      fprintf(stderr, "aimee-kb: tenant scope failed (is this a hardened tier? run migrations)\n");
      db2_shutdown();
      return 1;
   }

   int rc = 1;
   if (strcmp(sub, "set") == 0)
   {
      if (!has_window || window <= 0 || !has_max || maxc < 0)
      {
         fprintf(stderr, "aimee-kb: rate set needs --window >0 and --max >=0\n");
         db2_tenant_scope_rollback();
         db2_shutdown();
         return 1;
      }
      int64_t id = 0;
      int r = db2_org_rate_policy_set(dim, scope, window, maxc, &id);
      if (r == 0)
      {
         printf("{\"id\":%lld,\"dim\":\"%s\",\"scope\":\"%s\",\"window_seconds\":%lld,\"max_count\":%lld}\n",
                (long long)id, dim, scope, (long long)window, (long long)maxc);
         rc = 0;
      }
      else if (r == DB2_RATE_ERR_DENIED)
         fprintf(stderr, "rate set failed (not authorized — org-admin required)\n");
      else
         fprintf(stderr, "rate set failed\n");
   }
   else /* show */
   {
      db2_org_rate_policy_t rows[DB2_RATE_MAX_ROWS];
      int n = db2_org_rate_policy_show(dim, scope, rows, DB2_RATE_MAX_ROWS);
      if (n >= 0)
      {
         printf("id\tdim\tscope\twindow_seconds\tmax_count\n");
         for (int i = 0; i < n; i++)
            printf("%lld\t%s\t%s\t%lld\t%lld\n", (long long)rows[i].id, rows[i].dim,
                   rows[i].scope_key, (long long)rows[i].window_seconds,
                   (long long)rows[i].max_count);
         rc = 0;
      }
      else if (n == DB2_RATE_ERR_DENIED)
         fprintf(stderr, "rate show failed (not authorized — org-admin or team-lead required)\n");
      else
         fprintf(stderr, "rate show failed\n");
   }

   if (rc == 0)
   {
      if (db2_tenant_scope_commit() != 0)
      {
         fprintf(stderr, "aimee-kb: commit failed — the change was NOT persisted\n");
         rc = 1;
      }
   }
   else
      db2_tenant_scope_rollback();
   db2_shutdown();
   return rc;
}

static int kb_cmd_tenancy(int argc, char **argv)
{
   const char *group = argv[1]; /* "team" | "project" */
   const char *sub = argc > 2 ? argv[2] : "";
   if (kb_cmd_tenancy_init_db2() != 0)
      return 1;

   /* Act as the install owner (bootstrap admin). */
   kb_principal_t owner;
   kb_verify_result_t ovr;
   memset(&ovr, 0, sizeof(ovr));
   if (kb_principal_from_verify(&ovr, "", &owner) != 0)
      return 1;

   int rc_http = 1;
   if (db2_tenant_scope_begin(&owner, 0) != 0)
   {
      fprintf(stderr, "aimee-kb: tenant scope failed (is this a hardened tier? run migrations)\n");
      db2_shutdown();
      return 1;
   }

   if (strcmp(group, "team") == 0 && strcmp(sub, "create") == 0 && argc >= 4)
   {
      int64_t id = 0;
      if (db2_team_create(argv[3], "cli", &id) == 0)
      {
         printf("{\"id\":%lld,\"name\":\"%s\"}\n", (long long)id, argv[3]);
         rc_http = 0;
      }
      else
         fprintf(stderr, "create failed (not authorized or duplicate)\n");
   }
   else if (strcmp(group, "team") == 0 && strcmp(sub, "list") == 0)
   {
      db2_team_row_t rows[256];
      int n = db2_team_list(rows, 256);
      for (int i = 0; i < n; i++)
         printf("%lld\t%s\n", (long long)rows[i].id, rows[i].name);
      rc_http = (n < 0) ? 1 : 0;
   }
   else if (strcmp(group, "team") == 0 && strcmp(sub, "add-member") == 0 && argc >= 5)
   {
      int is_default = (argc >= 6 && strcmp(argv[5], "--default") == 0) ? 1 : 0;
      int64_t id = 0;
      rc_http =
          db2_membership_add(argv[4], strtoll(argv[3], NULL, 10), is_default, &id) == 0 ? 0 : 1;
      if (rc_http == 0)
         printf("ok\n");
      else
         fprintf(stderr, "add-member failed (not authorized)\n");
   }
   else if (strcmp(group, "team") == 0 && strcmp(sub, "remove-member") == 0 && argc >= 5)
   {
      rc_http = db2_membership_remove(argv[4], strtoll(argv[3], NULL, 10)) == 0 ? 0 : 1;
      if (rc_http == 0)
         printf("ok\n");
      else
         fprintf(stderr, "remove-member failed (not authorized)\n");
   }
   else if (strcmp(group, "project") == 0 && strcmp(sub, "create") == 0 && argc >= 5)
   {
      const char *mode = argc >= 6 ? argv[5] : "team-open";
      int64_t id = 0;
      if (db2_project_create(strtoll(argv[3], NULL, 10), argv[4], mode, "cli", &id) == 0)
      {
         printf("{\"id\":%lld,\"parent\":%s,\"name\":\"%s\"}\n", (long long)id, argv[3], argv[4]);
         rc_http = 0;
      }
      else
         fprintf(stderr, "project create failed (not authorized or bad access_mode)\n");
   }
   else if (strcmp(group, "project") == 0 && strcmp(sub, "list") == 0)
   {
      int64_t parent = argc >= 4 ? strtoll(argv[3], NULL, 10) : 0;
      db2_project_row_t rows[256];
      int n = db2_project_list(parent, rows, 256);
      for (int i = 0; i < n; i++)
         printf("%lld\t%lld\t%s\t%s\n", (long long)rows[i].id, (long long)rows[i].parent,
                rows[i].name, rows[i].access_mode);
      rc_http = (n < 0) ? 1 : 0;
   }
   else if (strcmp(group, "models") == 0 && strcmp(sub, "list") == 0)
   {
      db2_model_catalog_row_t rows[512];
      int n = db2_model_catalog_list(rows, 512);
      for (int i = 0; i < n; i++)
         printf("%s\t%s\t%s\t%s\t%s\t%s\n", rows[i].model_id,
                rows[i].enabled ? "enabled" : "disabled", rows[i].provider, rows[i].wire,
                rows[i].endpoint, rows[i].display_name);
      rc_http = (n < 0) ? 1 : 0;
   }
   else if (strcmp(group, "models") == 0 && strcmp(sub, "org") == 0 && argc >= 4 &&
            (strcmp(argv[3], "add") == 0 || strcmp(argv[3], "set") == 0) && argc >= 7)
   {
      /* models org add|set <model_id> <provider> <wire> [display_name] [endpoint] [--disabled] */
      int enabled = 1;
      for (int i = 4; i < argc; i++)
         if (strcmp(argv[i], "--disabled") == 0)
            enabled = 0;
      const char *display_name =
          (argc >= 8 && strncmp(argv[7], "--", 2) != 0) ? argv[7] : "";
      const char *endpoint = (argc >= 9 && strncmp(argv[8], "--", 2) != 0) ? argv[8] : "";
      int64_t id = 0;
      if (db2_model_catalog_upsert(argv[4], display_name, argv[5], argv[6], endpoint, enabled,
                                   &id) == 0)
      {
         printf("{\"id\":%lld,\"model_id\":\"%s\"}\n", (long long)id, argv[4]);
         rc_http = 0;
      }
      else
         fprintf(stderr, "models org add failed (not authorized or invalid wire)\n");
   }
   else if (strcmp(group, "models") == 0 && strcmp(sub, "org") == 0 && argc >= 5 &&
            strcmp(argv[3], "remove") == 0)
   {
      int64_t removed = 0;
      if (db2_model_catalog_remove(argv[4], &removed) == 0)
      {
         printf("{\"model_id\":\"%s\",\"removed\":%lld}\n", argv[4], (long long)removed);
         rc_http = 0;
      }
      else
         fprintf(stderr, "models org remove failed (not authorized)\n");
   }
   else if (strcmp(group, "models") == 0 && strcmp(sub, "org") == 0 && argc >= 6 &&
            strcmp(argv[3], "entitle") == 0)
   {
      int64_t id = 0;
      if (db2_model_entitle(argv[4], strtoll(argv[5], NULL, 10), &id) == 0)
      {
         printf("{\"model_id\":\"%s\",\"team\":%s,\"id\":%lld}\n", argv[4], argv[5], (long long)id);
         rc_http = 0;
      }
      else
         fprintf(stderr, "models org entitle failed (not authorized or unknown model/team)\n");
   }
   else if (strcmp(group, "models") == 0 && strcmp(sub, "org") == 0 && argc >= 6 &&
            strcmp(argv[3], "unentitle") == 0)
   {
      int64_t removed = 0;
      if (db2_model_unentitle(argv[4], strtoll(argv[5], NULL, 10), &removed) == 0)
      {
         printf("{\"model_id\":\"%s\",\"team\":%s,\"removed\":%lld}\n", argv[4], argv[5],
                (long long)removed);
         rc_http = 0;
      }
      else
         fprintf(stderr, "models org unentitle failed (not authorized)\n");
   }
   else
   {
      fprintf(stderr, "Usage: aimee-kb team create|list|add-member|remove-member ...\n"
                      "       aimee-kb project create|list ...\n"
                      "       aimee-kb models list\n"
                      "       aimee-kb models org add|set <model_id> <provider> "
                      "<anthropic|openai|responses|gemini> [display_name] [endpoint] [--disabled]\n"
                      "       aimee-kb models org remove <model_id>\n"
                      "       aimee-kb models org entitle|unentitle <model_id> <team_id>\n");
   }

   if (rc_http == 0)
   {
      if (db2_tenant_scope_commit() != 0)
      {
         fprintf(stderr, "aimee-kb: commit failed — the change was NOT persisted\n");
         rc_http = 1;
      }
   }
   else
      db2_tenant_scope_rollback();
   db2_shutdown();
   return rc_http;
}

int main(int argc, char **argv)
{
   /* Subcommands (must precede the daemon flag loop). */
   if (argc > 1 && strcmp(argv[1], "enroll") == 0)
      return kb_cmd_enroll(argc, argv);
   if (argc > 1 && (strcmp(argv[1], "team") == 0 || strcmp(argv[1], "project") == 0 ||
                    strcmp(argv[1], "models") == 0))
      return kb_cmd_tenancy(argc, argv);
   if (argc > 1 && strcmp(argv[1], "spend") == 0)
      return kb_cmd_spend(argc, argv);
   if (argc > 1 && strcmp(argv[1], "budget") == 0)
      return kb_cmd_budget(argc, argv);
   if (argc > 1 && strcmp(argv[1], "rate") == 0)
      return kb_cmd_rate(argc, argv);

   log_level_t log_level = LOG_INFO;
   int bootstrap_db2 = 0;
   int json_output = 0;
   int http_port_override = -1; /* -1 = use config */
   const char *fusion_probe_query = NULL;

   for (int i = 1; i < argc; i++)
   {
      if (strncmp(argv[i], "--socket=", 9) == 0)
         ; /* deprecated/ignored: HTTP is now the only transport */
      else if (strncmp(argv[i], "--bg-socket=", 12) == 0)
         ; /* deprecated/ignored: HTTP is now the only transport */
      else if (strncmp(argv[i], "--fusion-probe=", 15) == 0)
         fusion_probe_query = argv[i] + 15;
      else if (strcmp(argv[i], "--bootstrap-db2") == 0)
         bootstrap_db2 = 1;
      else if (strcmp(argv[i], "--json") == 0)
         json_output = 1;
      else if (strncmp(argv[i], "--http-port=", 12) == 0)
         http_port_override = atoi(argv[i] + 12);
      else if (strncmp(argv[i], "--log-level=", 12) == 0)
      {
         if (log_parse_level(argv[i] + 12, &log_level) != 0)
         {
            fprintf(stderr, "aimee-kb: invalid log level: %s\n", argv[i] + 12);
            return 1;
         }
      }
      else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0)
      {
         fprintf(stdout, "aimee-kb %s\n", AIMEE_VERSION);
         return 0;
      }
      else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      {
         static const char *usage =
             "Usage: aimee-kb [options]\n"
             "       aimee-kb enroll --host=HOST --port=N [--scope=SCOPE]\n"
             "                       Mint a one-time enrollment connection string for a client\n"
             "  --socket=PATH        (deprecated, ignored) Unix socket path\n"
             "  --bg-socket=PATH     (deprecated, ignored) Background-worker socket path\n"
             "  --http-port=N        TCP port for /v1/* REST API (required; default 0 = off)\n"
             "  --log-level=LEVEL    Log level: error, warn, info, debug (default: info)\n"
             "  --bootstrap-db2      Provision/verify the configured DB2 Postgres database\n"
             "  --json               Emit JSON for bootstrap commands\n"
             "  --version            Print version\n"
             "  --help               Show this help\n";
         fputs(usage, stdout);
         return 0;
      }
      else
      {
         fprintf(stderr, "aimee-kb: unknown option: %s\n", argv[i]);
         return 1;
      }
   }

   if (bootstrap_db2)
      return kb_bootstrap_db2(json_output);

   log_init(log_level);
   agent_http_init();
   kb_install_signal_handlers();

   config_t kb_cfg;
   config_load(&kb_cfg);

   /* aimee-kb owns DB2; tell the DB2 layer the deployment's embedding dimension
    * (one embedder: 1024 pplx-0.6b / 2560 pplx-4b) before any db2_init() so the
    * halfvec embedding columns are created at the right size. AIMEE_EMBEDDING_DIM
    * overrides the configured value (containerized deploys without a writable
    * aimee.yaml). */
   db2_set_embedding_dim(config_resolve_embedding_dim(&kb_cfg));
   db2_set_embedding_dim_pinned(config_embedding_dim_is_pinned(&kb_cfg));
   /* unified-llm-container §2: activate the model-identity drift guard (the kb applies
    * the schema, so this is the load-bearing site). Empty embedding_model => no-op. */
   db2_set_embedder_model_id(kb_cfg.embedding_model);
   /* §2b: on a FRESH DB with no pin, let db2_init derive the dim from the running
    * embedder's /health instead of the default — but only when a REAL remote embed
    * command is configured (the lexical "builtin" has no /health and a fixed dim,
    * so probing it would never succeed and would stall the retry loop). */
   {
      const char *embed_cmd = config_embedding_command(&kb_cfg, NULL);
      if (embed_cmd && strcmp(embed_cmd, "builtin") != 0)
         embedder_probe_register(embed_cmd);
   }
   /* Size the DB2 connection pool (leased by worker threads) before db2_init. */
   db2_set_pool_size(aimee_resolve_db2_pool_size(kb_cfg.db2_connection_pool_size));

   /* AIMEE_DB2_URL, when set, is the source of truth and overrides any db2_url
    * cached in aimee.yaml from a previous boot — applied here unconditionally,
    * BEFORE the bootstrap gate below. In a container deploy the runtime injects
    * the current Postgres address on every start; if Postgres is recreated on a
    * new bridge IP, the persisted db2_url goes stale. kb_bootstrap_db2_resolve()
    * already prefers the env URL, but it only runs when db2_url is empty (the
    * gate below), so a populated-but-stale cached URL would skip the override
    * entirely and db2_init() below would dial the dead address and exit. Apply
    * the override here so the kb self-heals across Postgres IP drift. */
   config_apply_db2_url_env_override(&kb_cfg);

   /* Auto-bootstrap on startup so kb keeps working for users who upgrade past
    * the "DB2 required" cutover (#1151) without their config being touched.
    * Mirrors the init RPC's fallback chain: env URL → default URL → createdb
    * locally. Persists the resolved URL to config so subsequent starts are a
    * fast path. */
   if (!kb_cfg.db2_url[0])
   {
      cJSON *resp = cJSON_CreateObject();
      int rc = kb_bootstrap_db2_resolve(&kb_cfg, resp);
      cJSON_Delete(resp);
      if (rc != 0)
      {
         fprintf(stderr, "aimee-kb: db2_url not configured and bootstrap failed; "
                         "run `aimee init` or set AIMEE_DB2_URL\n");
         agent_http_cleanup();
         return 1;
      }
   }

   /* DB2 owns project, workspace, and global knowledge for aimee-kb.
    *
    * Wait out a not-yet-ready Postgres on a bounded backoff instead of exiting
    * on the first failure. In a container/plugin deploy aimee-kb and its Postgres
    * come up as sibling services; Postgres is routinely still starting (or, as
    * seen on the smoothnas plugin runtime, started slightly later) when kb boots.
    * A hard exit here turns that ordinary startup race into a hard outage: the
    * process dies with DB2 reported "unavailable" and, absent an external
    * supervisor that restarts it, the kb stays down until a manual restart. The
    * retry is bounded, so a genuinely misconfigured/missing DB2 still surfaces as
    * a startup failure — just after giving a slow Postgres time to arrive. */
   {
      const int db2_max_attempts = 24; /* ~2 min at 5s spacing */
      const int db2_retry_secs = 5;
      int attempt = 1;
      while (db2_init(kb_cfg.db2_url) != 0)
      {
         if (attempt >= db2_max_attempts)
         {
            fprintf(stderr, "aimee-kb: DB2 init failed for %s after %d attempts (%ds)\n",
                    kb_cfg.db2_url, attempt, attempt * db2_retry_secs);
            agent_http_cleanup();
            return 1;
         }
         fprintf(stderr, "aimee-kb: DB2 not ready (%s); retry %d/%d in %ds\n", kb_cfg.db2_url,
                 attempt, db2_max_attempts, db2_retry_secs);
         sleep(db2_retry_secs);
         attempt++;
      }
   }

   /* Seed the relation-type ontology into the shared rel_types table now that DB2
    * is up. The fact-commit path resolves each seed relation's id from this table
    * (db2_fact_commit -> db2_rel_types_resolve); without the seed every seed-relation
    * commit DEFERs and no typed fact ever lands. ensure_seed is idempotent
    * (ON CONFLICT DO NOTHING) and cheap, so running it on each start is safe.
    * Non-fatal: a failure is logged but does not block the KB. */
   if (db2_rel_types_ensure_seed() != 0)
      fprintf(stderr, "aimee-kb: warning: rel_types ontology seed failed; typed-fact "
                      "commits will DEFER until the seed lands on a later start\n");

   /* Bind the Postgres credential-vault backend (P10 slice 2) now that DB2 is up.
    * The kb org vault stores ciphertext in org_vault_secret via the SECURITY DEFINER
    * vault functions; the KEK stays behind file custody (the default provider). This
    * is the kb bind — file custody stays default; later slices add external-anchor
    * custody + seal/unseal before any key-holding activation on a hardened tier. */
   vault_store_set_backend(&vault_pg_backend);

   /* Select the custody provider for the vault's server KEK (P10/P7 slice 3b).
    * `file` (default) keeps today's self-unsealing behavior; `mock` binds the
    * test/dev seal-barrier anchor; tpm2/pkcs11/kms are declared but unimplemented
    * and FAIL CLOSED here (never a silent fallback to a plaintext root). An unknown
    * vault.custody value is likewise rejected. */
   {
      char custody_err[160] = "";
      if (kb_vault_policy_select(kb_cfg.vault_custody, custody_err, sizeof(custody_err)) != 0)
      {
         fprintf(stderr, "aimee-kb: %s\n", custody_err);
         db2_shutdown();
         agent_http_cleanup();
         return 1;
      }
   }

   /* Diagnostic mode: run the fusion off-vs-on recall probe and exit without
    * starting the service. */
   if (fusion_probe_query)
   {
      int rc = kb_run_fusion_probe(fusion_probe_query);
      db2_shutdown();
      agent_http_cleanup();
      return rc;
   }

   /* Hidden directories (`.git`, `.aimee`, `.worktrees`, etc.) hold
    * dotfile state, not source code, so they are never indexed. The
    * scanner enforces this at find-time (src/index.c:135); this startup
    * purge cleans up rows from projects that registered before that
    * guard. No-op once the index is clean. */
   {
      int purged = db2_code_index_purge_hidden_pollution();
      if (purged > 0)
         LOG_INFO("kb_index", "purged %d hidden-dir index rows on startup", purged);
   }

   if (kb_service_init(&g_ctx) != 0)
   {
      db2_shutdown();
      agent_http_cleanup();
      return 1;
   }
   g_ctx.worker_count = kb_cfg.kb_connection_workers;

   /* #4-full render backend: register the command-driven computed-style render
    * adapter when css_render_command is configured (no-op otherwise — the oracle
    * then reports UNAVAILABLE rather than guessing). */
   css_render_cmd_register();

   int http_port = http_port_override >= 0 ? http_port_override : kb_cfg.kb_api_http_port;
   if (http_port <= 0)
   {
      db2_shutdown();
      agent_http_cleanup();
      fprintf(stderr,
              "aimee-kb: HTTP is the only transport; set --http-port=N or kb_api_http_port\n");
      return 1;
   }
   /* Register the BYO OIDC/JWT verifier from the environment (no-op unless
    * AIMEE_KB_OIDC_JWKS_FILE is set) before the listener accepts requests.
    * Additive: the owner kb-token verifier stays active regardless. */
   if (kb_oidc_register_from_env() != 0)
      LOG_WARN("kb_http", "OIDC verifier config present but invalid; OIDC auth disabled");
   /* Fleet-wide JWKS (I10): prefer the shared Postgres key set over the per-instance
    * file so all stateless kb instances agree on trusted keys and IdP rotation
    * converges within the bounded refresh. Falls back to the file when no PG rows. */
   kb_oidc_jwks_fleet_enable();
   if (kb_http_start(http_port, kb_cfg.kb_api_bearer_token) != 0)
   {
      /* Another instance owns the port; yield gracefully with success so
       * systemd (Restart=on-failure) doesn't restart-loop. */
      LOG_WARN("kb_http",
               "failed to start HTTP listener on port %d; another instance likely owns it",
               http_port);
      db2_shutdown();
      agent_http_cleanup();
      return 0;
   }

   /* Optional distributed-mode mTLS listener (every request presents a CA-issued
    * client cert; scope comes from the cert). Enabled by AIMEE_KB_MTLS_PORT. */
   {
      const char *mtls_port_s = getenv("AIMEE_KB_MTLS_PORT");
      if (mtls_port_s && mtls_port_s[0])
      {
         int mport = atoi(mtls_port_s);
         const char *mhost = getenv("AIMEE_KB_MTLS_HOST");
         if (!mhost || !mhost[0])
            mhost = "localhost";
         if (kb_mtls_start(mport, kb_default_config_dir(), mhost) != 0)
            LOG_WARN("kb_mtls", "failed to start mTLS listener on port %d", mport);
         /* Zero-config bootstrap: when asked (AIMEE_KB_EMIT_ENROLL), mint a
          * one-time connection string and log it on startup so an operator can
          * read it from the container logs and hand it to a client. */
         else if (getenv("AIMEE_KB_EMIT_ENROLL"))
         {
            const char *scope = getenv("AIMEE_KB_EMIT_SCOPE");
            char conn[1024];
            if (kb_enroll_mint(kb_default_config_dir(), mhost, kb_mtls_bound_port(),
                               (scope && scope[0]) ? scope : "global", conn, sizeof(conn)) == 0)
               LOG_INFO("kb_mtls", "enrollment connection string: %s", conn);
            else
               LOG_WARN("kb_mtls", "failed to mint enrollment connection string");
         }
      }
   }

   (void)shutdown_forensics_record_unclean_exits();
   (void)shutdown_forensics_mark_started("kb", (time_t)g_ctx.start_time);
   /* HTTP listener runs on its own thread; block here until a signal
    * (SIGINT/SIGTERM/SIGHUP) flips running, then tear down. */
   while (g_ctx.running)
   {
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 200L * 1000 * 1000};
      nanosleep(&ts, NULL);
   }
   int rc = 0;
   kb_mtls_stop();
   kb_http_stop();
   kb_service_shutdown(&g_ctx);
   (void)shutdown_forensics_mark_stopped("kb", getpid());
   embedder_probe_unregister(); /* §2b: deregister the probe before db2_shutdown */
   db2_shutdown();
   agent_http_cleanup();
   return rc == 0 ? 0 : 1;
}
