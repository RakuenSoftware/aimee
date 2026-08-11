/* server_error_kind.c: the classified dispatch-error reply.
 *
 * Split out of server.c only because that file sits at the 2500-line ceiling;
 * server_send_error() stays there and forwards here. Same precedent as
 * server_api_status.c.
 */

#include "cJSON.h"
#include "server.h"
#include "server_error_kind.h"

static server_error_http_status_provider_fn g_http_status_provider;

void server_error_kind_register_http_status_provider(server_error_http_status_provider_fn provider)
{
   g_http_status_provider = provider;
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
   if (kind && kind[0])
      cJSON_AddStringToObject(resp, "kind", kind);
   uint32_t http_status = 0;
   if (g_http_status_provider && g_http_status_provider(kind, &http_status) == 0 &&
       http_status >= 400u && http_status <= 599u)
      cJSON_AddNumberToObject(resp, "http_status", (double)http_status);
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);
   return resp;
}

int server_send_error_kind(server_conn_t *conn, const char *kind, const char *message,
                           const char *request_id)
{
   return server_send_ok(conn, server_error_kind_json(kind, message, request_id));
}
