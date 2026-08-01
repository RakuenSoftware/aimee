/* server_error_kind.c: the classified dispatch-error reply.
 *
 * Split out of server.c only because that file sits at the 2500-line ceiling;
 * server_send_error() stays there and forwards here. Same precedent as
 * server_api_status.c.
 */

#include "cJSON.h"
#include "server.h"

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
 * call sites are untouched and everything not yet audited keeps today's exact
 * behaviour (the relay's 502). Handlers opt in as they are reviewed, which lets
 * the mapping tighten one handler at a time instead of in a single 479-site
 * change nobody could review honestly.
 *
 * Use the SERVER_ERR_* constants from server.h. A kind the consumer does not
 * recognise is treated as unclassified, so adding one here can never make a
 * downstream mapping worse than it was. */
int server_send_error_kind(server_conn_t *conn, const char *kind, const char *message,
                           const char *request_id)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "error");
   cJSON_AddStringToObject(resp, "message", message);
   if (kind && kind[0])
      cJSON_AddStringToObject(resp, "kind", kind);
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);
   return server_send_ok(conn, resp);
}
