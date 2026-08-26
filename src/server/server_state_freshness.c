#include "server_state_internal.h"
#include "aimee.h"
#include "server.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <aimee/core/turn_integrity.h>

int handle_curator_invalidated(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *source_kind = jo_str(req, "source_kind", "");
   const char *source_id = jo_str(req, "source_id", "");
   int stale = jo_int(req, "artifacts_stale", 0);
   int valid_source = stale > 0 && source_kind[0] && source_id[0];
   unsigned long long scoped_epoch =
       valid_source ? ti_knowledge_epoch_advance(source_kind, source_id, "curator invalidated") : 0;
   unsigned long long global_epoch =
       valid_source ? ti_knowledge_epoch_advance("knowledge", "global", source_kind) : 0;
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "received", stale);
   cJSON_AddNumberToObject(resp, "scoped_epoch", (double)scoped_epoch);
   cJSON_AddNumberToObject(resp, "knowledge_epoch", (double)global_epoch);
   return send_and_free(conn, resp);
}
