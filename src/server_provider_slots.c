/* server_provider_slots.c: split from server.c into a real translation unit
 * (was server_provider_slots.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_internal.h"
#include "aimee.h"
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

/* Find or insert a slot entry for agent_name. Must be called with
 * provider_slots_mutex held. Returns the entry index, or -1 if the table
 * is full. */
static int provider_slot_find_or_add_locked(server_ctx_t *ctx, const char *agent_name)
{
   for (int i = 0; i < ctx->provider_slot_count; i++)
      if (strcmp(ctx->provider_slot_names[i], agent_name) == 0)
         return i;
   if (ctx->provider_slot_count >= CATALOG_MAX_ENTRIES)
      return -1;
   int i = ctx->provider_slot_count++;
   snprintf(ctx->provider_slot_names[i], sizeof(ctx->provider_slot_names[i]), "%s", agent_name);
   ctx->provider_slot_active[i] = 0;
   return i;
}

/* Reclaim expired slot holders for entry `idx` (caller holds the mutex). A holder
 * whose acquire is older than PROVIDER_SLOT_TTL_SEC is assumed dead (the request
 * that took it was killed / dropped / torn down without ever calling release);
 * without this the count would stay pinned until the next server restart. The
 * live stamps are kept compacted in [0..active-1], sorted oldest-first, so the
 * TTL check can stop at the first live one. Returns the number reaped. */
static int provider_slot_reap_expired_locked(server_ctx_t *ctx, int idx, time_t now)
{
   int before = ctx->provider_slot_active[idx];
   int after = provider_slots_reap_stamps(ctx->provider_slot_stamps[idx], before, now,
                                          PROVIDER_SLOT_TTL_SEC);
   ctx->provider_slot_active[idx] = after;
   if (after < before)
      aimee_log(LOG_WARN, "provider_slots",
                "reclaimed %d stale slot(s) for '%s' (held > %ds without release)", before - after,
                ctx->provider_slot_names[idx], PROVIDER_SLOT_TTL_SEC);
   return before - after;
}

/* provider.slot_acquire {"agent_name":"...", "max_parallel":N}
 * → {"status":"ok","acquired":true}  or  {"status":"ok","acquired":false} */
int handle_provider_slot_acquire(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(req, "agent_name");
   cJSON *jmax = cJSON_GetObjectItemCaseSensitive(req, "max_parallel");
   if (!cJSON_IsString(jname) || !jname->valuestring[0])
      return server_send_error(conn, "agent_name required", NULL);
   int max_parallel = cJSON_IsNumber(jmax) ? jmax->valueint : 0;
   if (max_parallel <= 0)
   {
      /* unlimited — always grant */
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddBoolToObject(resp, "acquired", 1);
      cJSON_AddNumberToObject(resp, "slot_id", 0);
      int rc = server_send_response(conn, resp);
      cJSON_Delete(resp);
      return rc;
   }

   pthread_mutex_lock(&ctx->provider_slots_mutex);
   int idx = provider_slot_find_or_add_locked(ctx, jname->valuestring);
   int acquired = 0;
   if (idx >= 0)
   {
      time_t now = time(NULL);
      /* self-heal: drop any holder that never released within the TTL */
      provider_slot_reap_expired_locked(ctx, idx, now);
      int cap = max_parallel < PROVIDER_SLOT_CAP ? max_parallel : PROVIDER_SLOT_CAP;
      if (ctx->provider_slot_active[idx] < cap)
      {
         ctx->provider_slot_stamps[idx][ctx->provider_slot_active[idx]] = now;
         ctx->provider_slot_active[idx]++;
         acquired = 1;
      }
   }
   pthread_mutex_unlock(&ctx->provider_slots_mutex);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddBoolToObject(resp, "acquired", acquired);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* provider.slot_release {"agent_name":"..."} */
int handle_provider_slot_release(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(req, "agent_name");
   if (cJSON_IsString(jname) && jname->valuestring[0])
   {
      pthread_mutex_lock(&ctx->provider_slots_mutex);
      for (int i = 0; i < ctx->provider_slot_count; i++)
      {
         if (strcmp(ctx->provider_slot_names[i], jname->valuestring) == 0)
         {
            /* opportunistic reap, then drop the oldest live holder (release is
             * by-name, so FIFO — keeps stamps sorted oldest-first for the TTL). */
            provider_slot_reap_expired_locked(ctx, i, time(NULL));
            if (ctx->provider_slot_active[i] > 0)
            {
               int n = --ctx->provider_slot_active[i];
               for (int k = 0; k < n; k++)
                  ctx->provider_slot_stamps[i][k] = ctx->provider_slot_stamps[i][k + 1];
            }
            break;
         }
      }
      pthread_mutex_unlock(&ctx->provider_slots_mutex);
   }
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}
