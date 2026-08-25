/* Human review and durable refusal handlers for episodic memories. */
#include "server.h"
#include "server_error_kind.h"
#include "server_state_internal.h"
#include "json_fluent.h"
#include "kb_client.h"

#include <stdlib.h>

cJSON *memory_review_list_command(cJSON *req)
{
   const char *state = jo_str(req, "state", "");
   int limit = jo_int(req, "limit", 64);
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
