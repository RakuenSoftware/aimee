#include "aimee.h"
#include "agent_exec.h"
#include "config.h"
#include "config_database.h"
#include "css_render_cmd.h"
#include "db2/code_index.h"
#include "kb_auth_oidc.h"
#include "kb_enroll.h"
#include "kb_http.h"
#include "kb_tls.h"
#include "kb_paths.h"
#include "kb_service.h"
#include "log.h"
#include "lifecycle.h"
#include "shutdown_forensics.h"
#include "util.h"
#include "cJSON.h"
#include "memory.h"
#include "memory_graph_fusion.h"
#include "db2/memory_vectors.h"
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

int main(int argc, char **argv)
{
   /* Subcommands (must precede the daemon flag loop). */
   if (argc > 1 && strcmp(argv[1], "enroll") == 0)
      return kb_cmd_enroll(argc, argv);

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
   db2_shutdown();
   agent_http_cleanup();
   return rc == 0 ? 0 : 1;
}
