/* Human review and durable refusal handlers for episodic memories. */
#include "server.h"
#include "server_error_kind.h"
#include "server_state_internal.h"
#include "json_fluent.h"
#include "kb_client.h"
#include "module_stage_adapters.h"

#include <stdlib.h>

cJSON *memory_review_list_command(cJSON *req)
{
   const char *state = jo_str(req, "state", "");
   int limit = jo_int(req, "limit", 64);
   int selection = server_memory_store_selection(req);
   if (selection < 0)
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT, "memory store must be user or kb",
                                    NULL);
   if (!selection)
   {
      cJSON *request = cJSON_CreateObject();
      cJSON_AddStringToObject(request, "operation", "review-list");
      cJSON_AddStringToObject(request, "state", state);
      cJSON_AddNumberToObject(request, "limit", limit);
      cJSON *reply = server_module_memory_data(request);
      cJSON_Delete(request);
      cJSON *rows = reply ? cJSON_DetachItemFromObjectCaseSensitive(reply, "reviews") : NULL;
      cJSON_Delete(reply);
      if (!cJSON_IsArray(rows))
      {
         cJSON_Delete(rows);
         return server_error_kind_json(SERVER_ERR_UNAVAILABLE, "user memory review unavailable",
                                       NULL);
      }
      cJSON *row;
      cJSON_ArrayForEach(row, rows)
          jo_add_str(row, "lifecycle", jo_str(row, "lifecycle_state", ""));
      cJSON *resp = jo_ok();
      cJSON_AddItemToObject(resp, "memories", rows);
      cJSON_AddStringToObject(resp, "store", "user");
      return resp;
   }
   server_memory_scope_begin(req);
   char *json = kb_client_memory_review_list_json(state, limit);
   kb_client_memory_scope_context_clear();
   if (!json)
      return jo_err("knowledge service unavailable; memory review is unreachable");
   cJSON *resp = cJSON_Parse(json);
   free(json);
   return resp ? resp : jo_err("knowledge service returned malformed memory review data");
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
   int64_t id = 0;
   if (memory_request_positive_id(req, "id", &id) != 0)
      return server_send_error_kind(conn, SERVER_ERR_INVALID_ARGUMENT,
                                    "memory.reject requires a positive integer id", NULL);
   const char *reason = jo_str(req, "reason", "explicit user rejection");
   server_memory_scope_begin(req);
   int rc = kb_client_memory_reject(id, reason);
   kb_client_memory_scope_context_clear();
   if (rc != 0)
      return server_send_error_kind(conn, SERVER_ERR_NOT_FOUND,
                                    "no such visible memory, or rejection was refused", NULL);
   cJSON *resp = jo_ok();
   jo_add_i64(resp, "id", id);
   jo_add_bool(resp, "tombstoned", 1);
   return send_and_free(conn, resp);
}

int handle_memory_restore(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   if (server_memory_store_selection(req) != 1)
      return server_send_error_kind(conn, SERVER_ERR_INVALID_ARGUMENT,
                                    "this operation requires store=kb", NULL);
   int64_t id = 0;
   if (memory_request_positive_id(req, "id", &id) != 0)
      return server_send_error_kind(conn, SERVER_ERR_INVALID_ARGUMENT,
                                    "memory.restore requires a positive integer id", NULL);
   server_memory_scope_begin(req);
   int rc = kb_client_memory_restore(id);
   kb_client_memory_scope_context_clear();
   if (rc != 0)
      return server_send_error_kind(conn, SERVER_ERR_NOT_FOUND,
                                    "no such rejected memory, or restore was refused", NULL);
   cJSON *resp = jo_ok();
   jo_add_i64(resp, "id", id);
   jo_add_bool(resp, "restored", 1);
   return send_and_free(conn, resp);
}
