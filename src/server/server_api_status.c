/* server_api_status.c: split from server.c into a real translation unit
 * (was server_api_status.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_internal.h"
#include "aimee.h"
#include "harness_memory.h"        /* hmem_upsert (server owns DB1) */
#include "harness_memory_audit.h"  /* hmem_audit */
#include "harness_memory_common.h" /* hmem_resolve_project / hmem_project_key_ok */
#include "harness_memory_scope.h"  /* hmem_scope_for_client */
#include "json_fluent.h"           /* jo_ok */
#include "memory_redirect.h"       /* memory_redirect_classify / _bash_targets / _rematerialize */
#include "server.h"
#include "turn_registry.h"
#include "server_http.h" /* server_http_api_status_report */
#include "config.h"      /* config_t / config_load for api.status, api.enable */
#include "delegate_backend_docker.h"
#include "delegate_backend_local.h"
#include "delegate_backend_ssh.h"
#include "server_delegate_monitor.h"
#include "server_coord_dispatcher.h"
#include "server_skill.h"
#include "server_compute_impl.h"
#include "skill_review.h"
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
#include "delegate_credentials.h"
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
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

/* api.status: report the aimee.api.* loopback /v1 listener config and emit
 * VS Code / OpenAI-compatible model-provider setup snippets. Read-only; the
 * bearer secret is never returned (only whether one is configured). The CLI
 * (`aimee api status`) prints the `report` field. */
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
   cJSON_AddNumberToObject(resp, "rate_limit_per_min", rate_limit);
   cJSON_AddStringToObject(resp, "report", report);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

#define API_DEFAULT_PORT       8910
#define API_DEFAULT_RATE_LIMIT 60

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
static int server_api_mint_bearer(config_t *cfg)
{
   return platform_random_hex(cfg->server_api_bearer_token, 64) == 0 ? 0 : -1;
}

/* api.enable: turn on the loopback /v1 listener. Picks a default port and rate
 * limit when unset, mints a bearer token if none is configured, persists the
 * aimee.api.* block, and returns a report that reveals the token once (the
 * caller has CAP_SESSION_ADMIN over the trusted local socket). The new config
 * takes effect on the next server restart. */
int handle_api_enable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   config_t cfg;
   config_load(&cfg);

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
      if (server_api_mint_bearer(&cfg) != 0)
         return handle_api_error(conn, "failed to generate bearer token");
      generated = 1;
   }

   if (config_save(&cfg) != 0)
      return handle_api_error(conn, "failed to persist aimee.api config");

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

   config_t cfg;
   config_load(&cfg);
   if (server_api_mint_bearer(&cfg) != 0)
      return handle_api_error(conn, "failed to generate bearer token");
   if (config_save(&cfg) != 0)
      return handle_api_error(conn, "failed to persist aimee.api config");

   /* Hot-swap the running listener's bearer. This handler runs on the single
    * listener thread that also reads the bearer for authorization, so the swap
    * is serialized against auth checks and needs no lock. After this returns the
    * bootstrap token no longer authorizes anything. */
   server_http_set_bearer(cfg.server_api_bearer_token);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "bearer_token", cfg.server_api_bearer_token);
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

   config_t cfg;
   config_load(&cfg);
   cfg.server_api_http_port = 0;
   if (config_save(&cfg) != 0)
      return handle_api_error(conn, "failed to persist aimee.api config");

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
