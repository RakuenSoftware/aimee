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
#include "request_context.h"
#include <aimee/workspace/workspace.h>
#include <math.h>
#include <errno.h>
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

/* Classify transport errors without decoding/re-encoding owner integer tokens. */
static cJSON *memory_owner_error_reply(const char *raw, const cJSON *parsed)
{
   cJSON *reply = NULL;
   /* HTTP classification belongs to the host's runtime-web provider. Add
    * only that transport field; preserve all owner tokens, including IDs
    * inside error receipts. Owner envelopes cannot supply this host field. */
   cJSON *classification = cJSON_CreateObject();
   server_error_kind_apply(classification, jo_cstr(parsed, "kind"));
   const cJSON *http = cJSON_GetObjectItemCaseSensitive(classification, "http_status");
   if (cJSON_IsNumber(http) && http->valuedouble >= 400 && http->valuedouble <= 599 &&
       floor(http->valuedouble) == http->valuedouble)
   {
      size_t capacity = strlen(raw) + 64;
      char *classified = malloc(capacity);
      if (classified)
      {
         snprintf(classified, capacity, "{\"http_status\":%d,%s", http->valueint,
                  strchr(raw, '{') + 1);
         reply = cJSON_CreateRaw(classified);
         free(classified);
      }
   }
   else
      reply = cJSON_CreateRaw(raw);
   cJSON_Delete(classification);
   return reply;
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
   static const char *fields[] = {"key",
                                  "content",
                                  "tier",
                                  "kind",
                                  "confidence",
                                  "session_id",
                                  "use_cases",
                                  "epistemic_kind",
                                  "limit",
                                  "old_id",
                                  "new_content",
                                  "keywords",
                                  "id",
                                  "as_of",
                                  "read_policy",
                                  "include_version",
                                  "at_version",
                                  "expected_version",
                                  "idempotency_key",
                                  "proposal_id",
                                  "payload_digest",
                                  "action",
                                  NULL};
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
      reply = cJSON_HasObjectItem(parsed, "http_status") ? cJSON_CreateRaw(raw)
                                                         : memory_owner_error_reply(raw, parsed);
   cJSON_Delete(parsed);
   free(raw);
   return reply ? reply
                : server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                         "KB memory owner unavailable or invalid response", NULL);
}

/* Hygiene has an explicit structured scope. Forward its arguments unchanged
 * for the Go owner to validate; ambient workspace state must not widen it. */
int handle_memory_hygiene(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *request = cJSON_Duplicate(req, 1);
   if (!cJSON_IsObject(request))
   {
      cJSON_Delete(request);
      return send_and_free(conn, server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                                        "invalid hygiene request", NULL));
   }
   cJSON_DeleteItemFromObjectCaseSensitive(request, "method");
   cJSON_DeleteItemFromObjectCaseSensitive(request, "protocol_version");
   char *raw = kb_v1_action_request("memory.hygiene", request);
   cJSON *parsed = raw && strlen(raw) <= AIMEE_MODULE_MESSAGE_MAX_BODY
                       ? cJSON_ParseWithOpts(raw, NULL, 1)
                       : NULL;
   cJSON *reply = NULL;
   if (cJSON_IsObject(parsed) && !strcmp(jo_cstr(parsed, "status"), "ok") &&
       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(parsed, "dry_run")) &&
       cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(parsed, "findings")))
      reply = cJSON_CreateRaw(raw);
   else if (cJSON_IsObject(parsed) && !strcmp(jo_cstr(parsed, "status"), "error") &&
            cJSON_IsString(cJSON_GetObjectItemCaseSensitive(parsed, "kind")))
      reply = memory_owner_error_reply(raw, parsed);
   cJSON_Delete(parsed);
   free(raw);
   return send_and_free(
       conn,
       reply ? reply
             : server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                      "KB hygiene owner unavailable or invalid response", NULL));
}

/* The runtime envelope quotes the owner's JSON so cJSON never rewrites its
 * integer tokens. Only the Go owner shapes private command results. */
static cJSON *user_memory_owner_command_as(const char *operation, const cJSON *req,
                                           memory_authority_t authority)
{
   const char *account = server_request_account();
   const char *principal = request_context_principal();
   if (!account)
      account = "";
   cJSON *context = cJSON_CreateObject();
   if (!context || !cJSON_AddBoolToObject(context, "authenticated", account[0] || principal[0]) ||
       !cJSON_AddBoolToObject(context, "user_authority",
                              account[0] && authority == MEMORY_AUTHORITY_USER) ||
       !cJSON_AddStringToObject(context, "principal", account[0] ? account : principal) ||
       !cJSON_AddStringToObject(context, "transport_identity", principal))
   {
      cJSON_Delete(context);
      return server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                    "user memory caller context unavailable", NULL);
   }
   cJSON *request = req ? cJSON_Duplicate(req, 1) : cJSON_CreateObject();
   cJSON *transport = NULL;
   int dispatched = -1;
   if (cJSON_IsObject(request))
   {
      cJSON_DeleteItemFromObjectCaseSensitive(request, "operation");
      if (cJSON_AddStringToObject(request, "operation", operation))
         dispatched = aimee_module_commands_dispatch_internal_context_timeout(
             "memory.runtime", request, context, 60000, &transport);
   }
   cJSON_Delete(request);
   cJSON_Delete(context);
   if (dispatched <= 0)
   {
      cJSON_Delete(transport);
      return server_error_kind_json(SERVER_ERR_UNAVAILABLE, "user memory module unavailable", NULL);
   }
   if (transport && !strcmp(jo_cstr(transport, "status"), "error"))
   {
      char *kind = strdup(jo_cstr(transport, "kind"));
      if (!kind)
      {
         cJSON_Delete(transport);
         return server_error_kind_json(SERVER_ERR_UNAVAILABLE, "user memory module unavailable",
                                       NULL);
      }
      server_error_kind_apply(transport, kind);
      free(kind);
      return transport;
   }
   const char *raw = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(transport, "json"));
   cJSON *parsed = raw && strlen(raw) <= AIMEE_MODULE_MESSAGE_MAX_BODY
                       ? cJSON_ParseWithOpts(raw, NULL, 1)
                       : NULL;
   const char *status = jo_cstr(parsed, "status");
   cJSON *reply = NULL;
   if (cJSON_IsObject(parsed) && !strcmp(status, "ok") &&
       (!strcmp(operation, "user-search") || !strcmp(jo_cstr(parsed, "store"), "user")))
      reply = cJSON_CreateRaw(raw);
   else if (cJSON_IsObject(parsed) && !strcmp(status, "error") &&
            cJSON_IsString(cJSON_GetObjectItemCaseSensitive(parsed, "kind")) &&
            !cJSON_HasObjectItem(parsed, "http_status"))
   {
      reply = memory_owner_error_reply(raw, parsed);
   }
   cJSON_Delete(parsed);
   cJSON_Delete(transport);
   return reply ? reply
                : server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                         "user memory owner unavailable or invalid response", NULL);
}

static cJSON *user_memory_owner_command(const char *operation, const cJSON *req)
{
   return user_memory_owner_command_as(operation, req, MEMORY_AUTHORITY_MODEL);
}

cJSON *memory_user_mcp_supersede_command(const cJSON *req)
{
   return user_memory_owner_command_as("user-mcp-supersede", req, MEMORY_AUTHORITY_MODEL);
}

/* Explicit placement keeps the existing shared command default. The host only
 * forwards the review envelope and authenticated context; Go admits decisions. */
static cJSON *memory_correction_command(cJSON *req, int review)
{
   int selection = cJSON_HasObjectItem(req, "store") ? server_memory_store_selection(req) : 1;
   if (selection == 0)
      return user_memory_owner_command_as(
          review ? "user-correction-review" : "user-correction-proposals", req,
          review ? MEMORY_AUTHORITY_USER : MEMORY_AUTHORITY_MODEL);
   if (selection != 1)
      return server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT, "memory store must be user or kb",
                                    NULL);
   return kb_memory_owner_command(
       review ? "memory.review_correction" : "memory.correction_proposals", req,
       review ? MEMORY_AUTHORITY_USER : MEMORY_AUTHORITY_MODEL, review ? "proposal" : "proposals",
       review ? cJSON_Object : cJSON_Array);
}

int handle_memory_correction_proposals(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return send_and_free(conn, memory_correction_command(req, 0));
}

int handle_memory_review_correction(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return send_and_free(conn, memory_correction_command(req, 1));
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
   return send_and_free(conn, user_memory_owner_command("user-search", req));
}

/* Placement selection and opaque transport only; Go owns the diagnostic. */
int handle_memory_validity(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int selection = server_memory_store_selection(req);
   if (selection < 0)
      return send_and_free(conn, memory_bad_store());
   cJSON *request = cJSON_Duplicate(req, 1);
   if (!cJSON_IsObject(request))
   {
      cJSON_Delete(request);
      return send_and_free(conn, server_error_kind_json(SERVER_ERR_INVALID_ARGUMENT,
                                                        "invalid validity request", NULL));
   }
   cJSON_DeleteItemFromObjectCaseSensitive(request, "store");
   if (!selection)
   {
      cJSON *reply = user_memory_owner_command_as(
          "user-validity", request, server_account_memory_authority(server_request_account()));
      cJSON_Delete(request);
      return send_and_free(conn, reply);
   }
   char *raw = kb_v1_action_request("memory.validity", request);
   cJSON *parsed = raw && strlen(raw) <= AIMEE_MODULE_MESSAGE_MAX_BODY
                       ? cJSON_ParseWithOpts(raw, NULL, 1)
                       : NULL;
   cJSON *reply = NULL;
   if (cJSON_IsObject(parsed) && !strcmp(jo_cstr(parsed, "status"), "ok") &&
       cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(parsed, "decision")))
      reply = cJSON_CreateRaw(raw);
   else if (cJSON_IsObject(parsed) && !strcmp(jo_cstr(parsed, "status"), "error") &&
            cJSON_IsString(cJSON_GetObjectItemCaseSensitive(parsed, "kind")))
      reply = memory_owner_error_reply(raw, parsed);
   cJSON_Delete(parsed);
   free(raw);
   return send_and_free(
       conn,
       reply ? reply
             : server_error_kind_json(SERVER_ERR_UNAVAILABLE,
                                      "KB validity owner unavailable or invalid response", NULL));
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

   return user_memory_owner_command_as("user-store", req, authority);
}

int handle_memory_store(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   /* Carry verified caller identity separately from the request body. Go owns
    * mutation admission; the host only identifies this authenticated channel. */
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
   return user_memory_owner_command("user-list", req);
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
      return user_memory_owner_command("user-stats", req);
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
   if (cJSON_IsString(item))
   {
      const char *text = item->valuestring;
      if (!text || text[0] < '1' || text[0] > '9')
         return -1;
      for (const char *p = text; *p; p++)
         if (*p < '0' || *p > '9')
            return -1;
      errno = 0;
      char *end = NULL;
      long long value = strtoll(text, &end, 10);
      if (errno || !end || *end || value <= 0 || value > INT64_MAX)
         return -1;
      *out = (int64_t)value;
      return 0;
   }
   if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble <= 0.0 ||
       item->valuedouble >= 9223372036854775808.0 || floor(item->valuedouble) != item->valuedouble)
      return -1;
   *out = (int64_t)item->valuedouble;
   return *out <= INT64_C(9007199254740991) ? 0 : -1;
}

/* Replacement and its version chain are owned by Go in the selected placement. */
int handle_memory_supersede(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int selection = server_memory_store_selection(req);
   if (selection < 0)
      return send_and_free(conn, memory_bad_store());
   if (!selection)
      return send_and_free(conn, user_memory_owner_command_as(
                                     "user-supersede", req,
                                     server_account_memory_authority(server_request_account())));

   return send_and_free(
       conn, kb_memory_owner_command("memory.supersede", req,
                                     server_account_memory_authority(server_request_account()),
                                     "id", cJSON_Number));
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
   return user_memory_owner_command_as("user-delete", req,
                                       server_account_memory_authority(account));
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
   return user_memory_owner_command("user-get", req);
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
static char *user_memory_recall_json(const char *hint, int limit_tokens, int session_start,
                                     const size_t *native_bytes)
{
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return NULL;
   cJSON_AddStringToObject(request, "task_hint", hint ? hint : "");
   cJSON_AddNumberToObject(request, "limit_tokens", limit_tokens);
   if (native_bytes)
      cJSON_AddNumberToObject(request, "native_context_bytes", (double)*native_bytes);
   cJSON_AddBoolToObject(request, "session_start", session_start != 0);
   cJSON *response = server_invoke_module_operation("memory.runtime", "personal-recall", request,
                                                    "user memory module unavailable");
   cJSON_Delete(request);
   const char *body = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(response, "json"));
   char *json = body ? strdup(body) : NULL;
   cJSON_Delete(response);
   integrity_result_t gate;
   if (json && integrity_ingress_decide(json, INTEGRITY_SOURCE_AGENT_MESSAGE, "recall", 1, &gate))
   {
      free(json);
      return strdup("{\"status\":\"quarantined\",\"store\":\"user\",\"recall\":{},"
                    "\"integrity_verdict\":\"quarantine\"}");
   }
   return json;
}

char *server_user_memory_recall_json(const char *hint, int limit_tokens, int session_start)
{
   return user_memory_recall_json(hint, limit_tokens, session_start, NULL);
}
char *server_user_memory_recall_native_json(const char *hint, int limit_tokens, int session_start,
                                            size_t native_bytes)
{
   return user_memory_recall_json(hint, limit_tokens, session_start, &native_bytes);
}
