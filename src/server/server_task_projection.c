/* Task state and memory owners perform all policy. This bridge supplies verified
 * request identity and fresh owner output; caller-supplied prepared data is dropped. */
#include "aimee.h"
#include "server.h"
#include "request_context.h"
#include "db1_client/session_state.h"
#include "json_fluent.h"
#include <stdlib.h>
#include <string.h>

static cJSON *task_projection_call(const char *principal, const char *sid, cJSON *request)
{
   char *wire = cJSON_PrintUnformatted(request);
   char *reply = malloc(262144);
   cJSON *result = NULL;
   if (wire && reply && db1_session_task_projection_apply(principal, sid, wire, reply, 262144) == 0)
      result = cJSON_ParseWithOpts(reply, NULL, 1);
   free(wire);
   free(reply);
   return result;
}

cJSON *task_projection_command(cJSON *input)
{
   const char *principal = request_context_principal();
   const char *sid = jo_cstr(input, "session_id");
   if (!principal || !principal[0] || !sid[0])
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "authenticated session_id required", NULL);
   cJSON *request = cJSON_CreateObject();
   const char *fields[] = {"operation",   "task_id",        "expected_revision",
                           "binding",     "ttl_seconds",    "max_context_bytes",
                           "items",       "events",         "target_id",
                           "claim_index", "preview_digest", NULL};
   for (int i = 0; fields[i]; i++)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(input, fields[i]);
      if (value)
         cJSON_AddItemToObject(request, fields[i], cJSON_Duplicate(value, 1));
   }
   const char *op = jo_cstr(input, "operation");
   if (strcmp(op, "rebuild") && strcmp(op, "get") && strcmp(op, "describe") &&
       strcmp(op, "discard") && strcmp(op, "promotion_preview") && strcmp(op, "promote"))
   {
      cJSON_Delete(request);
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "unknown task projection operation", NULL);
   }
   cJSON *binding = NULL;
   if (!strcmp(op, "rebuild"))
      binding = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(request, "binding"), 1);
   else if (!strcmp(op, "get") || !strcmp(op, "promotion_preview") || !strcmp(op, "promote"))
   {
      cJSON *describe = cJSON_CreateObject();
      cJSON_AddStringToObject(describe, "operation", "describe");
      cJSON_AddStringToObject(describe, "task_id", jo_cstr(input, "task_id"));
      cJSON *described = task_projection_call(principal, sid, describe);
      cJSON_Delete(describe);
      if (described && !strcmp(jo_cstr(described, "status"), "ok"))
         binding = cJSON_Duplicate(
             cJSON_GetObjectItemCaseSensitive(
                 cJSON_GetObjectItemCaseSensitive(described, "projection"), "binding"),
             1);
      if (!binding)
      {
         cJSON_Delete(request);
         return described ? described
                          : server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                                   "task projection owner unavailable", NULL);
      }
      cJSON_Delete(described);
   }
   if (binding)
   {
      cJSON *owner = memory_serve_command(binding);
      char *raw = owner ? cJSON_PrintUnformatted(owner) : NULL;
      cJSON *prepared = raw ? cJSON_ParseWithOpts(raw, NULL, 1) : NULL;
      free(raw);
      cJSON_Delete(owner);
      cJSON_Delete(binding);
      if (prepared)
         cJSON_AddItemToObject(request, "prepared", prepared);
   }
   cJSON *result = task_projection_call(principal, sid, request);
   if (!strcmp(op, "promote") && result && !strcmp(jo_cstr(result, "status"), "ok"))
   {
      cJSON *admitted = memory_task_promotion_command(result);
      char *raw = admitted ? cJSON_PrintUnformatted(admitted) : NULL;
      cJSON *parsed = raw ? cJSON_ParseWithOpts(raw, NULL, 1) : NULL;
      free(raw);
      const char *kind = jo_cstr(parsed, "kind");
      int definite = !strcmp(jo_cstr(parsed, "status"), "ok") || !strcmp(kind, "conflict") ||
                     !strcmp(kind, "invalid_argument") || !strcmp(kind, "unauthorized");
      if (definite)
      {
         cJSON *finish = cJSON_CreateObject();
         cJSON_AddStringToObject(finish, "operation", "promotion_finish");
         cJSON_AddStringToObject(finish, "task_id", jo_cstr(input, "task_id"));
         cJSON_AddStringToObject(finish, "expected_revision", jo_cstr(input, "expected_revision"));
         cJSON_AddStringToObject(finish, "preview_digest", jo_cstr(result, "preview_digest"));
         cJSON_AddItemToObject(finish, "promotion_result", cJSON_Duplicate(parsed, 1));
         cJSON *finished = task_projection_call(principal, sid, finish);
         cJSON_Delete(finish);
         if (!finished || strcmp(jo_cstr(finished, "status"), "ok"))
            cJSON_AddStringToObject(parsed, "task_receipt_state", "reconciliation_required");
         cJSON_Delete(finished);
      }
      cJSON_Delete(admitted);
      cJSON_Delete(result);
      result = parsed;
   }
   cJSON_Delete(request);
   return result ? result
                 : server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                          "task projection owner unavailable or request refused",
                                          NULL);
}

int handle_task_projection(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return server_send_ok(conn, task_projection_command(req));
}
