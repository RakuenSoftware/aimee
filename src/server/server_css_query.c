/* server_css_query.c: split from server_state.c into a real translation unit
 * (was server_css_query.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#include "server_state_internal.h"
#include "aimee.h"
#include "server.h"
#include "dashboard.h"
#include "lsp.h"
#include "platform_path.h"
#include <aimee/workspace/workspace.h>
#include "modules/workspace/workspace_mirror.h"
#include "modules/workspace/workspace_provider.h"
#include "modules/workspace/workspace_handle.h"
#include "modules/workspace/workspace_runner_registry.h"
#include "modules/git/forge_credentials.h"
#include "db1_client/db1.h"
#include "kb_client.h"
#include "compute_pool.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "dogfood.h"
#include "commands.h"
#include "platform_path.h"
#include "server_http.h"  /* session_primary_set/get/clear */
#include "agent_config.h" /* agent_load_config / agent_find */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Defined in kb_client.c; takes ownership of req. */
char *kb_v1_action_request(const char *method, cJSON *req);

int handle_css_signals(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *op = jo_str(req, "op", "");
   const char *project = jo_str(req, "project", "");
   if (!op[0])
      return server_send_error(conn, "css requires an 'op'", NULL);
   if (!project[0])
      return server_send_error(conn, "css requires a 'project'", NULL);

   /* Forward the whole validated request so op-specific fields (render-store's
    * unit/phase/snapshot, render-verify's unit, ...) pass through unchanged. */
   cJSON *kreq = cJSON_Duplicate(req, 1);
   char *json = kb_v1_action_request("css.signals", kreq);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "css signals query failed", NULL);
   return send_and_free(conn, resp);
}
