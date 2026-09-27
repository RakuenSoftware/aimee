/* server_error_kind.c: the classified dispatch-error reply.
 *
 * Split out of server.c only because that file sits at the 2500-line ceiling;
 * server_send_error() stays there and forwards here. Same precedent as
 * server_api_status.c.
 */

#include "cJSON.h"
#include "server.h"
#include "server_error_kind.h"
#include "module_commands.h"
#include <stdlib.h>
#include <string.h>

static server_error_http_status_provider_fn g_http_status_provider;

void server_error_kind_register_http_status_provider(server_error_http_status_provider_fn provider)
{
   g_http_status_provider = provider;
}

int server_error_kind_http_status(const char *kind)
{
   uint32_t status = 0;
   if (g_http_status_provider && g_http_status_provider(kind, &status) == 0 && status >= 400u &&
       status <= 599u)
      return (int)status;
   return 0;
}

void server_error_kind_apply(cJSON *resp, const char *kind)
{
   if (!resp)
      return;

   cJSON_DeleteItemFromObjectCaseSensitive(resp, "kind");
   if (kind && kind[0])
      cJSON_AddStringToObject(resp, "kind", kind);

   cJSON_DeleteItemFromObjectCaseSensitive(resp, "http_status");
   int http_status = server_error_kind_http_status(kind);
   if (http_status)
      cJSON_AddNumberToObject(resp, "http_status", (double)http_status);
}

/* Send a dispatch error, naming WHO was at fault.
 *
 * The envelope otherwise carries only {status:"error", message}, so anything in
 * front of it — the webchat relay, an SDK, any HTTP mapping — cannot separate
 * "you passed bad arguments" from "the vault refused" from "the database is
 * down". runtime-web mapped every one of them to 502 Bad Gateway, so `agent add`
 * with no arguments answered:
 *
 *     502  server: usage: agent add <name> <endpoint> <model>
 *
 * a usage message delivered as an upstream failure. That misleads whoever reads
 * the logs, and invites a client's retry logic to hammer a request that can
 * never succeed.
 *
 * `kind` is OPTIONAL and additive. server_send_error() passes NULL, so its ~479
 * call sites remain unclassified and the runtime-web module returns 502.
 * Handlers opt in as they are reviewed, which lets the mapping tighten one
 * handler at a time instead of in a single 479-site change nobody could review
 * honestly. The module-produced HTTP status is additive too; CLI consumers may
 * ignore it, while the physical web provider no longer evaluates `kind`.
 *
 * Use the SERVER_ERR_* constants from server.h. If the event-bus provider is
 * unavailable or returns invalid data, the envelope omits `http_status` and the
 * web boundary treats it as a generic transport failure. */
/* The typed error as a VALUE, so a command can return one instead of writing it.
 *
 * Split out for the command-table port: the RPC handlers write errors to a
 * connection, but every other surface needs a result. jo_err is NOT a substitute
 * -- it carries only {status, message}, so a mechanical split through it would
 * silently drop `kind` and the derived `http_status`, turning a typed
 * NOT_FOUND/INVALID_ARGUMENT into an untyped failure that clients cannot branch
 * on. Building both forms from one function is what keeps the two identical. */
cJSON *server_error_kind_json(const char *kind, const char *message, const char *request_id)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "error");
   cJSON_AddStringToObject(resp, "message", message);
   server_error_kind_apply(resp, kind);
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);
   return resp;
}

/* Modules return the complete public envelope. The host only supplies its
 * existing HTTP error classification, which belongs to the server transport. */
cJSON *server_invoke_module_operation(const char *method, const char *operation, const cJSON *args,
                                      const char *unavailable_message)
{
   cJSON *request = args ? cJSON_Duplicate(args, 1) : cJSON_CreateObject();
   if (!cJSON_IsObject(request))
   {
      cJSON_Delete(request);
      return server_error_kind_json(SERVER_ERR_UNAVAILABLE, unavailable_message, NULL);
   }
   cJSON_DeleteItemFromObjectCaseSensitive(request, "operation");
   cJSON *reply = NULL;
   int rc = -1;
   if (cJSON_AddStringToObject(request, "operation", operation))
      rc = aimee_module_commands_dispatch_internal_timeout(method, request, 60000, &reply);
   cJSON_Delete(request);
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "status"));
   if (rc <= 0 || !cJSON_IsObject(reply) || !status ||
       (strcmp(status, "ok") != 0 && strcmp(status, "error") != 0))
   {
      cJSON_Delete(reply);
      return server_error_kind_json(SERVER_ERR_UNAVAILABLE, unavailable_message, NULL);
   }
   const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "kind"));
   if (kind)
   {
      /* Applying the classification replaces the kind member. */
      char *owned_kind = strdup(kind);
      if (!owned_kind)
      {
         cJSON_Delete(reply);
         return server_error_kind_json(SERVER_ERR_UNAVAILABLE, unavailable_message, NULL);
      }
      server_error_kind_apply(reply, owned_kind);
      free(owned_kind);
   }
   return reply;
}

int server_send_error_kind(server_conn_t *conn, const char *kind, const char *message,
                           const char *request_id)
{
   return server_send_ok(conn, server_error_kind_json(kind, message, request_id));
}
