/* Public memory commands: explicit local-user or shared-KB placement. */
#include "server_state_internal.h"
#include "aimee.h"
#include "server.h"
#include "headers/module_commands.h"
#include "module_stage_adapters.h"
#include "kb_client.h"
#include "json_fluent.h"
#include "log.h"
#include "integrity.h"
#include <aimee/workspace/workspace.h>
#include <math.h>
#include <aimee/core/event_bus/module_protocol.h>

/* --- Memory handlers --- */

/* Placement is explicit at this transport boundary. A cwd, project, numeric
 * ID, or failed local lookup must never promote private memory to the KB. */
int server_memory_store_selection(const cJSON *req)
{
   const cJSON *store = cJSON_GetObjectItemCaseSensitive(req, "store");
   if (!store)
      return 0;
   if (!cJSON_IsString(store))
      return -1;
   if (strcmp(store->valuestring, "user") == 0)
      return 0;
   if (strcmp(store->valuestring, "kb") == 0)
      return 1;
   return -1;
}

static cJSON *memory_bad_store(void)
{
   return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT, "memory store must be user or kb",
                                 NULL);
}

static cJSON *memory_data_request(const char *operation)
{
   cJSON *request = cJSON_CreateObject();
   if (request)
      cJSON_AddStringToObject(request, "operation", operation);
   return request;
}

int server_memory_scope_begin(cJSON *req)
{
   const char *cwd = jo_str(req, "cwd", NULL);
   const char *project_arg = jo_str(req, "project", NULL);
   const char *workspace_arg = jo_str(req, "workspace", NULL);
   const char *scope_arg = jo_str(req, "scope", NULL);
   char project[MAX_PATH_LEN] = "";
   char workspace[MAX_PATH_LEN] = "";
   if (project_arg)
      snprintf(project, sizeof(project), "%s", project_arg);
   if (workspace_arg)
      snprintf(workspace, sizeof(workspace), "%s", workspace_arg);
   if ((!project[0] || !workspace[0]) && cwd && cwd[0])
   {
      char resolved_project[MAX_PATH_LEN] = "";
      char resolved_workspace[MAX_PATH_LEN] = "";
      if (workspace_repo_identity(cwd, resolved_project, sizeof(resolved_project),
                                  resolved_workspace, sizeof(resolved_workspace)) == 0)
      {
         if (!project[0])
            snprintf(project, sizeof(project), "%s", resolved_project);
         if (!workspace[0])
            snprintf(workspace, sizeof(workspace), "%s", resolved_workspace);
      }
   }
   int include_all = scope_arg && strcmp(scope_arg, "all") == 0;
   kb_client_memory_scope_context_set(workspace, project, include_all);
   return (!include_all && !workspace[0] && !project[0]) ? 1 : 0;
}

/* THE command, in the shape the core command table can route.
 *
 * Every surface needs the same thing from a command -- a result -- but the RPC
 * handlers were written to WRITE ONE to a connection and return int, so there was
 * nothing for a table to hand back to MCP or ACP. That shape is why capability
 * surface ended up declared four separate times: a command reachable over RPC had
 * no result-returning form to register, so each surface grew its own list.
 *
 * Splitting it costs nothing at the wire: server_send_error and jo_err build the
 * identical {status:"error", message} envelope, so the bytes on the RPC path are
 * unchanged. handle_memory_store below is now only the connection write. */
/* Transport complete owner envelopes without a fixed memory_t buffer or a
 * decode/re-encode of integer tokens. The authenticated host chooses authority;
 * the Go owner validates arguments and admits the actual mutation. */
static cJSON *kb_memory_owner_command(const char *method, const cJSON *req,
                                      memory_authority_t authority, const char *required,
                                      int expected_type)
{
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return server_error_kind_json(SERVER_ERR_UNAVAILABLE, "KB memory request unavailable", NULL);
   static const char *fields[] = {"key",        "content",    "tier",        "kind",
                                  "confidence", "session_id", "use_cases",   "epistemic_kind",
                                  "limit",      "old_id",     "new_content", "keywords",
                                  "id",         "as_of",      NULL};
   for (int i = 0; fields[i]; i++)
   {
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(req, fields[i]);
      if (value)
         cJSON_AddItemToObject(request, fields[i], cJSON_Duplicate(value, 1));
   }
   cJSON_AddStringToObject(request, "view", "server");
   if (authority == MEMORY_AUTHORITY_USER)
      cJSON_AddStringToObject(request, "authority", "user");
   server_memory_scope_begin((cJSON *)req);
   kb_client_memory_scope_context_apply(request);
   char *raw = kb_v1_action_request(method, request);
   kb_client_memory_scope_context_clear();
   cJSON *parsed = raw && strlen(raw) <= AIMEE_MODULE_MESSAGE_MAX_BODY
                       ? cJSON_ParseWithOpts(raw, NULL, 1)
                       : NULL;
   cJSON *reply = NULL;
   const cJSON *field = cJSON_GetObjectItemCaseSensitive(parsed, required);
   if (cJSON_IsObject(parsed) && !strcmp(jo_cstr(parsed, "status"), "ok") && field &&
       (field->type & 0xff) == expected_type &&
       (strcmp(method, "memory.search") ||
        cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(parsed, "windows"))) &&
       (expected_type != cJSON_Number || (isfinite(field->valuedouble) && field->valuedouble > 0 &&
                                          floor(field->valuedouble) == field->valuedouble)))
      reply = cJSON_CreateRaw(raw);
   else if (cJSON_IsObject(parsed) && !strcmp(jo_cstr(parsed, "status"), "error") &&
            cJSON_IsString(cJSON_GetObjectItemCaseSensitive(parsed, "kind")))
      reply = cJSON_CreateRaw(raw);
   cJSON_Delete(parsed);
   free(raw);
   return reply ? reply
                : server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                         "KB memory owner unavailable or invalid response", NULL);
}

int handle_memory_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int selection = server_memory_store_selection(req);
   if (selection < 0)
      return send_and_free(conn, memory_bad_store());
   if (selection)
      return send_and_free(conn,
                           kb_memory_owner_command("memory.search", req, MEMORY_AUTHORITY_MODEL,
                                                   "facts", cJSON_Array));
   return send_and_free(conn, server_invoke_module_operation("memory.runtime", "user-search", req,
                                                             "user memory module unavailable"));
}

static cJSON *kb_memory_store_command(const cJSON *req, memory_authority_t authority)
{
   return kb_memory_owner_command("memory.store", req, authority, "id", cJSON_Number);
}

cJSON *memory_store_command(const cJSON *req, memory_authority_t authority)
{
   int selection = server_memory_store_selection(req);
   if (selection < 0)
      return memory_bad_store();
   if (selection)
      return kb_memory_store_command(req, authority);

   return server_invoke_module_operation("memory.runtime", "user-store", req,
                                         "user memory module unavailable");
}

int handle_memory_store(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   /* The module's server placement is per-appliance-user. The legacy authority
    * argument remains in the command ABI, but cannot widen the module's user
    * scope and is not persisted as KB provenance. */
   return send_and_free(
       conn, memory_store_command(req, server_account_memory_authority(server_request_account())));
}

static cJSON *kb_memory_list_command(const cJSON *req)
{
   return kb_memory_owner_command("memory.list", req, MEMORY_AUTHORITY_MODEL, "memories",
                                  cJSON_Array);
}

cJSON *memory_list_command(const cJSON *req)
{
   int selection = server_memory_store_selection(req);
   if (selection < 0)
      return memory_bad_store();
   if (selection)
      return kb_memory_list_command(req);
   return server_invoke_module_operation("memory.runtime", "user-list", req,
                                         "user memory module unavailable");
}

int handle_memory_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return send_and_free(conn, memory_list_command(req));
}

cJSON *memory_stats_command(const cJSON *req)
{
   int selection = server_memory_store_selection(req);
   if (selection < 0)
      return memory_bad_store();
   if (!selection)
      return server_invoke_module_operation("memory.runtime", "user-stats", req,
                                            "user memory module unavailable");
   char *raw = kb_v1_action_request("memory.stats", cJSON_CreateObject());
   cJSON *parsed = raw && strlen(raw) <= AIMEE_MODULE_MESSAGE_MAX_BODY
                       ? cJSON_ParseWithOpts(raw, NULL, 1)
                       : NULL;
   cJSON *reply = NULL;
   if (cJSON_IsObject(parsed) && strcmp(jo_cstr(parsed, "status"), "ok") == 0 &&
       cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(parsed, "stats")))
      reply = cJSON_CreateRaw(raw);
   else if (cJSON_IsObject(parsed) && strcmp(jo_cstr(parsed, "status"), "error") == 0)
   {
      char *kind = strdup(jo_str(parsed, "kind", SERVER_ERR_UNAVAILABLE));
      if (kind)
      {
         server_error_kind_apply(parsed, kind[0] ? kind : SERVER_ERR_UNAVAILABLE);
         free(kind);
         reply = parsed;
         parsed = NULL;
      }
   }
   free(raw);
   cJSON_Delete(parsed);
   return reply ? reply
                : server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                         "memory statistics unavailable or invalid response", NULL);
}

int handle_memory_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return send_and_free(conn, memory_stats_command(req));
}

/* cJSON stores numbers as doubles. Reject fractional and unrepresentable IDs
 * instead of truncating them into a different memory's integer primary key. */
int memory_request_positive_id(cJSON *req, const char *field, int64_t *out)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(req, field);
   if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble <= 0.0 ||
       item->valuedouble >= 9223372036854775808.0 || floor(item->valuedouble) != item->valuedouble)
      return -1;
   *out = (int64_t)item->valuedouble;
   return *out <= INT64_C(9007199254740991) ? 0 : -1;
}

/* Replace a memory with a corrected one, linking the two.
 *
 * This is the operation that should be reached for far more often than
 * memory.delete, and it was equally unreachable over /v1: `aimee memory
 * supersede` exists and works on the server host (cmd_memory.c), but the thin
 * client routes through /v1 and there was no route, so a remote user could not
 * say "this belief was replaced" — only store another one, or delete.
 *
 * That asymmetry matters because the store DEPENDS on the supersession chain.
 * memory.list_superseded_keys and memory.fact_history walk it, so deleting a
 * wrong memory instead of superseding it loses the answer to "why did it assert
 * this in March" and destroys the negative examples effectiveness and
 * evidence_strength are computed from. A corrected memory is signal; a deleted
 * one is a hole.
 *
 * Same CAP_MEMORY_WRITE gate as store and delete. */
int handle_memory_supersede(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int selection = server_memory_store_selection(req);
   if (selection < 0)
      return send_and_free(conn, memory_bad_store());
   if (!selection)
      return send_and_free(conn,
                           server_invoke_module_operation("memory.runtime", "user-supersede", req,
                                                          "user memory module unavailable"));

   return send_and_free(conn, kb_memory_owner_command("memory.supersede", req,
                                                      MEMORY_AUTHORITY_MODEL, "id", cJSON_Number));
}

/* Retire one user memory by id. Physical deletion and KB provenance are not
 * part of the server placement's data contract. */
static cJSON *kb_memory_delete_command(cJSON *req, const char *account)
{
   return kb_memory_owner_command("memory.delete", req, server_account_memory_authority(account),
                                  "deleted", cJSON_True);
}

cJSON *memory_delete_command(cJSON *req, const char *account)
{
   int selection = server_memory_store_selection(req);
   if (selection < 0)
      return memory_bad_store();
   if (selection)
      return kb_memory_delete_command(req, account);
   return server_invoke_module_operation("memory.runtime", "user-delete", req,
                                         "user memory module unavailable");
}

int handle_memory_delete(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return server_send_ok(conn, memory_delete_command(req, server_request_account()));
}

static cJSON *kb_memory_get_command(cJSON *req)
{
   return kb_memory_owner_command("memory.get", req, MEMORY_AUTHORITY_MODEL, "memory",
                                  cJSON_Object);
}

cJSON *memory_get_command(cJSON *req)
{
   int selection = server_memory_store_selection(req);
   if (selection < 0)
      return memory_bad_store();
   if (selection)
      return kb_memory_get_command(req);
   return server_invoke_module_operation("memory.runtime", "user-get", req,
                                         "user memory module unavailable");
}

int handle_memory_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return send_and_free(conn, memory_get_command(req));
}

int handle_memory_read(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return send_and_free(conn,
                        kb_memory_owner_command("memory.assemble_context", req,
                                                MEMORY_AUTHORITY_MODEL, "context", cJSON_String));
}

/* Personal recall uses the same memory module as shared recall, with the
 * process's own data grant. No KB request is needed to create its envelope. */
char *server_user_memory_recall_json(const char *hint, int limit_tokens, int session_start)
{
   cJSON *request = memory_data_request("recall-bundle");
   if (!request)
      return NULL;
   cJSON_AddStringToObject(request, "query", hint ? hint : "");
   cJSON_AddNumberToObject(request, "limit_tokens", limit_tokens);
   cJSON_AddBoolToObject(request, "session_start", session_start != 0);
   cJSON *response = server_module_memory_data(request);
   cJSON_Delete(request);
   cJSON *payload = response ? cJSON_DetachItemFromObjectCaseSensitive(response, "payload") : NULL;
   cJSON_Delete(response);
   if (!cJSON_IsObject(payload))
   {
      cJSON_Delete(payload);
      return NULL;
   }
   cJSON *envelope = jo_ok();
   if (!envelope)
   {
      cJSON_Delete(payload);
      return NULL;
   }
   cJSON_AddStringToObject(envelope, "store", "user");
   cJSON_AddItemToObject(envelope, "recall", payload);
   char *json = cJSON_PrintUnformatted(envelope);
   cJSON_Delete(envelope);
   integrity_result_t gate;
   if (json && integrity_ingress_decide(json, INTEGRITY_SOURCE_AGENT_MESSAGE, "recall", 1, &gate))
   {
      free(json);
      return strdup("{\"status\":\"quarantined\",\"store\":\"user\",\"recall\":{},"
                    "\"integrity_verdict\":\"quarantine\"}");
   }
   return json;
}
