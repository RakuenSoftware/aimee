#include "aimee.h"
#include "aimee_home.h"
#include "agent_exec.h"
#include "config.h"
#include "config_database.h"
#include "css_render_cmd.h"
#include "db2/code_index.h"
#include "db2/db2.h"
#include "kb_witness_cadence.h"
#include "managed_server_identity_install.h"
#include "kb_auth_oidc.h"
#include "kb_oidc_jwks_fleet.h"
#include "kb_identity.h"
#include "db2_tenant.h"
#include "team.h"
#include "membership.h"
#include "kb_insights_util.h"
#include "org_budget.h"
#include "org_rate.h"
#include "org_egress.h"
#include "org_telemetry.h"
#include "org_model_catalog.h"
#include "org_vault_key_use.h"
#include "org_spend.h"
#include "project.h"
#include "kb_enroll.h"
#include "kb_http.h"
#include "aimee/protocols/mcp/mcp_client_registry.h" /* host install:kb MCP plugins */
#include "kb_tls.h"
#include "kb_sidecar_identity.h"
#include "kb_paths.h"
#include "kb_service.h"
#include "log.h"
#include "lifecycle.h"
#include "embedder_probe.h"
#include "shutdown_forensics.h"
#include "util.h"
#include "cJSON.h"
#include "memory.h"
#include "modules/memory/memory_graph_fusion.h"
#include "db2/memory_vectors.h"
#include "db2/rel_types_store.h" /* db2_rel_types_ensure_seed (typed-fact ontology) */
#include "db2/vault_pg.h"        /* vault_pg_backend + vault_store_set_backend (kb vault bind) */
#include "kb/kb_vault_policy.h"  /* kb_vault_policy_select (custody selection, P7 §3) */
#include "kb/kb_management_runtime.h"
#include "kb/kb_vault_operator_runtime.h"
#include "kb_vault_operator_status.h"
#include "kb_vault_tpm_runtime_lock.h"
#include "db2/kb_audit_worm.h"
#include "db2/vault_operator_status_runtime.h"
#include "vault_server_key.h"         /* startup durable seal-epoch synchronization */
#include "vault_env_bootstrap.h"      /* first-boot credential env -> Vault */
#include "vault_config_bootstrap.h"   /* legacy config credential -> Vault */
#include "runtime_secret.h"           /* wipe Vault-sourced runtime cache at exit */
#include "kb_memory_audit_bridge.h"   /* record memory mutations on aimee-kb's own obs bus */
#include "kb_module_stage_adapters.h" /* process-module calls over aimee-kb's event bus */
#include <aimee/audit/obs_bus.h>
#include "log.h" /* audit_log_open — KB memory-audit ledger */
#include <signal.h>
#include <errno.h>
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

typedef struct
{
   kb_vault_activation_latch_t activation;
   kb_vault_operator_runtime_t runtime;
   kb_vault_operator_mutation_t mutation;
   int activation_initialized;
   int runtime_initialized;
   int mutation_initialized;
} kb_vault_operator_components_t;

static void kb_vault_operator_components_destroy(kb_vault_operator_components_t *components);

static int kb_vault_operator_components_init(kb_vault_operator_components_t *components,
                                             db2_vault_operator_runtime_t *database,
                                             kb_vault_tpm_runtime_lock_t *singleton)
{
   if (!components || !database || !singleton)
      return -1;
   memset(components, 0, sizeof(*components));
   if (kb_vault_activation_latch_init(&components->activation) != 0)
      return -1;
   components->activation_initialized = 1;
   if (kb_vault_operator_runtime_init(&components->runtime, database, singleton,
                                      &components->activation) != 0)
   {
      kb_vault_operator_components_destroy(components);
      return -1;
   }
   components->runtime_initialized = 1;
   kb_vault_operator_mutation_deps_t deps;
   memset(&deps, 0, sizeof(deps));
   kb_vault_operator_runtime_fill_deps(&components->runtime, &deps);
   if (kb_vault_operator_mutation_init(&components->mutation, &deps, &components->runtime) != 0)
   {
      kb_vault_operator_components_destroy(components);
      return -1;
   }
   components->mutation_initialized = 1;
   return 0;
}

static void kb_vault_operator_components_destroy(kb_vault_operator_components_t *components)
{
   if (!components)
      return;
   if (components->mutation_initialized)
      kb_vault_operator_mutation_destroy(&components->mutation);
   if (components->runtime_initialized)
      kb_vault_operator_runtime_destroy(&components->runtime);
   if (components->activation_initialized)
      kb_vault_activation_latch_destroy(&components->activation);
   memset(components, 0, sizeof(*components));
}

_Static_assert((int)DB2_VAULT_STATE_SEALED_IDLE == (int)KB_VAULT_OPERATOR_STATE_SEALED_IDLE,
               "vault operator state wire mismatch");
_Static_assert((int)DB2_VAULT_OPERATION_RECOVERY_REQUIRED ==
                   (int)KB_VAULT_OPERATOR_OPERATION_RECOVERY_REQUIRED,
               "vault operator operation wire mismatch");
_Static_assert((int)DB2_VAULT_REMEDIATION_FINALIZE == (int)KB_VAULT_OPERATOR_REMEDIATION_FINALIZE,
               "vault operator remediation wire mismatch");

static int kb_vault_operator_provider_status(void *opaque, db2_vault_provider_status_t *out)
{
   (void)opaque;
   if (!out)
      return -1;
   switch (vault_custody_selected_local_status())
   {
   case VAULT_CUSTODY_LOCAL_AVAILABLE_SEALED:
      *out = DB2_VAULT_PROVIDER_AVAILABLE_SEALED;
      return 0;
   case VAULT_CUSTODY_LOCAL_AVAILABLE_UNSEALED:
      *out = DB2_VAULT_PROVIDER_AVAILABLE_UNSEALED;
      return 0;
   case VAULT_CUSTODY_LOCAL_UNAVAILABLE:
      *out = DB2_VAULT_PROVIDER_UNAVAILABLE;
      return 0;
   case VAULT_CUSTODY_LOCAL_MALFORMED:
      *out = DB2_VAULT_PROVIDER_MALFORMED;
      return 0;
   }
   return -1;
}

static int kb_vault_operator_project(kb_vault_operator_status_t *out, void *opaque)
{
   db2_vault_operator_runtime_t *runtime = opaque;
   db2_vault_operator_status_t status;
   int rc =
       db2_vault_operator_runtime_status(runtime, kb_vault_operator_provider_status, NULL, &status);
   if (rc == DB2_VAULT_OPERATOR_UNAVAILABLE || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   out->state = (kb_vault_operator_state_t)status.state;
   out->operation_state = (kb_vault_operator_operation_state_t)status.snapshot.operation_state;
   out->remediation = (kb_vault_operator_remediation_t)status.remediation;
   out->flags = status.snapshot.operation_present ? 1u : 0u;
   out->seal_epoch = (uint64_t)status.snapshot.seal_epoch;
   out->control_fence = (uint64_t)status.snapshot.control_fence;
   out->old_generation = (uint64_t)status.snapshot.old_generation;
   out->new_generation = (uint64_t)status.snapshot.new_generation;
   out->last_opened_fence = (uint64_t)status.snapshot.last_opened_fence;
   memcpy(out->operation_id, status.snapshot.operation_id, sizeof(out->operation_id));
   return kb_vault_operator_status_validate(out) ? 0 : -1;
}

static int kb_vault_operator_service_project(kb_vault_operator_status_t *out, void *opaque)
{
   kb_vault_operator_components_t *components = opaque;
   return components && components->runtime_initialized
              ? kb_vault_operator_project(out, components->runtime.database)
              : -1;
}

static int kb_vault_operator_service_mutate(kb_vault_operator_opcode_t opcode,
                                            const uint8_t request_id[16], const uint8_t *secret,
                                            size_t secret_len, kb_vault_operator_result_t *result,
                                            void *opaque)
{
   kb_vault_operator_components_t *components = opaque;
   return components && components->mutation_initialized
              ? kb_vault_operator_mutation_execute(opcode, request_id, secret, secret_len, result,
                                                   &components->mutation)
              : -1;
}

static int kb_vault_operator_service_post_wipe(kb_vault_operator_opcode_t opcode,
                                               kb_vault_operator_result_t result, void *opaque)
{
   (void)opcode;
   (void)result;
   kb_vault_operator_components_t *components = opaque;
   return components && components->mutation_initialized
              ? kb_vault_operator_mutation_after_secret_wipe(&components->mutation)
              : -1;
}

static int kb_vault_operator_status_equal(const db2_vault_operator_status_t *a,
                                          const db2_vault_operator_status_t *b)
{
   return a && b && a->state == b->state && a->remediation == b->remediation &&
          a->provider == b->provider &&
          db2_vault_operator_snapshot_equal(&a->snapshot, &b->snapshot);
}

static int kb_cmd_vault(int argc, char **argv)
{
   if (argc < 3)
   {
      fputs("Usage: aimee-kb vault status [--json]\n"
            "       aimee-kb vault start --request-id=<32-lowercase-hex> "
            "[--secret-stdin] [--json]\n"
            "       aimee-kb vault resume [--secret-stdin] [--json]\n"
            "       aimee-kb vault unseal [--secret-stdin] [--json]\n",
            stderr);
      return KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE;
   }

   int json = 0;
   int secret_stdin = 0;
   const char *request_text = NULL;
   for (int i = 3; i < argc; i++)
   {
      if (strcmp(argv[i], "--json") == 0 && !json)
         json = 1;
      else if (strcmp(argv[i], "--secret-stdin") == 0 && !secret_stdin)
         secret_stdin = 1;
      else if (strncmp(argv[i], "--request-id=", 13) == 0 && !request_text)
         request_text = argv[i] + 13;
      else
      {
         fprintf(stderr, "aimee-kb vault: invalid or duplicate option: %s\n", argv[i]);
         return KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE;
      }
   }

   kb_vault_operator_status_t status;
   if (strcmp(argv[2], "status") == 0)
   {
      if (secret_stdin || request_text)
      {
         fputs("aimee-kb vault status: mutation options are not accepted\n", stderr);
         return KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE;
      }
      kb_vault_operator_client_result_t client = kb_vault_operator_status_client(&status);
      if (client == KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE)
      {
         fputs("aimee-kb vault status: operator status service unavailable\n", stderr);
         return client;
      }
      char output[768];
      if (kb_vault_operator_status_format(&status, json, output, sizeof(output)) < 0)
         return KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE;
      fputs(output, stdout);
      return client;
   }

   kb_vault_operator_opcode_t opcode;
   uint8_t request_id[KB_VAULT_OPERATOR_REQUEST_ID_LEN];
   const uint8_t *request = NULL;
   if (strcmp(argv[2], "start") == 0)
   {
      opcode = KB_VAULT_OPERATOR_OPCODE_START;
      if (!request_text || kb_vault_operator_request_id_parse(request_text, request_id) != 0)
      {
         fputs("aimee-kb vault start: --request-id must be 32 lowercase hexadecimal bytes\n",
               stderr);
         return KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE;
      }
      request = request_id;
   }
   else if (strcmp(argv[2], "resume") == 0)
      opcode = KB_VAULT_OPERATOR_OPCODE_RESUME;
   else if (strcmp(argv[2], "unseal") == 0)
      opcode = KB_VAULT_OPERATOR_OPCODE_UNSEAL;
   else
   {
      fprintf(stderr, "aimee-kb vault: unknown command: %s\n", argv[2]);
      return KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE;
   }
   if (opcode != KB_VAULT_OPERATOR_OPCODE_START && request_text)
   {
      fputs("aimee-kb vault: --request-id is accepted only by start\n", stderr);
      return KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE;
   }

   kb_vault_operator_result_t operation_result;
   kb_vault_operator_client_result_t client =
       kb_vault_operator_mutation_client(opcode, request, secret_stdin, &operation_result, &status);
   memset(request_id, 0, sizeof(request_id));
   if (client == KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE)
   {
      fputs("aimee-kb vault: operator mutation service unavailable\n", stderr);
      return client;
   }
   char output[896];
   if (kb_vault_operator_mutation_format(operation_result, &status, json, output, sizeof(output)) <
       0)
      return KB_VAULT_OPERATOR_CLIENT_TRANSPORT_FAILURE;
   fputs(output, stdout);
   return client;
}

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
   /* O_EXCL first, so we can tell "we just created the lock" from "a previous
    * attempt left it behind". This matters: open(O_CREAT) stamps a NEW file
    * with the current time, so the cooldown check below saw `now - mtime == 0`
    * and skipped — meaning the very FIRST bootstrap on a fresh host, the one
    * case the fallback exists for, never ran. It only became reachable once the
    * cooldown had expired, five minutes into a crash loop. */
   int created = 1;
   int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
   if (fd < 0)
   {
      created = 0;
      fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
   }
   if (fd < 0)
      return -2;
   if (flock(fd, LOCK_EX | LOCK_NB) != 0)
   {
      close(fd); /* another bootstrap is in flight */
      return -1;
   }
   struct stat st;
   time_t now = time(NULL);
   if (!created && fstat(fd, &st) == 0 && st.st_mtime > 0 &&
       now - st.st_mtime < DB2_BOOTSTRAP_COOLDOWN_SECS)
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

static int bootstrap_db2_try_url(const char *url, int save_config, cJSON *resp)
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
      if (config_set("db2_url", url) != 0)
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

/* Resolve and bootstrap DB2, persisting the URL that succeeded via config_set
 * (which republishes the live snapshot, so a caller re-reading afterwards sees
 * it). `resp` collects step-level details (used by the init RPC; pass a
 * throwaway object when calling from startup). Returns 0 on success, 1 on
 * failure. */
static int kb_bootstrap_db2_resolve(cJSON *resp)
{
   cJSON *steps = cJSON_AddArrayToObject(resp, "steps");

   /* AIMEE_DB2_URL, when set, is the source of truth and overrides any db2_url
    * cached in aimee.yaml from a previous boot. In a container deploy the
    * runtime injects the current Postgres address every start; a service it
    * depends on can be recreated onto a NEW bridge IP, so a db2_url persisted on
    * an earlier boot goes stale. Preferring the cached value (as before) made
    * the kb connect to the old/wrong address forever — even though the correct
    * URL was right there in the environment. The successful bootstrap below
    * re-persists this URL (config_set inside bootstrap_db2_try_url), refreshing
    * the cache. The cached value is used only as a fallback when AIMEE_DB2_URL is
    * unset (manual / non-container setups). That precedence now lives in
    * config_db2_url_effective() rather than being applied to a struct here.
    *
    * `url` is a stable copy for the same reason it always was: try_url used to
    * write the winner back through the same buffer it was reading, and
    * snprintf'ing a buffer onto itself is undefined -- on glibc it truncated the
    * destination to empty, leaving db2_init() with an empty URL. */
   char url[CONFIG_DB2_URL_LEN];
   int have_url = config_db2_url_effective(url, sizeof(url));

   if (have_url && bootstrap_db2_try_url(url, 1, resp) == 0)
      return 0;

   if (!have_url && bootstrap_db2_try_url(AIMEE_DB2_BOOTSTRAP_URL, 1, resp) == 0)
      return 0;

   if (!have_url)
   {
      (void)bootstrap_db2_with_local_tools(steps);
      if (bootstrap_db2_try_url(AIMEE_DB2_BOOTSTRAP_URL, 1, resp) == 0)
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
   cJSON *resp = cJSON_CreateObject();

   (void)kb_bootstrap_db2_resolve(resp);

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

/* `aimee-kb enroll --host H --port N --scope S` — mint a one-time enrollment:
 * persist/reuse the internal CA, issue a single-use token, and print the
 * `aimee://` connection string an operator hands to a client. The CA + token
 * store live under the kb config dir, so the running server can later redeem the
 * token and existing enrollments survive a restart. */
static int kb_cmd_enroll(int argc, char **argv)
{
   const char *host = NULL;
   const char *scope = NULL;
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
   if (!host || !host[0] || port <= 0 || port > 65535 || !scope || !scope[0])
   {
      /* --scope is REQUIRED. It used to default to "global", which has no ':'
       * and therefore mints an UNSCOPED certificate — the install owner, past
       * every administrative gate. A credential that powerful should never be
       * what you get for not passing a flag. */
      fprintf(stderr, "Usage: aimee-kb enroll --host=HOST --port=N --scope=SCOPE\n"
                      "  Mints a single-use enrollment token and prints the aimee:// "
                      "connection string\n"
                      "  for a client. HOST/PORT are the kb's externally reachable address.\n"
                      "\n"
                      "  SCOPE is required and decides how much the client may reach:\n"
                      "    <kind>:<id>   a scoped client, e.g. project:acme or "
                      "service:aimee-server\n"
                      "    a bare word   NO scope: the install owner, past every "
                      "administrative gate\n");
      return 1;
   }

   char conn[1024];
   if (kb_enroll_mint(kb_default_config_dir(), host, port, scope, conn, sizeof(conn)) != 0)
   {
      fprintf(stderr, "aimee-kb enroll: failed to mint enrollment (check CA and Vault custody)\n");
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
   /* memory_find_facts takes the lexical-fallback path (which skips the fusion
    * block) unless the pgvector memory collection exists, so ensure it. */
   if (pgvec_memory_vector_collection_exists() <= 0)
   {
      int dim = config_embedder_dims() > 0 ? config_embedder_dims() : 1024;
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
   char db2_url[CONFIG_DB2_URL_LEN];
   if (!config_db2_url_effective(db2_url, sizeof(db2_url)))
   {
      fprintf(stderr, "aimee-kb: db2_url not configured (set AIMEE_DB2_URL or run `aimee init`)\n");
      return -1;
   }
   db2_set_embedding_dim_default(config_embedder_dims_default());
   db2_set_embedding_dim(config_embedder_dims_current());
   /* These commands read a deployment somebody else is running. Applying the
    * schema from here races the daemon's own pass, and Postgres answers "tuple
    * concurrently updated", which surfaced below as "DB2 not reachable" against a
    * KB that was reachable and healthy. Verify instead. */
   db2_set_schema_readonly(1);
   if (db2_init(db2_url) != 0)
   {
      fprintf(stderr, "aimee-kb: DB2 not reachable at %s\n", db2_url);
      return -1;
   }
   return 0;
}

static int kb_parse_unsigned(const char *text, unsigned long long max, unsigned long long *out)
{
   if (!text || !text[0] || !out)
      return -1;
   char *end = NULL;
   errno = 0;
   unsigned long long value = strtoull(text, &end, 10);
   if (errno || !end || *end || value > max)
      return -1;
   *out = value;
   return 0;
}

static int kb_cmd_managed_server_identity(int argc, char **argv)
{
   if (argc < 3 || strcmp(argv[2], "install") != 0)
   {
      fputs("Usage: aimee-kb managed-server-identity install --server-home=PATH "
            "--host=HOST --port=N --endpoint=URL --uid=N [--force]\n"
            "  --force  re-issue the client certificate even when a stored identity\n"
            "           already matches this KB (repairs trust the KB no longer accepts)\n",
            stderr);
      return 1;
   }
   kb_managed_server_identity_install_options_t options = {0};
   int port_seen = 0, owner_seen = 0;
   unsigned long long owner = 0;
   for (int i = 3; i < argc; i++)
   {
      if (strncmp(argv[i], "--server-home=", 14) == 0 && !options.server_home)
         options.server_home = argv[i] + 14;
      else if (strncmp(argv[i], "--host=", 7) == 0 && !options.host)
         options.host = argv[i] + 7;
      else if (strncmp(argv[i], "--port=", 7) == 0 && !port_seen)
      {
         unsigned long long port = 0;
         port_seen = 1;
         if (kb_parse_unsigned(argv[i] + 7, 65535, &port) == 0)
            options.port = (int)port;
      }
      else if (strncmp(argv[i], "--endpoint=", 11) == 0 && !options.endpoint)
         options.endpoint = argv[i] + 11;
      else if (strncmp(argv[i], "--uid=", 6) == 0 && !owner_seen)
      {
         owner_seen = 1;
         if (kb_parse_unsigned(argv[i] + 6, (unsigned long long)(uid_t)-1, &owner) != 0)
            owner_seen = -1;
      }
      else if (strcmp(argv[i], "--force") == 0 && !options.force)
         options.force = 1;
      else
      {
         fprintf(stderr, "aimee-kb managed-server-identity: invalid or duplicate option: %s\n",
                 argv[i]);
         return 1;
      }
   }
   if (!options.server_home || !options.host || !options.endpoint || options.port < 1 ||
       options.port > 65535 || owner_seen != 1)
   {
      fputs("aimee-kb managed-server-identity: incomplete install options\n", stderr);
      return 1;
   }
   options.owner = (uid_t)owner;
   int initialized = 0;
   for (int attempt = 0; attempt < 60 && !initialized; attempt++)
   {
      if (kb_cmd_tenancy_init_db2() == 0)
         initialized = 1;
      else
         sleep(1);
   }
   if (!initialized)
      return 1;
   int rc = kb_managed_server_identity_install(&options) == 0 ? 0 : 1;
   if (rc)
      fputs("aimee-kb managed-server-identity: install failed\n", stderr);
   db2_shutdown();
   return rc;
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
         fprintf(stderr,
                 "aimee-kb: report too large (>%d rows); narrow --team/--project/"
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
      printf(
          "team\tproject\tmodel\tprompt\tcompletion\tcache_read\tcache_write\tcost_usd\tcalls\n");
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
      fprintf(stderr, "Usage: aimee-kb budget set --team X [--project Y] --period day|month "
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
         fprintf(stderr,
                 "budget set failed (retroactive reduction below committed spend+reserved)\n");
      else
         fprintf(stderr, "budget set failed\n");
   }
   else /* show */
   {
      db2_org_budget_row_t rows[DB2_BUDGET_MAX_ROWS];
      int n = db2_org_budget_show(team, has_project, project, rows, DB2_BUDGET_MAX_ROWS);
      if (n >= 0)
      {
         printf("team\tproject\tperiod\tperiod_id\tlimit_usd\tsoft_usd\tspend_usd\treserved_"
                "usd\tremaining_usd\n");
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
      fprintf(stderr, "Usage: aimee-kb rate set --dim team|project|cert|model|cred_slot --scope S "
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
         printf("{\"id\":%lld,\"dim\":\"%s\",\"scope\":\"%s\",\"window_seconds\":%lld,\"max_"
                "count\":%lld}\n",
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

/* aimee-kb telemetry {show, allow} — the P9a operator surface. `show` prints the
 * ingest allowlist + a Prometheus dump of the authoritative-state metrics; `allow`
 * upserts an allowlist entry (admin, WORM-audited). Acts as the install owner
 * (bootstrap admin), mirroring `aimee-kb rate`. */
static int kb_cmd_telemetry(int argc, char **argv)
{
   const char *sub = argc > 2 ? argv[2] : "";
   const char *schema = NULL, *metrics = NULL;
   int enabled = 1;
   for (int i = 3; i < argc; i++)
   {
      if (strcmp(argv[i], "--schema") == 0 && i + 1 < argc)
         schema = argv[++i];
      else if (strcmp(argv[i], "--metrics") == 0 && i + 1 < argc)
         metrics = argv[++i];
      else if (strcmp(argv[i], "--disabled") == 0)
         enabled = 0;
   }
   if (strcmp(sub, "show") != 0 && strcmp(sub, "allow") != 0)
   {
      fprintf(stderr, "Usage: aimee-kb telemetry show\n"
                      "       aimee-kb telemetry allow --schema S --metrics a,b,c [--disabled]\n");
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
   if (strcmp(sub, "show") == 0)
   {
      db2_telemetry_allow_row_t rows[DB2_TELEMETRY_ALLOW_MAX_ROWS];
      int n = db2_telemetry_allow_show(rows, DB2_TELEMETRY_ALLOW_MAX_ROWS);
      if (n >= 0)
      {
         printf("event_schema\tenabled\tmetric_names\tupdated_at\n");
         for (int i = 0; i < n; i++)
            printf("%s\t%d\t%s\t%s\n", rows[i].event_schema, rows[i].enabled, rows[i].metric_names,
                   rows[i].updated_at);
         static org_metric_row_t mrows[DB2_TELEMETRY_MAX_ROWS];
         int m = db2_metrics_snapshot(mrows, DB2_TELEMETRY_MAX_ROWS);
         if (m >= 0)
         {
            static char buf[256 * 1024];
            if (org_telemetry_render_prom(mrows, m, buf, sizeof(buf)) >= 0)
            {
               printf("\n# --- /v1/metrics (Prometheus) ---\n");
               fputs(buf, stdout);
            }
            rc = 0;
         }
         else if (m == DB2_TELEMETRY_ERR_DENIED)
            fprintf(stderr, "telemetry metrics failed (not authorized — org-admin required)\n");
         else
            fprintf(stderr, "telemetry metrics snapshot failed\n");
      }
      else if (n == DB2_TELEMETRY_ERR_DENIED)
         fprintf(stderr, "telemetry show failed (not authorized — org-admin required)\n");
      else
         fprintf(stderr, "telemetry show failed\n");
   }
   else /* allow */
   {
      if (!schema || !metrics)
      {
         fprintf(stderr, "aimee-kb: telemetry allow needs --schema S --metrics a,b,c\n");
         db2_tenant_scope_rollback();
         db2_shutdown();
         return 1;
      }
      /* Build the Postgres array literal '{a,b,c}' from the comma list. Each name
       * must be a bounded [a-zA-Z0-9_:] identifier (no quoting/escaping needed). */
      char arr[1024];
      size_t o = 0;
      arr[o++] = '{';
      arr[o] = '\0';
      int first = 1, bad = 0;
      char tmp[1024];
      snprintf(tmp, sizeof(tmp), "%s", metrics);
      for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ","))
      {
         if (!org_telemetry_metric_name_valid(tok))
         {
            bad = 1;
            break;
         }
         size_t nl = strlen(tok);
         if (o + nl + 2 >= sizeof(arr))
         {
            bad = 1;
            break;
         }
         if (!first)
            arr[o++] = ',';
         memcpy(arr + o, tok, nl);
         o += nl;
         arr[o] = '\0';
         first = 0;
      }
      if (bad || first)
      {
         fprintf(stderr, "aimee-kb: each --metrics name must match [a-zA-Z0-9_:]{1,128}\n");
         db2_tenant_scope_rollback();
         db2_shutdown();
         return 1;
      }
      arr[o++] = '}';
      arr[o] = '\0';
      int r = db2_telemetry_allow(schema, arr, enabled);
      if (r == 0)
      {
         printf("{\"event_schema\":\"%s\",\"metric_names\":\"%s\",\"enabled\":%s}\n", schema, arr,
                enabled ? "true" : "false");
         rc = 0;
      }
      else if (r == DB2_TELEMETRY_ERR_DENIED)
         fprintf(stderr, "telemetry allow failed (not authorized — org-admin required)\n");
      else
         fprintf(stderr, "telemetry allow failed\n");
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
      const char *display_name = (argc >= 8 && strncmp(argv[7], "--", 2) != 0) ? argv[7] : "";
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
   if (argc > 1 && strcmp(argv[1], "--list-credential-env-names") == 0)
      return vault_env_print_credential_names() == 0 ? 0 : 1;

   /* A native launch follows the same disposable first-boot boundary as the
    * container entrypoint. unsetenv() alone can leave inherited bytes visible
    * in /proc/<pid>/environ, so a credential-bearing process may seal into
    * Vault but may not become the long-lived KB. The bootstrap helper itself is
    * already short-lived and therefore does not need to re-exec. */
   if (!(argc > 1 && (strcmp(argv[1], "--bootstrap-vault-env") == 0 ||
                      strcmp(argv[1], "--bootstrap-vault-stdin") == 0)))
   {
      int credential_env = vault_env_has_credential_environment();
      if (credential_env < 0)
      {
         fputs("aimee-kb: malformed credential environment name\n", stderr);
         return 1;
      }
      if (credential_env > 0)
      {
         if (vault_env_bootstrap_init_all() < 0 || vault_env_has_credential_environment() != 0)
         {
            runtime_secret_clear();
            fputs("aimee-kb: first-boot credential Vault bootstrap failed\n", stderr);
            return 1;
         }
         runtime_secret_clear();
         execvp(argv[0], argv);
         fprintf(stderr, "aimee-kb: clean-environment re-exec failed: %s\n", strerror(errno));
         return 1;
      }
   }

   /* The local file Vault is still bound here (before KB switches ordinary
    * tenant Vault operations to Postgres), so it can break the DB credential
    * bootstrap cycle and hydrate process memory without retaining env secrets. */
   if (vault_env_bootstrap_init_all() < 0)
   {
      fputs("aimee-kb: credential Vault bootstrap failed\n", stderr);
      return 1;
   }
   if (argc == 2 && strcmp(argv[1], "--bootstrap-vault-stdin") == 0 &&
       (vault_env_import_stream(stdin) < 0 || vault_env_bootstrap_init_all() < 0 ||
        vault_env_has_credential_environment() != 0))
   {
      runtime_secret_clear();
      fputs("aimee-kb: streamed credential Vault bootstrap failed\n", stderr);
      return 1;
   }
   if (vault_config_bootstrap_init() < 0)
   {
      fputs("aimee-kb: credential config Vault migration failed\n", stderr);
      return 1;
   }
   {
      char legacy_enroll[1024];
      int n = snprintf(legacy_enroll, sizeof(legacy_enroll), "%s/kb-enroll-tokens",
                       kb_default_config_dir());
      if (n <= 0 || (size_t)n >= sizeof(legacy_enroll) ||
          kb_enroll_store_migrate(legacy_enroll) != 0)
      {
         fputs("aimee-kb: enrollment credential Vault migration failed\n", stderr);
         return 1;
      }
   }
   (void)atexit(runtime_secret_clear);

   if (argc > 1 && (strcmp(argv[1], "--bootstrap-vault-env") == 0 ||
                    strcmp(argv[1], "--bootstrap-vault-stdin") == 0))
      return 0;

   /* Entrypoint decision probe: presence only, never the DB credential. A KB
    * restarted without first-boot environment metadata must still select the
    * external database whose URL is held exclusively in Vault. */
   if (argc == 2 && strcmp(argv[1], "--vault-db2-external") == 0)
   {
      char db2_url[4096];
      int present = runtime_secret_get("AIMEE_DB2_URL", db2_url, sizeof(db2_url));
      char embedded[4096];
      const char *home = aimee_home();
      int n = home ? snprintf(embedded, sizeof(embedded), "postgresql:///aimee_shared?host=%s/run",
                              home)
                   : -1;
      /* Prefix match to a parameter boundary, not string equality: the entrypoint
       * seals the embedded DSN with an explicit &user=<cluster owner> so that
       * containers sharing the socket connect as the right role. That trailing
       * parameter does not make the topology external, and treating it as such
       * would stop the KB provisioning its own cluster. */
      size_t embedded_len = n > 0 ? (size_t)n : 0;
      int matches_embedded = embedded_len > 0 && embedded_len < sizeof(embedded) &&
                             strncmp(db2_url, embedded, embedded_len) == 0 &&
                             (db2_url[embedded_len] == '\0' || db2_url[embedded_len] == '&');
      int external = present && !matches_embedded;
      runtime_secret_wipe(db2_url, sizeof(db2_url));
      runtime_secret_wipe(embedded, sizeof(embedded));
      return external ? 0 : 1;
   }
   /* The entrypoint's selection query. It used to parse aimee.yaml with a sed regex —
    * a second reader of a setting, hardcoding the file paths and assuming the key sits
    * at the top level. It worked only because config_save happens to write it there,
    * and its failure was silent: an unparsed key reads as "no embedder selected", the
    * builtin serves forever, and nothing says so. Ask config instead, which is the
    * only thing that knows where the value lives and how it is spelled. */
   if (argc == 2 && strcmp(argv[1], "--print-embedding-model") == 0)
   {
      const char *model = config_embedder_model();
      if (!model || !model[0])
         return 1; /* nothing selected — the caller starts no embedder */
      printf("%s\n", model);
      return 0;
   }
   if (argc == 2 && strcmp(argv[1], "--vault-llm-auth-configured") == 0)
   {
      char token[513];
      int present = runtime_secret_get("SYNTHESIS_API_KEY", token, sizeof(token));
      runtime_secret_wipe(token, sizeof(token));
      return present ? 0 : 1;
   }

   /* Subcommands (must precede the daemon flag loop). */
   if (argc > 1 && strcmp(argv[1], "enroll") == 0)
      return kb_cmd_enroll(argc, argv);
   if (argc > 1 && strcmp(argv[1], "managed-server-identity") == 0)
      return kb_cmd_managed_server_identity(argc, argv);
   if (argc > 1 && (strcmp(argv[1], "team") == 0 || strcmp(argv[1], "project") == 0 ||
                    strcmp(argv[1], "models") == 0))
      return kb_cmd_tenancy(argc, argv);
   if (argc > 1 && strcmp(argv[1], "spend") == 0)
      return kb_cmd_spend(argc, argv);
   if (argc > 1 && strcmp(argv[1], "budget") == 0)
      return kb_cmd_budget(argc, argv);
   if (argc > 1 && strcmp(argv[1], "rate") == 0)
      return kb_cmd_rate(argc, argv);
   if (argc > 1 && strcmp(argv[1], "telemetry") == 0)
      return kb_cmd_telemetry(argc, argv);
   if (argc > 1 && strcmp(argv[1], "vault") == 0)
      return kb_cmd_vault(argc, argv);

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
             "       aimee-kb enroll --host=HOST --port=N --scope=SCOPE\n"
             "                       Mint a one-time enrollment connection string for a client.\n"
             "                       SCOPE is required: '<kind>:<id>' scopes the client, a bare\n"
             "                       word mints the install owner.\n"
             "       aimee-kb vault status [--json]\n"
             "       aimee-kb vault start --request-id=<32-lowercase-hex> "
             "[--secret-stdin] [--json]\n"
             "       aimee-kb vault resume [--secret-stdin] [--json]\n"
             "       aimee-kb vault unseal [--secret-stdin] [--json]\n"
             "       aimee-kb team create|list|add-member|remove-member ...\n"
             "       aimee-kb project create|list ...\n"
             "       aimee-kb models list | models org add|set|remove|entitle|unentitle ...\n"
             "       aimee-kb budget set|show --team N ...\n"
             "       aimee-kb rate set|show --dim D --scope S ...\n"
             "       aimee-kb spend [--team N] [--since D] [--until D] [--json]\n"
             "       aimee-kb telemetry show|allow ...\n"
             "                       Run any of the above with no arguments for its own usage.\n"
             "                       See docs/ORG_GOVERNANCE.md.\n"
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

   /* Seed the live snapshot first: from here every config read in this process
    * is an accessor against it, and the bootstrap below republishes through
    * config_set when it persists a resolved db2_url. */
   (void)config_snapshot_seed();

   /* Copied out because kb_vault_tpm_runtime_identity hands BACK one of these
    * pointers (whichever wins over the env) and main holds it to the end. */
   char vault_tpm2_tcti[CONFIG_COPY_MAX];
   char vault_tpm2_nv_index[CONFIG_COPY_MAX];
   char vault_custody[CONFIG_COPY_MAX];
   config_vault_tpm2_tcti_copy(vault_tpm2_tcti, sizeof(vault_tpm2_tcti));
   config_vault_tpm2_nv_index_copy(vault_tpm2_nv_index, sizeof(vault_tpm2_nv_index));
   config_vault_custody_copy(vault_custody, sizeof(vault_custody));

   /* aimee-kb records the AUTHORITATIVE memory-mutation events on its own
    * observability bus at the store (every caller). Open the KB audit ledger so
    * the bus consumer can persist the rows, then install the store-side hook. */
   audit_log_open();
   if (obs_bus_configure_daemon_module_runtime("kb", kb_default_config_dir()) != 0)
   {
      fputs("aimee-kb: invalid module-bus path configuration\n", stderr);
      audit_log_close();
      agent_http_cleanup();
      return 1;
   }
   kb_module_stage_adapters_configure();
   kb_memory_audit_bridge_install();

   /* P7-D3a is an all-or-none service-manager contract. The listener fd and
    * pathname are fixed in the wire module; only activation and the dedicated
    * primary Postgres credential are environment-backed. */
   const char *vault_operator_enable = getenv("AIMEE_KB_VAULT_OPERATOR_ENABLED");
   const char *vault_orchestrator_url = getenv("AIMEE_KB_VAULT_ORCHESTRATOR_URL");
   int vault_operator_enabled = vault_operator_enable && strcmp(vault_operator_enable, "1") == 0;
   const char *vault_tpm2_effective_tcti = NULL;
   const char *vault_tpm2_effective_nv_index = NULL;
   kb_vault_tpm_runtime_identity(vault_tpm2_tcti, vault_tpm2_nv_index, &vault_tpm2_effective_tcti,
                                 &vault_tpm2_effective_nv_index);
   if ((vault_operator_enable && !vault_operator_enabled) ||
       (vault_operator_enabled != (vault_orchestrator_url && vault_orchestrator_url[0])))
   {
      fputs("aimee-kb: incomplete or invalid vault operator configuration\n", stderr);
      agent_http_cleanup();
      return 1;
   }
   if (vault_operator_enabled && strcmp(vault_custody, "tpm2") != 0)
   {
      fputs("aimee-kb: vault operator status requires TPM2 custody\n", stderr);
      agent_http_cleanup();
      return 1;
   }
   kb_vault_tpm_runtime_lock_t *vault_tpm_runtime_lock = NULL;
   db2_vault_operator_runtime_t vault_operator_runtime;
   memset(&vault_operator_runtime, 0, sizeof(vault_operator_runtime));
   int vault_operator_runtime_opened = 0;
   kb_vault_operator_service_t *vault_operator_service = NULL;
   kb_vault_operator_components_t vault_operator_components;
   memset(&vault_operator_components, 0, sizeof(vault_operator_components));
   db2_vault_operator_status_t vault_operator_startup_before;
   memset(&vault_operator_startup_before, 0, sizeof(vault_operator_startup_before));

   /* aimee-kb owns DB2; tell the DB2 layer the deployment's embedding dimension
    * (one embedder: 1024 pplx-0.6b / 2560 pplx-4b) before any db2_init() so the
    * halfvec embedding columns are created at the right size. EMBEDDER_DIMS
    * overrides the configured value (containerized deploys without a writable
    * aimee.yaml). */
   db2_set_embedding_dim_default(config_embedder_dims_default());
   db2_set_embedding_dim(config_resolve_embedder_dims_current());
   db2_set_embedding_dim_pinned(config_embedder_dims_pinned_current());
   /* unified-llm-container §2: activate the model-identity drift guard (the kb applies
    * the schema, so this is the load-bearing site). Empty embedding_model => no-op. */
   db2_set_embedder_model_id(config_embedder_model());
   /* §2b: register the embedder probes. Unconditionally, for whatever embed command is
    * configured: embedder_probe_register decides which probes that command supports.
    * The distinction belongs to the module that knows what each probe requires, not to
    * its caller. */
   embedder_probe_register(config_embedder_command_current(NULL));
   /* Size the DB2 connection pool (leased by worker threads) before db2_init. */
   db2_set_pool_size(aimee_resolve_db2_pool_size(config_db2_connection_pool_size()));

   /* AIMEE_DB2_URL, when set, is the source of truth and overrides any db2_url
    * cached in aimee.yaml from a previous boot — applied here unconditionally,
    * BEFORE the bootstrap gate below. In a container deploy the runtime injects
    * the current Postgres address on every start; if Postgres is recreated on a
    * new bridge IP, the persisted db2_url goes stale. kb_bootstrap_db2_resolve()
    * already prefers the env URL, but it only runs when db2_url is empty (the
    * gate below), so a populated-but-stale cached URL would skip the override
    * entirely and db2_init() below would dial the dead address and exit. The
    * precedence now lives in config_db2_url_effective(), which every read below
    * goes through, so there is no struct to pre-apply it to and no window in
    * which a stale cached value is visible. */

   /* Auto-bootstrap on startup so kb keeps working for users who upgrade past
    * the "DB2 required" cutover (#1151) without their config being touched.
    * Mirrors the init RPC's fallback chain: env URL → default URL → createdb
    * locally. Persists the resolved URL to config so subsequent starts are a
    * fast path. */
   char db2_url[CONFIG_DB2_URL_LEN];
   if (!config_db2_url_effective(db2_url, sizeof(db2_url)))
   {
      cJSON *resp = cJSON_CreateObject();
      int rc = kb_bootstrap_db2_resolve(resp);
      if (rc != 0)
      {
         /* kb_bootstrap_db2_resolve already recorded WHY each fallback failed —
          * the per-step command outcomes plus a message and a remediation. That
          * detail used to be discarded, so the only thing an operator saw was
          * "bootstrap failed", which does not distinguish "Postgres is not
          * running" from "this image ships no Postgres at all" (the published
          * aimee-kb:latest predating the embedded-DB2 packaging is exactly the
          * latter). Print it: this message is the whole diagnosis for a KB that
          * will not start. */
         fprintf(stderr, "aimee-kb: db2_url not configured and bootstrap failed; "
                         "run `aimee init` or set AIMEE_DB2_URL\n");
         const cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
         if (cJSON_IsString(msg) && msg->valuestring[0])
            fprintf(stderr, "aimee-kb:   cause: %s\n", msg->valuestring);
         const cJSON *steps = cJSON_GetObjectItemCaseSensitive(resp, "steps");
         const cJSON *step = NULL;
         cJSON_ArrayForEach(step, steps)
         {
            char *one = cJSON_PrintUnformatted(step);
            if (one)
            {
               fprintf(stderr, "aimee-kb:   step: %s\n", one);
               free(one);
            }
         }
         const cJSON *fix = cJSON_GetObjectItemCaseSensitive(resp, "remediation");
         if (cJSON_IsString(fix) && fix->valuestring[0])
            fprintf(stderr, "aimee-kb:   remediation: %s\n", fix->valuestring);
         cJSON_Delete(resp);
         agent_http_cleanup();
         return 1;
      }
      cJSON_Delete(resp);
   }

   /* Publish the fully resolved daemon config before request threads start.
    * This keeps hot-path config reads on the lock-free snapshot instead of
    * racing through the process-wide file mtime cache. */
   /* Republish: the bootstrap above may have persisted a resolved db2_url. */
   (void)config_snapshot_seed();
   /* And re-read it. The value taken before the bootstrap is the PRE-bootstrap
    * one; when the bootstrap succeeded it wrote a different URL, and dialling the
    * old one here would undo the whole point of bootstrapping. */
   (void)config_db2_url_effective(db2_url, sizeof(db2_url));

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
      while (db2_init(db2_url) != 0)
      {
         if (attempt >= db2_max_attempts)
         {
            fprintf(stderr, "aimee-kb: DB2 init failed for %s after %d attempts (%ds)\n", db2_url,
                    attempt, attempt * db2_retry_secs);
            agent_http_cleanup();
            return 1;
         }
         fprintf(stderr, "aimee-kb: DB2 not ready (%s); retry %d/%d in %ds\n", db2_url, attempt,
                 db2_max_attempts, db2_retry_secs);
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

   /* Every TPM2-custodied daemon takes the same NV-index singleton, including
    * deployments where D3 operator status is disabled. This closes the mixed
    * enabled/disabled race before custody initialization or any listener. */
   if (strcmp(vault_custody, "tpm2") == 0)
   {
      char lock_error[192] = "";
      if (kb_vault_tpm_runtime_lock_acquire(
              vault_tpm2_effective_tcti, vault_tpm2_effective_nv_index, &vault_tpm_runtime_lock,
              lock_error, sizeof(lock_error)) != KB_VAULT_TPM_RUNTIME_LOCK_OK ||
          kb_vault_tpm_runtime_lock_revalidate(vault_tpm_runtime_lock) !=
              KB_VAULT_TPM_RUNTIME_LOCK_OK)
      {
         fprintf(stderr, "aimee-kb: %s; refusing to start\n",
                 lock_error[0] ? lock_error : "TPM runtime singleton validation failed");
         db2_shutdown();
         kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
         agent_http_cleanup();
         return 1;
      }
   }

   /* Select the custody provider for the vault's server KEK (P10/P7 slice 3b).
    * `file` (default) keeps today's self-unsealing behavior; `mock` binds the
    * test/dev seal-barrier anchor; tpm2/pkcs11/kms are declared but unimplemented
    * and FAIL CLOSED here (never a silent fallback to a plaintext root). An unknown
    * vault.custody value is likewise rejected. */
   {
      char custody_err[160] = "";
      if (kb_vault_policy_select(vault_custody, custody_err, sizeof(custody_err)) != 0)
      {
         fprintf(stderr, "aimee-kb: %s\n", custody_err);
         db2_shutdown();
         kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
         agent_http_cleanup();
         return 1;
      }
   }

   {
      /* P7-witness-e2: a key-holding kb must have a working checkpoint signer
       * before serving, so org key use can never outrun the evidence that witnesses
       * it. No-op on a dev/no-live-key kb. */
      char witness_err[220] = "";
      if (kb_witness_boot_check(witness_err, sizeof(witness_err)) != 0)
      {
         fprintf(stderr, "aimee-kb: %s\n", witness_err);
         db2_shutdown();
         kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
         agent_http_cleanup();
         return 1;
      }
   }

   if (vault_operator_enabled)
   {
      char operator_error[256] = "";
      if (kb_vault_tpm_runtime_lock_revalidate(vault_tpm_runtime_lock) !=
              KB_VAULT_TPM_RUNTIME_LOCK_OK ||
          vault_seal() != 0)
      {
         fputs("aimee-kb: cannot establish sealed D3 operator startup state\n", stderr);
         db2_shutdown();
         kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
         agent_http_cleanup();
         return 1;
      }
#if defined(AIMEE_P7_D3_INTEGRATION_TEST_OVERRIDE)
      LOG_WARN("kb.vault", "P7-D3 integration-only loopback TPM override is ACTIVE");
      if (db2_kb_audit_append("integration", "aimee-kb", "vault.tpm.test_override", "p7-d3",
                              "allow", "integration-only build flag active") != 0)
      {
         fputs("aimee-kb: cannot WORM-audit P7-D3 test override; refusing to start\n", stderr);
         db2_shutdown();
         kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
         agent_http_cleanup();
         return 1;
      }
#endif
      if (db2_vault_operator_runtime_open(&vault_operator_runtime, vault_orchestrator_url,
                                          operator_error,
                                          sizeof(operator_error)) != DB2_VAULT_OPERATOR_OK ||
          db2_vault_operator_runtime_status(
              &vault_operator_runtime, kb_vault_operator_provider_status, NULL,
              &vault_operator_startup_before) != DB2_VAULT_OPERATOR_OK ||
          kb_vault_operator_startup_mode(
              (kb_vault_operator_state_t)vault_operator_startup_before.state) < 0)
      {
         fprintf(stderr, "aimee-kb: vault operator authority/status unavailable: %s\n",
                 operator_error[0] ? operator_error : "initial status failed");
         db2_vault_operator_runtime_close(&vault_operator_runtime);
         db2_shutdown();
         kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
         agent_http_cleanup();
         return 1;
      }
      vault_operator_runtime_opened = 1;
   }

   /* Diagnostic mode: run the fusion off-vs-on recall probe and exit without
    * starting the service. */
   if (fusion_probe_query)
   {
      int rc = kb_run_fusion_probe(fusion_probe_query);
      if (vault_operator_runtime_opened)
         db2_vault_operator_runtime_close(&vault_operator_runtime);
      db2_shutdown();
      kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
      agent_http_cleanup();
      return rc;
   }

   /* Synchronize the in-process use barrier with durable primary control before
    * any service or worker can admit a key use. PKCS#11/KMS selection may have
    * already logged in and unsealed its provider; durable `sealed=true` always
    * wins and is forced into the provider before the epoch is installed. Any
    * read, seal, or initialization failure is terminal and gets one final
    * fail-closed seal attempt before process teardown. */
   {
      int64_t primary_epoch = 0;
      int primary_sealed = 0;
      int startup_tx = db2_vault_control_startup_begin(&primary_epoch, &primary_sealed) == 0;
      int startup_ok =
          startup_tx && primary_epoch > 0 && (primary_sealed == 0 || primary_sealed == 1);
      const char *startup_error = "vault control startup status invalid";
      if (startup_ok && primary_sealed && vault_seal() != 0)
      {
         startup_ok = 0;
         startup_error = "durable vault is sealed but custody seal failed";
      }
      if (startup_ok &&
          vault_primary_epoch_initialize((uint64_t)primary_epoch) != VAULT_MAINTENANCE_OK)
      {
         startup_ok = 0;
         startup_error = "vault primary seal epoch initialization failed";
      }
      if (startup_tx && db2_vault_control_startup_end(startup_ok) != 0)
      {
         startup_ok = 0;
         startup_error = "vault control startup transaction failed";
      }
      if (!startup_ok)
      {
         (void)vault_seal();
         fprintf(stderr, "aimee-kb: %s; refusing to start\n", startup_error);
         if (vault_operator_runtime_opened)
            db2_vault_operator_runtime_close(&vault_operator_runtime);
         db2_shutdown();
         kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
         agent_http_cleanup();
         return 1;
      }
   }

   if (vault_operator_enabled)
   {
      db2_vault_operator_status_t startup_after;
      memset(&startup_after, 0, sizeof(startup_after));
      if (db2_vault_operator_runtime_status(&vault_operator_runtime,
                                            kb_vault_operator_provider_status, NULL,
                                            &startup_after) != DB2_VAULT_OPERATOR_OK ||
          !kb_vault_operator_status_equal(&vault_operator_startup_before, &startup_after) ||
          kb_vault_operator_startup_mode((kb_vault_operator_state_t)startup_after.state) < 0 ||
          kb_vault_tpm_runtime_lock_revalidate(vault_tpm_runtime_lock) !=
              KB_VAULT_TPM_RUNTIME_LOCK_OK ||
          kb_vault_operator_components_init(&vault_operator_components, &vault_operator_runtime,
                                            vault_tpm_runtime_lock) != 0 ||
          !(vault_operator_service = kb_vault_operator_service_start_mutations_ex(
                KB_VAULT_OPERATOR_LISTEN_FD, kb_vault_operator_service_project,
                kb_vault_operator_service_mutate, kb_vault_operator_service_post_wipe,
                &vault_operator_components)))
      {
         fputs("aimee-kb: vault operator post-epoch status/listener validation failed\n", stderr);
         kb_vault_operator_components_destroy(&vault_operator_components);
         db2_vault_operator_runtime_close(&vault_operator_runtime);
         vault_operator_runtime_opened = 0;
         db2_shutdown();
         kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
         agent_http_cleanup();
         return 1;
      }

      /* A sealed startup exposes only the fixed operator socket until one exact
       * mutation completes the durable open and publishes the activation latch. */
      if (kb_vault_operator_startup_mode((kb_vault_operator_state_t)startup_after.state) > 0)
      {
         LOG_INFO("kb.vault", "vault operator local mode active (state=%d)",
                  (int)startup_after.state);
         g_ctx.start_time = (uint64_t)time(NULL);
         g_ctx.running = 1;
         kb_vault_operator_status_t activated;
         memset(&activated, 0, sizeof(activated));
         while (g_ctx.running)
         {
            int wait_rc = kb_vault_activation_latch_wait(&vault_operator_components.activation, 200,
                                                         &activated);
            if (wait_rc < 0)
            {
               g_ctx.running = 0;
               break;
            }
            if (wait_rc > 0)
               break;
         }
         if (!g_ctx.running ||
             kb_vault_operator_mutation_activation_window_valid(
                 &vault_operator_components.mutation) != 0 ||
             kb_vault_operator_runtime_activation_validate(&vault_operator_components.runtime,
                                                           &activated) != 0 ||
             kb_vault_operator_mutation_activation_window_valid(
                 &vault_operator_components.mutation) != 0)
         {
            (void)vault_seal();
            kb_vault_operator_service_stop(vault_operator_service);
            vault_operator_service = NULL;
            kb_vault_operator_components_destroy(&vault_operator_components);
            db2_vault_operator_runtime_close(&vault_operator_runtime);
            vault_operator_runtime_opened = 0;
            embedder_probe_unregister();
            db2_shutdown();
            kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
            agent_http_cleanup();
            return g_ctx.running ? 1 : 0;
         }
      }
      if (kb_vault_operator_runtime_mark_general_serving(&vault_operator_components.runtime) != 0)
      {
         (void)vault_seal();
         kb_vault_operator_service_stop(vault_operator_service);
         vault_operator_service = NULL;
         kb_vault_operator_components_destroy(&vault_operator_components);
         db2_vault_operator_runtime_close(&vault_operator_runtime);
         vault_operator_runtime_opened = 0;
         db2_shutdown();
         kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
         agent_http_cleanup();
         return 1;
      }
      if (kb_vault_operator_mutation_mark_general_serving(&vault_operator_components.mutation) != 0)
      {
         (void)vault_seal();
         kb_vault_operator_service_stop(vault_operator_service);
         vault_operator_service = NULL;
         kb_vault_operator_components_destroy(&vault_operator_components);
         db2_vault_operator_runtime_close(&vault_operator_runtime);
         vault_operator_runtime_opened = 0;
         db2_shutdown();
         kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
         agent_http_cleanup();
         return 1;
      }
   }

#if defined(AIMEE_P2B_INTEGRATION_TEST_OVERRIDE)
   if (kb_egress_release_allowed())
   {
      LOG_WARN("kb.egress", "P2b integration-only egress override is ACTIVE");
      if (db2_kb_audit_append("integration", "aimee-kb", "egress.test_override", "p2b", "allow",
                              "integration-only build flag active") != 0)
      {
         fprintf(stderr, "aimee-kb: cannot WORM-audit P2b test override; refusing to start\n");
         kb_vault_operator_service_stop(vault_operator_service);
         vault_operator_service = NULL;
         kb_vault_operator_components_destroy(&vault_operator_components);
         if (vault_operator_runtime_opened)
            db2_vault_operator_runtime_close(&vault_operator_runtime);
         db2_shutdown();
         kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
         agent_http_cleanup();
         return 1;
      }
   }
#endif

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
      kb_vault_operator_service_stop(vault_operator_service);
      vault_operator_service = NULL;
      kb_vault_operator_components_destroy(&vault_operator_components);
      if (vault_operator_runtime_opened)
         db2_vault_operator_runtime_close(&vault_operator_runtime);
      db2_shutdown();
      kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
      agent_http_cleanup();
      return 1;
   }
   g_ctx.worker_count = config_kb_connection_workers();

   /* #4-full render backend: register the command-driven computed-style render
    * adapter when css_render_command is configured (no-op otherwise — the oracle
    * then reports UNAVAILABLE rather than guessing). */
   css_render_cmd_register();

   int http_port = http_port_override >= 0 ? http_port_override : config_kb_api_http_port();
   if (http_port <= 0)
   {
      kb_service_shutdown(&g_ctx);
      kb_vault_operator_service_stop(vault_operator_service);
      vault_operator_service = NULL;
      kb_vault_operator_components_destroy(&vault_operator_components);
      if (vault_operator_runtime_opened)
         db2_vault_operator_runtime_close(&vault_operator_runtime);
      db2_shutdown();
      kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
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
   /* P9a: register the /v1/metrics + /v1/telemetry/metrics scrape/ingest token
    * (config telemetry.metrics_token, a SHA-256 hex) before the listener accepts. */
   kb_http_set_telemetry_token(config_telemetry_metrics_token());
   if (vault_tpm_runtime_lock &&
       kb_vault_tpm_runtime_lock_revalidate(vault_tpm_runtime_lock) != KB_VAULT_TPM_RUNTIME_LOCK_OK)
   {
      fputs("aimee-kb: TPM runtime singleton lost before listener activation\n", stderr);
      kb_service_shutdown(&g_ctx);
      kb_vault_operator_service_stop(vault_operator_service);
      vault_operator_service = NULL;
      kb_vault_operator_components_destroy(&vault_operator_components);
      if (vault_operator_runtime_opened)
         db2_vault_operator_runtime_close(&vault_operator_runtime);
      db2_shutdown();
      kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
      agent_http_cleanup();
      return 1;
   }
   if (kb_management_runtime_start() != 0)
   {
      fprintf(stderr, "aimee-kb: invalid management runtime configuration; refusing to start\n");
      kb_service_shutdown(&g_ctx);
      kb_vault_operator_service_stop(vault_operator_service);
      vault_operator_service = NULL;
      kb_vault_operator_components_destroy(&vault_operator_components);
      if (vault_operator_runtime_opened)
         db2_vault_operator_runtime_close(&vault_operator_runtime);
      db2_shutdown();
      kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
      agent_http_cleanup();
      return 1;
   }
   if (obs_bus_start() != 0)
   {
      fputs("aimee-kb: module bus failed to start\n", stderr);
      kb_management_runtime_stop();
      kb_service_shutdown(&g_ctx);
      kb_vault_operator_service_stop(vault_operator_service);
      vault_operator_service = NULL;
      kb_vault_operator_components_destroy(&vault_operator_components);
      if (vault_operator_runtime_opened)
         db2_vault_operator_runtime_close(&vault_operator_runtime);
      db2_shutdown();
      kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
      audit_log_close();
      agent_http_cleanup();
      return 1;
   }
   if (kb_http_start(http_port, config_kb_api_bearer_token()) != 0)
   {
      /* Another instance owns the port; yield gracefully with success so
       * systemd (Restart=on-failure) doesn't restart-loop. */
      LOG_WARN("kb_http",
               "failed to start HTTP listener on port %d; another instance likely owns it",
               http_port);
      kb_management_runtime_stop();
      kb_service_shutdown(&g_ctx);
      kb_vault_operator_service_stop(vault_operator_service);
      vault_operator_service = NULL;
      kb_vault_operator_components_destroy(&vault_operator_components);
      if (vault_operator_runtime_opened)
         db2_vault_operator_runtime_close(&vault_operator_runtime);
      db2_shutdown();
      kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
      obs_bus_stop();
      audit_log_close();
      agent_http_cleanup();
      return 0;
   }

   /* Now that this instance owns the port, boot the MCP plugins this kb HOSTS
    * (config install: kb) so their tools are live for the first federated
    * tools/list. Boot filters by install target — a no-op when none are
    * configured. Each plugin is OSV-scanned at startup (same gate as the server
    * path; see kb_mcp_osv_stub.c). Torn down after kb_http_stop() below. */
   (void)mcp_client_registry_boot(CONFIG_MCP_INSTALL_KB);

   /* Synthesis sidecar identities. Issued here because the deployment order is
    * server, wizard, kb, then the sidecar: minting at kb startup means the material
    * exists before anything can ask for it, with no extra route and no ordering to
    * coordinate. Idempotent, and only when a sidecar host is configured -- an
    * external or absent synthesis provider needs none of this. */
   {
      const char *llm_host = getenv("AIMEE_LLM_HOST");
      if (llm_host && llm_host[0])
      {
         if (kb_synthesis_identity_ensure(kb_default_config_dir(), llm_host) != 0)
            LOG_WARN("kb_sidecar_identity",
                     "could not provision synthesis mTLS identities for %s; the sidecar "
                     "will refuse to start until this succeeds",
                     llm_host);
      }
   }

   /* Embedder sidecar identities, on the same terms and for the same reason. Keyed on
    * its own env var rather than on EMBEDDER_MODEL: the model name says WHAT to embed
    * with, which is equally satisfied by an external endpoint over plain HTTPS, while
    * this says a sidecar container exists on the aimee network to issue for. */
   {
      const char *embedder_host = getenv("AIMEE_EMBEDDER_HOST");
      if (embedder_host && embedder_host[0])
      {
         if (kb_embedder_identity_ensure(kb_default_config_dir(), embedder_host) != 0)
            LOG_WARN("kb_sidecar_identity",
                     "could not provision embedder mTLS identities for %s; the sidecar "
                     "will refuse to start until this succeeds",
                     embedder_host);
      }
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
   /* P2b recovery owns stale dispatch leases; bounded batches keep startup and
    * periodic work predictable while the SQL advisory lock makes it singleton. */
   {
      int64_t recovered = 0;
      do
      {
         recovered = 0;
      } while (db2_org_egress_recover(100, &recovered) == 0 && recovered == 100);
   }
   db2_lease_release_idle();
   time_t next_egress_recovery = time(NULL) + 5;
   /* HTTP listener runs on its own thread; block here until a signal
    * (SIGINT/SIGTERM/SIGHUP) flips running, then tear down. */
   while (g_ctx.running)
   {
      time_t now = time(NULL);
      if (now >= next_egress_recovery)
      {
         int64_t recovered = 0;
         (void)db2_org_egress_recover(100, &recovered);
         next_egress_recovery = now + 5;
      }
      kb_management_runtime_tick((int64_t)now);
      kb_witness_cadence_tick(now); /* P7-witness-e2: periodic checkpoint cadence */
      /* Main-thread maintenance leases lazily from the DB2 pool. Return the
       * lease before sleeping so the daemon does not pin one member forever. */
      db2_lease_release_idle();
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 200L * 1000 * 1000};
      nanosleep(&ts, NULL);
   }
   int rc = 0;
   kb_mtls_stop();
   kb_http_stop();
   mcp_client_registry_shutdown(); /* stop kb-hosted MCP plugins (install: kb) */
   obs_bus_stop();
   kb_management_runtime_stop();
   kb_service_shutdown(&g_ctx);
   kb_vault_operator_service_stop(vault_operator_service);
   vault_operator_service = NULL;
   kb_vault_operator_components_destroy(&vault_operator_components);
   if (vault_operator_runtime_opened)
      db2_vault_operator_runtime_close(&vault_operator_runtime);
   (void)shutdown_forensics_mark_stopped("kb", getpid());
   embedder_probe_unregister(); /* §2b: deregister the probe before db2_shutdown */
   db2_shutdown();
   kb_vault_tpm_runtime_lock_release(&vault_tpm_runtime_lock);
   audit_log_close();
   agent_http_cleanup();
   return rc == 0 ? 0 : 1;
}
