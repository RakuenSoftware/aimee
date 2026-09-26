/* server_state_audit.c: split from server_state.c into a real translation unit
 * (was server_state_audit.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#include "server_state_internal.h"
#include "aimee.h"
#include "server.h"
#include "ingress_preinject.h"
#include "module_commands.h"
#include "request_context.h"
#include <aimee/audit/audit_worm.h>
#include "dashboard.h"
#include "lsp.h"
#include "platform_path.h"
#include <aimee/workspace/workspace.h>
#include "modules/workspace/workspace_mirror.h"
#include "modules/workspace/workspace_provider.h"
#include "modules/workspace/workspace_handle.h"
#include "modules/workspace/workspace_runner_registry.h"
#include "modules/git/forge_credentials.h"
#include "db1_client/db1.h"
#include "kb_client.h"
#include "compute_pool.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "dogfood.h"
#include "commands.h"
#include "platform_path.h"
#include "server_http.h"  /* session_primary_set/get/clear */
#include "agent_config.h" /* agent_load_config / agent_find */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* /v1/audit/trace handler. Its op (evidence.trace_retrieval_event) is a KB-only
 * action, so — like handle_kb_search and the curator handlers — the server
 * forwards it to aimee-kb via kb_client and passes the JSON response through.
 * rh_dispatch_op alone cannot reach a KB-only method (server_dispatch resolves
 * only server-side methods), which is why the cloned /v1/code/audit route never
 * reached the KB. */
int handle_evidence_trace(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *turn_id = jo_str(req, "turn_id", "");
   if (!turn_id[0])
      return server_send_error(conn, "audit trace requires turn_id", NULL);
   char *json = kb_client_evidence_trace_retrieval_event(turn_id);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service audit trace failed", NULL);
   return send_and_free(conn, resp);
}

/* /v1/audit/provenance handler. Like handle_evidence_trace, its op
 * (evidence.provenance_retrieval_event) is a KB-only action, so the server
 * forwards it to aimee-kb via kb_client and passes the JSON response through. */
int handle_evidence_provenance(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *turn_id = jo_str(req, "turn_id", "");
   if (!turn_id[0])
      return server_send_error(conn, "audit provenance requires turn_id", NULL);
   char *json = kb_client_evidence_provenance_retrieval_event(turn_id);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service audit provenance failed", NULL);
   return send_and_free(conn, resp);
}

/* /v1/audit/fidelity handler. Like the trace/provenance handlers, its op
 * (evidence.fidelity_retrieval_event) is a KB-only action (the fidelity_report /
 * fidelity_attribution artifacts live in DB2), so the server forwards it to
 * aimee-kb via kb_client and passes the JSON response through. */
int handle_evidence_fidelity(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *turn_id = jo_str(req, "turn_id", "");
   if (!turn_id[0])
      return server_send_error(conn, "audit fidelity requires turn_id", NULL);
   char *json = kb_client_evidence_fidelity_retrieval_event(turn_id);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service audit fidelity failed", NULL);
   return send_and_free(conn, resp);
}

/* Authenticated transport only; receipt interpretation stays in Go. */
int handle_memory_receipt(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *id = jo_str(req, "request_id", "");
   cJSON *response = ingress_preinject_receipt_options(
       id, 0, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "replay")));
   return response ? send_and_free(conn, response)
                   : server_send_error(
                         conn, "receipt ledger unavailable for this authenticated request", NULL);
}

int handle_memory_receipt_forget(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *response = ingress_preinject_receipt_options(jo_str(req, "request_id", ""), 1, 0);
   return response ? send_and_free(conn, response)
                   : server_send_error(conn, "receipt payload removal unavailable", NULL);
}

/* The public body cannot choose a principal, ledger or completeness assertion.
 * This adapter transports only a verified, principal-bound local snapshot. */
static cJSON *memory_health_command(cJSON *request)
{
   cJSON *response = NULL;
   int rc =
       aimee_module_commands_dispatch_internal_timeout("memory.runtime", request, 5000, &response);
   cJSON_Delete(request);
   if (rc != 1)
   {
      cJSON_Delete(response);
      return NULL;
   }
   return response;
}

int handle_memory_health(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const request_context_t *context = request_context_get();
   if (!context || !context->principal[0])
      return server_send_error(conn, "authenticated health scope required", NULL);
   cJSON *query = cJSON_CreateObject();
   cJSON_AddStringToObject(query, "health_principal", context->principal);
   const char *fields[] = {"window", "project", "workspace", "purpose", "query_class", "stage"};
   for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
   {
      const cJSON *v = cJSON_GetObjectItemCaseSensitive(req, fields[i]);
      if (v && !cJSON_IsString(v))
      {
         cJSON_Delete(query);
         return server_send_error(conn, "invalid health filter", NULL);
      }
      if (v)
         cJSON_AddItemToObject(query, fields[i], cJSON_Duplicate(v, 1));
   }
   const cJSON *traces = cJSON_GetObjectItemCaseSensitive(req, "traces");
   if (traces && !cJSON_IsBool(traces))
   {
      cJSON_Delete(query);
      return server_send_error(conn, "invalid trace selection", NULL);
   }
   cJSON_AddBoolToObject(query, "traces", cJSON_IsTrue(traces));
   cJSON_AddStringToObject(query, "operation", "health-plan");
   cJSON *plan = memory_health_command(cJSON_Duplicate(query, 1));
   if (!plan || strcmp(jo_str(plan, "status", ""), "ok"))
   {
      cJSON_Delete(query);
      return plan ? send_and_free(conn, plan)
                  : server_send_error(conn, "health owner unavailable", NULL);
   }
   const char *from = jo_str(plan, "from", ""), *until = jo_str(plan, "until", "");
   char owner[33], error[256];
   long head = 0, checkpoint = 0;
   int verified = audit_worm_verify(error, sizeof error, &head, &checkpoint);
   int truncated = 0;
   cJSON *requests =
       verified == AUDIT_WORM_VERIFY_RED
           ? NULL
           : audit_worm_memory_requests(context->principal, jo_str(plan, "scan_from", from), until,
                                        head, &truncated);
   if (!requests || audit_worm_dispatch_owner(owner) != 0)
   {
      cJSON_Delete(requests);
      cJSON_Delete(plan);
      cJSON_Delete(query);
      return server_send_error(conn, "verified health receipt population unavailable", NULL);
   }
   cJSON_AddStringToObject(query, "from", from);
   cJSON_AddStringToObject(query, "until", until);
   cJSON_Delete(plan);
   unsigned imported = 0, failed = 0;
   struct timespec start, current;
   clock_gettime(CLOCK_MONOTONIC, &start);
   /* Import chronologically, allowing a bounded journal to keep newest rows. */
   for (int i = cJSON_GetArraySize(requests) - 1; i >= 0; i--)
   {
      clock_gettime(CLOCK_MONOTONIC, &current);
      if (current.tv_sec - start.tv_sec >= 45)
      {
         failed += (unsigned)i + 1;
         break;
      }
      const char *id = cJSON_GetStringValue(cJSON_GetArrayItem(requests, i));
      cJSON *rows = audit_worm_read_request_through(context->principal, id, head);
      if (!rows)
      {
         failed++;
         continue;
      }
      cJSON *input = cJSON_CreateObject();
      cJSON_AddStringToObject(input, "operation", "health-import");
      cJSON_AddStringToObject(input, "health_principal", context->principal);
      cJSON_AddStringToObject(input, "receipt_request_id", id);
      cJSON_AddStringToObject(input, "dispatch_owner", owner);
      cJSON_AddTrueToObject(input, "chain_intact");
      cJSON_AddStringToObject(input, "checkpoint_state",
                              verified == AUDIT_WORM_VERIFY_GREEN ? "locally_attested"
                                                                  : "uncheckpointed_tail");
      cJSON_AddItemToObject(input, "ledger_events", rows);
      cJSON *result = memory_health_command(input);
      if (result && !strcmp(jo_str(result, "status", ""), "ok"))
         imported++;
      else
         failed++;
      cJSON_Delete(result);
   }
   cJSON_DeleteItemFromObjectCaseSensitive(query, "operation");
   cJSON_AddStringToObject(query, "operation", "health-report");
   cJSON_AddBoolToObject(query, "collection_complete", !truncated && !failed);
   cJSON *result = memory_health_command(query);
   if (result)
   {
      cJSON *collection = cJSON_AddObjectToObject(result, "collection");
      cJSON_AddNumberToObject(collection, "selected_requests", cJSON_GetArraySize(requests));
      cJSON_AddNumberToObject(collection, "imported_requests", imported);
      cJSON_AddNumberToObject(collection, "failed_requests", failed);
      cJSON_AddBoolToObject(collection, "truncated", truncated);
      char sequence[32];
      snprintf(sequence, sizeof sequence, "%ld", head);
      cJSON_AddStringToObject(collection, "verified_through_sequence", sequence);
   }
   cJSON_Delete(requests);
   return result ? send_and_free(conn, result)
                 : server_send_error(conn, "health owner unavailable", NULL);
}
