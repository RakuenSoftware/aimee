/* Human review and durable refusal handlers for episodic memories. */
#include "server.h"
#include "server_error_kind.h"
#include "server_state_internal.h"
#include "json_fluent.h"
#include "kb_client.h"
#include <aimee/core/event_bus/module_protocol.h>

#include <stdlib.h>
#include <string.h>

/* Keep the complete owner envelope, including integer tokens in review rows.
 * Errors retain their owner classification instead of becoming a missing row. */
static cJSON *memory_review_response(char *raw, int64_t restore_id)
{
   cJSON *parsed = raw && strlen(raw) <= AIMEE_MODULE_MESSAGE_MAX_BODY
                       ? cJSON_ParseWithOpts(raw, NULL, 1)
                       : NULL;
   const char *status = jo_cstr(parsed, "status");
   cJSON *result = NULL;
   if (cJSON_IsObject(parsed) && strcmp(status, "ok") == 0)
   {
      int valid = restore_id > 0
                      ? cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(parsed, "restored")) &&
                            cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(parsed, "id")) &&
                            cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(parsed, "id")) ==
                                (double)restore_id
                      : cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(parsed, "memories"));
      if (valid)
         result = cJSON_CreateRaw(raw);
   }
   else if (cJSON_IsObject(parsed) && strcmp(status, "error") == 0)
   {
      char *kind = strdup(jo_str(parsed, "kind", SERVER_ERR_UNAVAILABLE));
      if (kind)
      {
         server_error_kind_apply(parsed, kind[0] ? kind : SERVER_ERR_UNAVAILABLE);
         free(kind);
         result = parsed;
         parsed = NULL;
      }
   }
   if (restore_id > 0 && raw)
      kb_client_memory_audit_note("memory.restore", restore_id, NULL, NULL, NULL, 0.0, NULL,
                                  result && cJSON_IsRaw(result));
   cJSON_Delete(parsed);
   free(raw);
   return result ? result
                 : server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                          "memory review unavailable or invalid response", NULL);
}

cJSON *memory_review_list_command(cJSON *req)
{
   int selection = server_memory_store_selection(req);
   if (selection < 0)
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT, "memory store must be user or kb",
                                    NULL);
   if (!selection)
   {
      cJSON *reply = server_invoke_module_operation("memory.runtime", "user-review-list", req,
                                                    "user memory review unavailable");
      if (!strcmp(jo_cstr(reply, "status"), "error"))
         return reply;
      const cJSON *json = cJSON_GetObjectItemCaseSensitive(reply, "json");
      char *raw = cJSON_IsString(json) ? strdup(json->valuestring) : NULL;
      cJSON_Delete(reply);
      return memory_review_response(raw, 0);
   }
   server_memory_scope_begin(req);
   cJSON *request = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(request);
   cJSON_AddStringToObject(request, "state", jo_str(req, "state", ""));
   cJSON_AddNumberToObject(request, "limit", jo_int(req, "limit", 64));
   char *json = kb_v1_action_request("memory.review_list", request);
   kb_client_memory_scope_context_clear();
   return memory_review_response(json, 0);
}

int handle_memory_review_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return send_and_free(conn, memory_review_list_command(req));
}

int handle_memory_reject(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   if (server_memory_store_selection(req) != 1)
      return server_send_error_kind(conn, SERVER_ERR_INVALID_ARGUMENT,
                                    "this operation requires store=kb", NULL);
   cJSON *request = cJSON_CreateObject();
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (id)
      cJSON_AddItemToObject(request, "id", cJSON_Duplicate(id, 1));
   cJSON_AddStringToObject(request, "reason", jo_str(req, "reason", "explicit user rejection"));
   cJSON_AddStringToObject(request, "view", "server");
   server_memory_scope_begin(req);
   kb_client_memory_scope_context_apply(request);
   char *raw = kb_v1_action_request("memory.reject", request);
   kb_client_memory_scope_context_clear();
   cJSON *parsed = raw ? cJSON_ParseWithOpts(raw, NULL, 1) : NULL;
   cJSON *reply = NULL;
   if (!strcmp(jo_cstr(parsed, "status"), "ok") &&
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(parsed, "tombstoned")))
      reply = cJSON_CreateRaw(raw);
   else if (!strcmp(jo_cstr(parsed, "status"), "error"))
   {
      reply = parsed;
      parsed = NULL;
      char *kind = strdup(jo_str(reply, "kind", SERVER_ERR_UNAVAILABLE));
      if (kind)
      {
         server_error_kind_apply(reply, kind);
         free(kind);
      }
   }
   int64_t audit_id = 0;
   (void)memory_request_positive_id(req, "id", &audit_id);
   kb_client_memory_audit_note("memory.reject", audit_id, NULL, NULL, NULL, 0.0, NULL,
                               reply && cJSON_IsRaw(reply));
   cJSON_Delete(parsed);
   free(raw);
   return send_and_free(conn, reply ? reply
                                    : server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                                             "memory rejection unavailable", NULL));
}

cJSON *memory_restore_command(cJSON *req)
{
   if (server_memory_store_selection(req) != 1)
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT, "this operation requires store=kb",
                                    NULL);
   int64_t id = 0;
   if (memory_request_positive_id(req, "id", &id) != 0)
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                    "memory.restore requires a positive integer id", NULL);
   server_memory_scope_begin(req);
   cJSON *request = cJSON_CreateObject();
   kb_client_memory_scope_context_apply(request);
   jo_add_i64(request, "id", id);
   char *raw = kb_v1_action_request("memory.restore", request);
   kb_client_memory_scope_context_clear();
   return memory_review_response(raw, id);
}

int handle_memory_restore(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return send_and_free(conn, memory_restore_command(req));
}
