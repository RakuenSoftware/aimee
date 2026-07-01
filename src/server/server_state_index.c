/* server_state_index.c: server-side handlers for the code-index families that
 * proxy aimee-kb /v1/code routes. Split out of server_state.c to keep that
 * translation unit under the 2000-line build-integrity limit. */
#include "server.h"
#include "kb_client.h"
#include "cJSON.h"
#include "json_fluent.h"

#include <stdlib.h>
#include <string.h>

/* Local mirror of the send_and_free helper each server TU defines (see
 * server_state.c / server_memory_benchmark.c) — wraps the ok-envelope sender. */
static int send_and_free(server_conn_t *conn, cJSON *resp)
{
   return server_send_ok(conn, resp);
}

/* S6: index.deps — proxy the kb cross-repo dependency query. The kb response is
 * rich (per-edge evidence + version stamp, or the AMBIGUOUS review queue when
 * status=ambiguous) so we forward it verbatim, same passthrough idiom as
 * handle_graph_explain. Inputs are validated at this public boundary so a direct
 * API client gets an actionable error rather than an opaque 502: direction must be
 * out (default), in (dependents, --reverse), or both. Validation runs before any
 * allocation, so the error paths cannot leak. */
int handle_index_deps(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *project;
   if (jo_need_str(req, "project", &project) < 0 || !project[0])
      return server_send_error(conn, "missing project", NULL);

   const char *direction = jo_str(req, "direction", NULL);
   if (direction && strcmp(direction, "out") != 0 && strcmp(direction, "in") != 0 &&
       strcmp(direction, "both") != 0)
      return server_send_error(conn, "direction must be 'out', 'in', or 'both'", NULL);

   const char *status = jo_str(req, "status", NULL);
   if (status && strcmp(status, "ambiguous") != 0)
      return server_send_error(conn, "status must be 'ambiguous' or omitted", NULL);
   int status_ambiguous = (status != NULL);

   const char *min_tier = jo_str(req, "min_tier", NULL);
   if (min_tier && strcmp(min_tier, "high") != 0 && strcmp(min_tier, "medium") != 0 &&
       strcmp(min_tier, "tentative") != 0)
      return server_send_error(conn, "min_tier must be high, medium, or tentative", NULL);

   char *json =
       kb_client_index_cross_repo_deps_json(project, direction, min_tier, status_ambiguous);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service did not return cross-repo deps", NULL);
   return send_and_free(conn, resp);
}

/* S7: repo.trust — proxy the cross-repo trust write to the kb. The server op is
 * gated by CAP_INDEX_ADMIN (method_registry), which over TCP is local/owner-only;
 * the kb re-checks the owner credential. Inputs are validated here so a bad value
 * never reaches the kb, and the kb HTTP status is mapped to a precise client error
 * (404 no-such-project, 403 forbidden) instead of a blanket 502. `actor` is
 * caller-asserted (single-tenant P1); a server-derived principal is a future
 * hardening. */
int handle_repo_trust(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *project;
   if (jo_need_str(req, "project", &project) < 0 || !project[0])
      return server_send_error(conn, "missing project", NULL);
   const char *trust;
   if (jo_need_str(req, "trust", &trust) < 0 ||
       (strcmp(trust, "trusted") != 0 && strcmp(trust, "untrusted") != 0))
      return server_send_error(conn, "trust must be 'trusted' or 'untrusted'", NULL);
   const char *actor = jo_str(req, "actor", NULL);
   const char *request_id = jo_str(req, "request_id", NULL);

   int status = -1;
   char *json = kb_client_repo_trust_json(project, trust, actor, request_id, &status);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (resp)
      return send_and_free(conn, resp);
   if (status == 404)
      return server_send_error(conn, "no such project", NULL);
   if (status == 403)
      return server_send_error(conn, "forbidden: repo trust requires the owner credential", NULL);
   if (status == 400)
      return server_send_error(conn, "invalid trust request", NULL);
   return server_send_error(conn, "knowledge service did not apply the trust change", NULL);
}
