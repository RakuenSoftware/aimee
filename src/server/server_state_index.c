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
 * API client gets an actionable error rather than an opaque 502: the kb's
 * canonical query is OUT-only today (S4a), so direction=in/both is rejected here
 * instead of letting the kb 501 collapse into a bad-gateway. Validation runs
 * before any allocation, so the error paths cannot leak. */
int handle_index_deps(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *project;
   if (jo_need_str(req, "project", &project) < 0 || !project[0])
      return server_send_error(conn, "missing project", NULL);

   const char *direction = jo_str(req, "direction", NULL);
   if (direction && strcmp(direction, "out") != 0)
      return server_send_error(
          conn, "direction not supported (only 'out'); reverse traversal is a later slice", NULL);

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
