/* server_state_audit.c: split from server_state.c into a real translation unit
 * (was server_state_audit.inc, textually included only to stay under the
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

/* /v1/audit/trace handler. Its op (evidence.trace_retrieval_event) is a KB-only
 * action, so — like handle_kb_search and the curator handlers — the server
 * forwards it to aimee-kb via kb_client and passes the JSON response through.
 * rh_dispatch_op alone cannot reach a KB-only method (server_dispatch resolves
 * only server-side methods), which is why the cloned /v1/code/audit route never
 * reached the KB. */
int handle_evidence_trace(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *turn_id = jo_str(req, "turn_id", "");
   if (!turn_id[0])
      return server_send_error(conn, "audit trace requires turn_id", NULL);
   char *json = kb_client_evidence_trace_retrieval_event(turn_id);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service audit trace failed", NULL);
   return send_and_free(conn, resp);
}

/* /v1/audit/provenance handler. Like handle_evidence_trace, its op
 * (evidence.provenance_retrieval_event) is a KB-only action, so the server
 * forwards it to aimee-kb via kb_client and passes the JSON response through. */
int handle_evidence_provenance(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *turn_id = jo_str(req, "turn_id", "");
   if (!turn_id[0])
      return server_send_error(conn, "audit provenance requires turn_id", NULL);
   char *json = kb_client_evidence_provenance_retrieval_event(turn_id);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service audit provenance failed", NULL);
   return send_and_free(conn, resp);
}

/* /v1/audit/fidelity handler. Like the trace/provenance handlers, its op
 * (evidence.fidelity_retrieval_event) is a KB-only action (the fidelity_report /
 * fidelity_attribution artifacts live in DB2), so the server forwards it to
 * aimee-kb via kb_client and passes the JSON response through. */
int handle_evidence_fidelity(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *turn_id = jo_str(req, "turn_id", "");
   if (!turn_id[0])
      return server_send_error(conn, "audit fidelity requires turn_id", NULL);
   char *json = kb_client_evidence_fidelity_retrieval_event(turn_id);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service audit fidelity failed", NULL);
   return send_and_free(conn, resp);
}
