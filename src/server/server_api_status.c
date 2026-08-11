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
#include "config.h"      /* config accessors + setters for api.status / api.enable */

/* Defined below; the enrolled-bearer picture is Vault state, not config. */
static int api_bearer_primary(char *out, size_t out_n);
static int api_bearer_extra_count(void);
#include <aimee/delegates/delegate_backend_docker.h>
#include "server_delegate_monitor.h"
#include "server_coord_dispatcher.h"
#include "server_skill.h"
#include "server_compute_impl.h"
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
#include <aimee/workspace/workspace.h>
#include "worktree_gc.h"
#include "modules/git/git_verify.h"
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
/* Split a newline-joined kb field back into a JSON array under `key`, omitting the
 * key entirely when there is nothing to say. Extracted when warnings joined
 * blockers: two copies of the same pointer arithmetic was how the warnings array
 * came to be dropped in the first place. */
static void kb_health_add_lines(cJSON *kbo, const char *key, const char *joined)
{
   if (!kbo || !key || !joined || !joined[0])
      return;
   cJSON *arr = cJSON_AddArrayToObject(kbo, key);
   if (!arr)
      return;
   for (const char *p = joined; p && *p;)
   {
      const char *nl = strchr(p, '\n');
      size_t len = nl ? (size_t)(nl - p) : strlen(p);
      char line[384];
      if (len >= sizeof(line))
         len = sizeof(line) - 1;
      memcpy(line, p, len);
      line[len] = '\0';
      cJSON_AddItemToArray(arr, cJSON_CreateString(line));
      p = nl ? nl + 1 : NULL;
   }
}

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

   /* `status` used to be exactly `reachable ? "ok" : "unreachable"` — "is the kb
    * process up", which is not the same question as "can I query it". Every
    * capability the kb reported arrived as a sibling field beside it, so this
    * object could say "ok" and "embed_configured": false in the same breath, and
    * `aimee status` printed "aimee-kb: ok" directly above "embedder: not
    * configured". The transport breaker hit the same wall and was answered by
    * adding ANOTHER sibling (queries_suppressed) for the CLI to special-case;
    * that is the gap reproducing rather than closing.
    *
    * Three states now, and they are ordered by what the operator can act on:
    *
    *   unreachable  nothing answered — the kb's own verdict does not exist
    *   degraded     something answered and told us it cannot work, OR the breaker
    *                is refusing every call locally before the kb is ever contacted
    *   ok           answered and claims capability
    *
    * The breaker is folded into the verdict rather than left as a parallel flag:
    * an open breaker means queries fail, which is the definition of degraded, and
    * leaving it beside `status` is what forced the bespoke CLI branch. */
   kb_client_dependency_health_t dep;
   kb_client_dependency_health(&dep);
   int breaker_open = strcmp(dep.state, "open") == 0;

   const char *status;
   if (!reachable)
      status = "unreachable";
   else if (breaker_open || strcmp(kb.status, "degraded") == 0)
      status = "degraded";
   else
      status = "ok";
   cJSON_AddStringToObject(kbo, "status", status);

   cJSON_AddStringToObject(kbo, "transport_state", dep.state);
   if (breaker_open)
   {
      cJSON_AddBoolToObject(kbo, "queries_suppressed", 1);
      cJSON_AddNumberToObject(kbo, "retry_after_ms", (double)dep.retry_after_ms);
      cJSON_AddNumberToObject(kbo, "suppressed_calls", (double)dep.suppressed_calls);
   }
   if (!reachable)
      return;
   /* Pass the reasons through verbatim. The kb composed them next to the evidence
    * and named the remedy; re-deriving them here from the booleans below would be
    * a second place for the verdict to drift out of step with the facts.
    *
    * WARNINGS TRAVEL TOO, not just blockers. The kb has always assembled a
    * warnings array and this block dropped it on the floor, so findings that do
    * not move the verdict reached no operator at all: a typed-fact backlog that
    * nothing will ever drain sat in /v1/health for hours while `aimee status`
    * showed a clean kb and `aimee kb status` printed a bare "4 pending". Publishing
    * a finding into a field no surface renders is the same defect as not computing
    * it -- the evidence exists and the summary does not carry it. */
   kb_health_add_lines(kbo, "blockers", kb.blockers);
   kb_health_add_lines(kbo, "warnings", kb.warnings);
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

   int http_port = config_server_api_http_port();
   /* Vault, not config: config_load scrubs the bearer fields after migrating any
    * legacy value out, so asking config would report an enabled API as unkeyed. */
   char probe[256];
   int bearer_configured = api_bearer_primary(probe, sizeof(probe));
   runtime_secret_wipe(probe, sizeof(probe));
   int rate_limit = config_server_api_rate_limit_per_min();

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

/* The enrolled-bearer picture is VAULT state, not config. config_save persists
 * neither the primary nor the extras, and config_load migrates any legacy values
 * out to Vault and scrubs them from the struct. These two helpers read that state
 * where it actually lives, instead of staging it through a config_t. */
static int api_bearer_primary(char *out, size_t out_n)
{
   if (out && out_n)
      out[0] = '\0';
   return runtime_secret_get("AIMEE_API_BEARER_TOKEN", out, out_n) ? 1 : 0;
}

/* Number of contiguous enrolled bearers. The Vault secret name is keyed on the
 * slot index, so a caller must know the count BEFORE minting the next one. */
static int api_bearer_extra_count(void)
{
   int n = 0;
   for (int i = 0; i < AIMEE_API_BEARER_EXTRA_MAX; i++)
   {
      char name[96], val[256];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", i);
      if (!runtime_secret_get(name, val, sizeof(val)))
      {
         runtime_secret_wipe(val, sizeof(val));
         break;
      }
      runtime_secret_wipe(val, sizeof(val));
      n++;
   }
   return n;
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

   /* Port/rate come from the request, else the current config, else the defaults. */
   cJSON *jport = cJSON_GetObjectItemCaseSensitive(req, "port");
   cJSON *jrate = cJSON_GetObjectItemCaseSensitive(req, "rate_limit");
   int port = (cJSON_IsNumber(jport) && jport->valuedouble > 0) ? (int)jport->valuedouble
                                                                : config_server_api_http_port();
   int rate = (cJSON_IsNumber(jrate) && jrate->valuedouble > 0)
                  ? (int)jrate->valuedouble
                  : config_server_api_rate_limit_per_min();
   int with_vscode = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "vscode"));
   if (port <= 0)
      port = API_DEFAULT_PORT;
   if (rate <= 0)
      rate = API_DEFAULT_RATE_LIMIT;

   /* Bearer presence is a VAULT question, not a config one -- asking config would
    * report "unset" for a perfectly good token and mint a second one over it. */
   char bearer[256];
   int generated = 0;
   if (!api_bearer_primary(bearer, sizeof(bearer)))
   {
      if (server_api_mint_bearer(bearer, sizeof(bearer)) != 0 ||
          vault_runtime_secret_set("AIMEE_API_BEARER_TOKEN", bearer) != 0)
      {
         runtime_secret_wipe(bearer, sizeof(bearer));
         pthread_mutex_unlock(&g_api_bearer_mutation_lock);
         return handle_api_error(conn, "failed to generate bearer token");
      }
      generated = 1;
   }

   if (config_set_api_http_listener(port, rate) != 0)
   {
      runtime_secret_wipe(bearer, sizeof(bearer));
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to persist aimee.api config");
   }
   pthread_mutex_unlock(&g_api_bearer_mutation_lock);

   char snippets[2048];
   server_http_api_status_report(port, 1, rate, snippets, sizeof(snippets));

   char report[4096];
   int off = snprintf(report, sizeof(report),
                      "aimee /v1 HTTP API enabled on http://127.0.0.1:%d/v1\n"
                      "  bearer token: %s%s\n"
                      "  rate limit:   %d req/min\n"
                      "\nRestart the server to apply: aimee server restart\n\n%s",
                      port, bearer, generated ? "   (newly generated — store it now)" : "", rate,
                      snippets);
   if (with_vscode && off > 0 && (size_t)off < sizeof(report))
      snprintf(report + off, sizeof(report) - (size_t)off,
               "\nVS Code aimee extension (Settings -> Extensions -> aimee):\n"
               "  aimee.apiBase     = http://127.0.0.1:%d/v1\n"
               "  aimee.bearerToken = %s\n"
               "  aimee.model       = aimee\n",
               port, bearer);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "enabled", 1);
   cJSON_AddNumberToObject(resp, "http_port", port);
   cJSON_AddStringToObject(resp, "bearer_token", bearer);
   cJSON_AddNumberToObject(resp, "rate_limit_per_min", rate);
   cJSON_AddBoolToObject(resp, "vscode", with_vscode);
   cJSON_AddStringToObject(resp, "report", report);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* api.rotate_bearer: mint a fresh /v1 primary, persist it in Vault, and HOT-SWAP
 * the live listener so the old primary and all additive enrollments stop working
 * at once. CAP_SESSION_ADMIN-gated (same as api.enable); reveals the replacement
 * once to the authorized caller. */
int handle_api_rotate_bearer(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   pthread_mutex_lock(&g_api_bearer_mutation_lock);
   char bearer[256];
   if (server_api_mint_bearer(bearer, sizeof(bearer)) != 0 ||
       vault_runtime_secret_set("AIMEE_API_BEARER_TOKEN", bearer) != 0)
   {
      runtime_secret_wipe(bearer, sizeof(bearer));
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to generate bearer token");
   }
   /* Rotation is the explicit revoke-all operation. Deleting the Vault secrets IS
    * the persistence: config_save never wrote the enrolled set (config_load
    * migrates any legacy values out to Vault and scrubs them), so the config
    * write this used to do persisted nothing. */
   for (int i = 0; i < AIMEE_API_BEARER_EXTRA_MAX; i++)
   {
      char name[96];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", i);
      if (vault_runtime_secret_delete(name) != 0)
      {
         runtime_secret_wipe(bearer, sizeof(bearer));
         pthread_mutex_unlock(&g_api_bearer_mutation_lock);
         return handle_api_error(conn, "failed to revoke enrolled bearer from Vault");
      }
   }

   /* Hot-swap the running listener's primary and atomically clear its enrolled
    * set. After this returns none of the previous credentials authorize. */
   server_http_set_bearer(bearer);
   pthread_mutex_unlock(&g_api_bearer_mutation_lock);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "bearer_token", bearer);
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
   /* Both the primary's presence and the enrolled count are VAULT state. Reading
    * either from config would report "unset" for a live credential -- the config
    * fields are legacy migration staging that config_load scrubs. */
   char primary[256];
   int have_primary = api_bearer_primary(primary, sizeof(primary));
   runtime_secret_wipe(primary, sizeof(primary));
   if (!have_primary)
   {
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "no primary bearer configured; run api.enable first");
   }
   int enrolled = api_bearer_extra_count();
   if (enrolled >= AIMEE_API_BEARER_EXTRA_MAX)
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

   int slot = enrolled;
   char vault_name[96];
   snprintf(vault_name, sizeof(vault_name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", slot);
   if (vault_runtime_secret_set(vault_name, minted) != 0)
   {
      runtime_secret_wipe(minted, sizeof(minted));
      pthread_mutex_unlock(&g_api_bearer_mutation_lock);
      return handle_api_error(conn, "failed to store enrolled bearer in Vault");
   }
   /* The Vault write above IS the persistence -- config_save never stored the
    * enrolled set. Re-read the whole set for the hot-swap so the live listener
    * reflects Vault exactly. */
   int now_enrolled = slot + 1;
   char extra_buf[AIMEE_API_BEARER_EXTRA_MAX][256];
   const char *extra[AIMEE_API_BEARER_EXTRA_MAX];
   int n_extra = 0;
   for (int i = 0; i < now_enrolled && i < AIMEE_API_BEARER_EXTRA_MAX; i++)
   {
      char name[96];
      snprintf(name, sizeof(name), "AIMEE_API_BEARER_TOKEN_EXTRA_%d", i);
      if (!runtime_secret_get(name, extra_buf[i], sizeof(extra_buf[i])))
         break;
      extra[n_extra++] = extra_buf[i];
   }
   server_http_set_bearer_extra(extra, n_extra);
   pthread_mutex_unlock(&g_api_bearer_mutation_lock);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "bearer_token", minted);
   cJSON_AddNumberToObject(resp, "enrolled_count", now_enrolled);
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
   if (config_disable_api_http_listener() != 0)
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
