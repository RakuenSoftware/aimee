/* server_api_status.c: split from server.c into a real translation unit
 * (was server_api_status.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_internal.h"
#include "aimee.h"
#include "harness_memory_audit.h"  /* hmem_audit */
#include "harness_memory_common.h" /* hmem_resolve_project / hmem_project_key_ok */
#include "harness_memory_scope.h"  /* hmem_scope_for_client */
#include "kb_client.h"             /* kb_client_health */
#include "json_fluent.h"           /* jo_ok */
#include "memory_redirect.h"       /* memory_redirect_classify / _bash_targets / _rematerialize */
#include "runtime_secret.h"
#include "vault_config_bootstrap.h"
#include "server.h"
#include "turn_registry.h"
#include "server_http.h" /* server_http_api_status_report */
#include "config.h"      /* config_t / config_load for api.status, api.enable */
#include <aimee/delegates/delegate_backend_docker.h>
#include <aimee/delegates/delegate_backend_local.h>
#include <aimee/delegates/delegate_backend_ssh.h>
#include "server_delegate_monitor.h"
#include "server_coord_dispatcher.h"
#include "server_skill.h"
#include "server_compute_impl.h"
#include <aimee/skills/skill_review.h>
#include "trigger_scheduler.h"
#include "wfe_live_delegate.h"
#include "wfe_scheduler.h"
#include "server_trigger.h"
#include "server_cron.h"
#include "server_pipeline.h" /* roundtable authoring pipeline (pipeline.*) */
#include "commands.h"
#include "agent.h"
#include "agent_exec.h"     /* agent_audit_async_flush — drain audit queue at shutdown */
#include "webuser_editor.h" /* webuser_editor_shutdown — reap editors at shutdown (WP-I) */
#include "agent_config.h"
#include "provider_catalog.h"
#include <aimee/delegates/delegate_credentials.h>
#include "model_registry.h"
#include "model_provider.h"
#include "model_registry.h"
#include "db1.h"
#include "token_audit.h"
#include "dashboard.h"
#include "log.h"
#include "hud.h"
#include "platform_event.h"
#include "platform_ipc.h"
#include "platform_path.h"
#include "platform_process.h"
#include "util.h"
#include "workspace.h"
#include "worktree_gc.h"
#include "git_verify.h"
#include "toolset.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

/* api.status: report the aimee.api.* loopback /v1 listener config and emit
 * VS Code / OpenAI-compatible model-provider setup snippets. Read-only; the
 * bearer secret is never returned (only whether one is configured). The CLI
 * (`aimee api status`) prints the `report` field. */

/* Attach the knowledge-base block to a server.health response.
 *
 * Lives here rather than in server.c only because that file is at its line-check
 * ceiling; this TU exists for exactly that reason.
 *
 * NOT the container healthcheck: that polls /v1/health (rh_health), a pure
 * liveness answer with no downstream dependency. This is the dispatch route
 * /v1/server/health, reached only when a human asks "is my install healthy" — and
 * the kb is the part most likely to be broken while everything around it looks
 * fine. The probe is bounded by kb_client_health's CLIENT_DEFAULT_TIMEOUT_MS, and
 * an unreachable kb is REPORTED rather than failing the call: a broken kb must
 * still let you run the command that tells you the kb is broken.
 *
 * Field names are deliberately tier-neutral (store_ok / vectors_ok, not db2_ok /
 * pgvec_ok): these strings land in the thin client, which must not carry the
 * storage tier's vocabulary — build-integrity greps the client binary for exactly
 * that and fails the build. The client reports whether the kb's store and vector
 * index are healthy without knowing what implements them. */
void server_health_add_kb(cJSON *resp)
{
   if (!resp)
      return;
   kb_health_t kb;
   memset(&kb, 0, sizeof(kb));
   int kb_rc = kb_client_health(&kb);
   cJSON *kbo = cJSON_AddObjectToObject(resp, "kb");
   if (!kbo)
      return;
   int reachable = (kb_rc == 0 && kb.process_ok);
   cJSON_AddStringToObject(kbo, "status", reachable ? "ok" : "unreachable");
   if (!reachable)
      return;
   cJSON_AddBoolToObject(kbo, "store_ok", kb.db2_ok ? 1 : 0);
   cJSON_AddBoolToObject(kbo, "vectors_ok", kb.pgvec_ok ? 1 : 0);
   cJSON_AddBoolToObject(kbo, "embed_configured", kb.embed_ok ? 1 : 0);
   cJSON_AddNumberToObject(kbo, "vectors", kb.pgvec_vectors);
   if (kb.version[0])
      cJSON_AddStringToObject(kbo, "version", kb.version);
}

int handle_api_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   config_t cfg;
   config_load(&cfg);
   int http_port = cfg.server_api_http_port;
   int bearer_configured = cfg.server_api_bearer_token[0] != '\0';
   int rate_limit = cfg.server_api_rate_limit_per_min;

   char report[2048];
   server_http_api_status_report(http_port, bearer_configured, rate_limit, report, sizeof(report));

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "enabled", http_port > 0);
   cJSON_AddNumberToObject(resp, "http_port", http_port);
   cJSON_AddBoolToObject(resp, "bearer_configured", bearer_configured);
   cJSON_AddNumberToObject(resp, "enrolled_bearer_count", server_http_enrolled_bearer_count());
   cJSON_AddNumberToObject(resp, "rate_limit_per_min", rate_limit);
   cJSON_AddStringToObject(resp, "report", report);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

#define API_DEFAULT_PORT       8910
#define API_DEFAULT_RATE_LIMIT 60

/* /v1 connections are handled concurrently. Serialize every aimee.api
 * read-modify-save sequence in this file: otherwise two enrollments can select
 * the same slot, or enable/disable can overwrite a just-enrolled credential.
 * Each mutation also reads DISK rather than config_load's immutable live
 * snapshot. The snapshot is intentionally stable between reloads; using it for
 * two back-to-back enrollments made the second overwrite the first on disk and
 * in the live auth set. */
static pthread_mutex_t g_api_bearer_mutation_lock = PTHREAD_MUTEX_INITIALIZER;

static int handle_api_error(server_conn_t *conn, const char *message)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "error");
   cJSON_AddStringToObject(resp, "message", message);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* Mint a fresh 256-bit (64 hex-char) /v1 bearer into cfg->server_api_bearer_token.
 * Returns 0 on success, -1 if the RNG failed. Shared by api.enable (mint-if-empty)
 * and api.rotate_bearer (mint-unconditionally). */
static int server_api_mint_bearer(char *out, size_t out_cap)
{
   return out && out_cap >= 65 && platform_random_hex(out, 64) == 0 ? 0 : -1;
}

static int server_api_load_mutable(config_t *cfg)
{
   if (!cfg || config_load_file(cfg) != 0)
      return -1;
   (void)runtime_secret_get("AIMEE_API_BEARER_TOKEN", cfg->server_api_bearer_token,
                            sizeof(cfg->server_api_bearer_token));
   cfg->server_api_bearer_extra_count = 0;
   for (int i = 0; i < AIMEE_API_BEARER_EXTRA_MAX; i++)
   {
      char name[96];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", i);
      if (!runtime_secret_get(name, cfg->server_api_bearer_extra[i],
                              sizeof(cfg->server_api_bearer_extra[i])))
         break;
      cfg->server_api_bearer_extra_count++;
   }
   return 0;
}

/* api.enable: turn on the loopback /v1 listener. Picks a default port and rate
 * limit when unset, mints a bearer token if none is configured, persists the
 * aimee.api.* block, and returns a report that reveals the token once (the
 * caller has CAP_SESSION_ADMIN over the trusted local socket). The new config
 * takes effect on the next server restart. */
int handle_api_enable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   pthread_mutex_lock(&g_api_bearer_mutation_lock);
   config_t cfg;
   if (server_api_load_mutable(&cfg) != 0)
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to load aimee.api config");
   }

   cJSON *jport = cJSON_GetObjectItemCaseSensitive(req, "port");
   if (cJSON_IsNumber(jport) && jport->valuedouble > 0)
      cfg.server_api_http_port = (int)jport->valuedouble;
   cJSON *jrate = cJSON_GetObjectItemCaseSensitive(req, "rate_limit");
   if (cJSON_IsNumber(jrate) && jrate->valuedouble > 0)
      cfg.server_api_rate_limit_per_min = (int)jrate->valuedouble;
   int with_vscode = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "vscode"));

   if (cfg.server_api_http_port <= 0)
      cfg.server_api_http_port = API_DEFAULT_PORT;
   if (cfg.server_api_rate_limit_per_min <= 0)
      cfg.server_api_rate_limit_per_min = API_DEFAULT_RATE_LIMIT;

   int generated = 0;
   if (cfg.server_api_bearer_token[0] == '\0')
   {
      if (server_api_mint_bearer(cfg.server_api_bearer_token,
                                 sizeof(cfg.server_api_bearer_token)) != 0 ||
          vault_runtime_secret_set("AIMEE_API_BEARER_TOKEN",
                                   cfg.server_api_bearer_token) != 0)
      {
         pthread_mutex_unlock(&g_api_bearer_mutation_lock);
         return handle_api_error(conn, "failed to generate bearer token");
      }
      generated = 1;
   }

   if (config_save(&cfg) != 0)
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to persist aimee.api config");
   }
   (void)config_reload();
   pthread_mutex_unlock(&g_api_bearer_mutation_lock);

   char snippets[2048];
   server_http_api_status_report(cfg.server_api_http_port, 1, cfg.server_api_rate_limit_per_min,
                                 snippets, sizeof(snippets));

   char report[4096];
   int off = snprintf(report, sizeof(report),
                      "aimee /v1 HTTP API enabled on http://127.0.0.1:%d/v1\n"
                      "  bearer token: %s%s\n"
                      "  rate limit:   %d req/min\n"
                      "\nRestart the server to apply: aimee server restart\n\n%s",
                      cfg.server_api_http_port, cfg.server_api_bearer_token,
                      generated ? "   (newly generated — store it now)" : "",
                      cfg.server_api_rate_limit_per_min, snippets);
   if (with_vscode && off > 0 && (size_t)off < sizeof(report))
      snprintf(report + off, sizeof(report) - (size_t)off,
               "\nVS Code aimee extension (Settings -> Extensions -> aimee):\n"
               "  aimee.apiBase     = http://127.0.0.1:%d/v1\n"
               "  aimee.bearerToken = %s\n"
               "  aimee.model       = aimee\n",
               cfg.server_api_http_port, cfg.server_api_bearer_token);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "enabled", 1);
   cJSON_AddNumberToObject(resp, "http_port", cfg.server_api_http_port);
   cJSON_AddStringToObject(resp, "bearer_token", cfg.server_api_bearer_token);
   cJSON_AddNumberToObject(resp, "rate_limit_per_min", cfg.server_api_rate_limit_per_min);
   cJSON_AddBoolToObject(resp, "vscode", with_vscode);
   cJSON_AddStringToObject(resp, "report", report);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* api.rotate_bearer: mint a FRESH /v1 bearer unconditionally (replacing any
 * existing one — e.g. the image-seeded `aimee-local-dev` bootstrap), persist it,
 * and HOT-SWAP the live listener so the new token authorizes immediately and the
 * old one stops working at once — no restart, no dual-validity window. This is
 * the server side of trust-on-first-use enrollment: a thin client connects once
 * with the bootstrap bearer and calls this to obtain its strong per-deployment
 * token, which it then uses exclusively. CAP_SESSION_ADMIN-gated (same as
 * api.enable); reveals the new token once to the authorized caller. */
int handle_api_rotate_bearer(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   pthread_mutex_lock(&g_api_bearer_mutation_lock);
   config_t cfg;
   if (server_api_load_mutable(&cfg) != 0)
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to load aimee.api config");
   }
   if (server_api_mint_bearer(cfg.server_api_bearer_token,
                              sizeof(cfg.server_api_bearer_token)) != 0 ||
       vault_runtime_secret_set("AIMEE_API_BEARER_TOKEN", cfg.server_api_bearer_token) != 0)
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to generate bearer token");
   }
   /* Rotation is the explicit revoke-all operation. Persist that revocation;
    * otherwise the additional credentials would return after a restart. */
   for (int i = 0; i < AIMEE_API_BEARER_EXTRA_MAX; i++)
   {
      char name[96];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", i);
      if (vault_runtime_secret_delete(name) != 0)
      {
         pthread_mutex_unlock(&g_api_bearer_mutation_lock);
         return handle_api_error(conn, "failed to revoke enrolled bearer from Vault");
      }
   }
   memset(cfg.server_api_bearer_extra, 0, sizeof(cfg.server_api_bearer_extra));
   cfg.server_api_bearer_extra_count = 0;
   if (config_save(&cfg) != 0)
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to persist aimee.api config");
   }
   (void)config_reload();

   /* Hot-swap the running listener's primary and atomically clear its enrolled
    * set. After this returns none of the previous credentials authorize. */
   server_http_set_bearer(cfg.server_api_bearer_token);
   pthread_mutex_unlock(&g_api_bearer_mutation_lock);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "bearer_token", cfg.server_api_bearer_token);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* api.enroll_bearer: mint a fresh bearer and ADD it to the accepted set, leaving
 * the primary and every previously-enrolled token working.
 *
 * This exists because pairing a client used to be done with rotate_bearer, which
 * replaces the single global bearer — so the second client to enrol silently
 * evicted the first, and every already-paired client began failing at the same
 * instant. Pairing is additive; revoking is rotate_bearer's job and stays
 * separate, explicit, and unchanged.
 *
 * Fails closed at the cap rather than evicting the oldest: silently dropping a
 * credential someone is still using is the exact failure this replaces.
 * CAP_SESSION_ADMIN-gated, like api.enable and api.rotate_bearer. */
int handle_api_enroll_bearer(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   pthread_mutex_lock(&g_api_bearer_mutation_lock);
   config_t cfg;
   if (server_api_load_mutable(&cfg) != 0)
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to load aimee.api config");
   }

   if (cfg.server_api_bearer_token[0] == '\0')
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "no primary bearer configured; run api.enable first");
   }
   if (cfg.server_api_bearer_extra_count >= AIMEE_API_BEARER_EXTRA_MAX)
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "enrolled bearer limit reached; perform an explicit revoke-all "
                                    "with api.rotate_bearer before enrolling another client");
   }

   char minted[256];
   if (platform_random_hex(minted, 64) != 0)
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to generate bearer token");
   }

   int slot = cfg.server_api_bearer_extra_count;
   char vault_name[96];
   snprintf(vault_name, sizeof(vault_name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", slot);
   if (vault_runtime_secret_set(vault_name, minted) != 0)
   {
      runtime_secret_wipe(minted, sizeof(minted));
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to store enrolled bearer in Vault");
   }
   snprintf(cfg.server_api_bearer_extra[slot], sizeof(cfg.server_api_bearer_extra[0]), "%s",
            minted);
   cfg.server_api_bearer_extra_count = slot + 1;

   if (config_save(&cfg) != 0)
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to persist aimee.api config");
   }
   (void)config_reload();

   /* Hot-swap so the new token authorizes immediately. The live auth layer has
    * its own lock for concurrent connection workers. */
   const char *extra[AIMEE_API_BEARER_EXTRA_MAX];
   for (int i = 0; i < cfg.server_api_bearer_extra_count; i++)
      extra[i] = cfg.server_api_bearer_extra[i];
   server_http_set_bearer_extra(extra, cfg.server_api_bearer_extra_count);
   pthread_mutex_unlock(&g_api_bearer_mutation_lock);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "bearer_token", minted);
   cJSON_AddNumberToObject(resp, "enrolled_count", cfg.server_api_bearer_extra_count);
   cJSON_AddNumberToObject(resp, "enrolled_max", AIMEE_API_BEARER_EXTRA_MAX);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* api.disable: turn the loopback /v1 listener off (clears aimee.api.http_port).
 * The bearer token and rate limit are left in place so a later `enable` reuses
 * them. Takes effect on the next server restart. */
int handle_api_disable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   pthread_mutex_lock(&g_api_bearer_mutation_lock);
   config_t cfg;
   if (config_load_file(&cfg) != 0)
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to load aimee.api config");
   }
   cfg.server_api_http_port = 0;
   if (config_save(&cfg) != 0)
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to persist aimee.api config");
   }
   (void)config_reload();
   pthread_mutex_unlock(&g_api_bearer_mutation_lock);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "enabled", 0);
   cJSON_AddStringToObject(resp, "report",
                           "aimee /v1 HTTP API disabled (loopback listener off).\n"
                           "Restart the server to apply: aimee server restart\n");
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}
