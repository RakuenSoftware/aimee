/* kb_client_roadmap.c: request-side glue from aimee (CLI/server) to the
 * aimee-kb sidecar for the spec-driven roadmap artifact ops.
 *
 * Roadmap / plan_unit artifacts live in KB_STORE, which only aimee-kb may touch
 * (3db boundary). These thin wrappers build the request JSON and hand it to
 * kb_v1_action_request(), which speaks the v1 action protocol over the kb
 * socket and returns the heap-allocated JSON response string (caller frees).
 *
 * See src/kb/kb_service_roadmap.c for the matching handlers and
 * docs/proposals/pending/spec-driven-roadmaps-and-autonomous-delegate-dispatch.md
 */

#include "kb_client.h"
#include "kb_client_memory_internal.h" /* kb_v1_action_request */

#include <cJSON.h>

char *kb_client_roadmap_show_json(const char *roadmap_id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "roadmap_id", roadmap_id ? roadmap_id : "");
   return kb_v1_action_request("roadmap.show", req);
}

char *kb_client_roadmap_list_json(void)
{
   cJSON *req = cJSON_CreateObject();
   return kb_v1_action_request("roadmap.list", req);
}
