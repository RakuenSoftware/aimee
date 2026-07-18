/* server_compute_episodes.c: split from server_compute.c into a real translation unit
 * (was server_compute_episodes.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_compute_internal.h"
#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "db1.h"
#include "server_delegate_monitor.h" /* delegate heartbeat begin/end (keep slow delegates alive) */
#include "server_compute_impl.h"
#include "agent_config.h"
#include "gateway_policy.h"
#include "presence.h"
#include "compute_pool.h"
#include "agent.h"
#include "agent_coord.h"
#include "cmd_agent_delegate_impl.h"
#include "config.h"
#include "token_tracker.h"
#include "delegate_credential_retry.h"
#include "delegate_launch.h"
#include "delegate_source_authority.h"
#include "agent_source_authority.h" /* TLS source-authority context (race-free in-process) */
#include "server_coord_dispatcher.h"
#include "delegate_credentials.h"
#include "vault_service.h" /* WP-C.1 vault-first credential resolution */
#include <openssl/crypto.h>
#include "delegate_economics.h"
#include "delegate_run_phases.h"
#include "db1/delegate_learning.h"
#include "kb_client.h"
#include "kb_bandit.h"
#include "db1/interaction_events.h"
#include "delegate_role.h"
#include "delegate_ensemble.h"
#include "evidence_replay.h"
#include "guardrails.h"
#include "liveness.h"
#include "log.h"
#include "model_registry.h"
#include "openai_runs_store.h"
#include "platform_process.h"
#include "prompts.h"
#include "persona.h"
#include "server_http.h"
#include "provider_catalog.h"
#include "role_templates.h"
#include "workspace.h"
#include "workspace_provider.h"
#include "workspace_turn.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EPISODE_LIST_MAX 64

static cJSON *agent_log_entries_to_json(const char *agent_filter, int limit)
{
   db1_agent_log_entry_t rows[EPISODE_LIST_MAX];
   int n =
       db1_agent_log_list(agent_filter, rows, limit < EPISODE_LIST_MAX ? limit : EPISODE_LIST_MAX);
   cJSON *episodes = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *ep = cJSON_CreateObject();
      cJSON_AddStringToObject(ep, "agent", rows[i].agent_name);
      cJSON_AddStringToObject(ep, "role", rows[i].role);
      cJSON_AddNumberToObject(ep, "turns", rows[i].turns);
      cJSON_AddNumberToObject(ep, "tool_calls", rows[i].tool_calls);
      cJSON_AddBoolToObject(ep, "success", rows[i].success);
      cJSON_AddNumberToObject(ep, "confidence", rows[i].confidence);
      cJSON_AddNumberToObject(ep, "prompt_tokens", rows[i].prompt_tokens);
      cJSON_AddNumberToObject(ep, "completion_tokens", rows[i].completion_tokens);
      cJSON_AddNumberToObject(ep, "latency_ms", rows[i].latency_ms);
      cJSON_AddStringToObject(ep, "created_at", rows[i].created_at);
      cJSON_AddItemToArray(episodes, ep);
   }
   return episodes;
}

int handle_delegate_log(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jlimit = cJSON_GetObjectItemCaseSensitive(req, "limit");
   int limit = cJSON_IsNumber(jlimit) ? (int)jlimit->valuedouble : 20;
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "episodes", agent_log_entries_to_json(NULL, limit));
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_episode_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   return handle_delegate_log(ctx, conn, req);
}

int handle_agent_episodes(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jlimit = cJSON_GetObjectItemCaseSensitive(req, "limit");
   int limit = cJSON_IsNumber(jlimit) ? (int)jlimit->valuedouble : 40;
   cJSON *jname = cJSON_GetObjectItemCaseSensitive(req, "agent");
   const char *filter_agent = cJSON_IsString(jname) ? jname->valuestring : NULL;
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "episodes", agent_log_entries_to_json(filter_agent, limit));
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}
