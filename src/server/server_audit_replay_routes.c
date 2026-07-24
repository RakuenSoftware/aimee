/* server_audit_replay_routes.c: the /v1/audit capture-replay HTTP handlers.
 *
 * Split out of server_state.c (which was at the 2500-line cap). These expose the
 * audit-on-bus capture streams — recorded by modules/audit/audit_bus.c — over the
 * /v1 surface as a dashboard read: list the capture files, and replay one file's
 * governed-action rows as JSON. The heavy lifting (reading + decoding a capture,
 * the byte-budget paging, the path-traversal-safe basename check) lives in
 * modules/audit/audit_replay.c; these are thin request/response adapters. */
#include "server.h"

#include "audit_replay.h"          /* audit_replay_capture_list / _to_json / _valid_basename */
#include "config.h"                /* config_default_dir */
#include "json_fluent.h"           /* jo_ok / jo_err */
#include "server_state_internal.h" /* send_and_free */

/* GET /v1/audit/captures: list the audit-on-bus capture files (the recorded
 * governed-action event streams) available for replay. A dashboard read. */
int handle_audit_captures(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "captures", audit_replay_capture_list(config_default_dir()));
   return send_and_free(conn, resp);
}

/* POST /v1/audit/replay {"file":"audit-bus-capture-...aimeecap"}: replay one
 * capture file's governed-action rows in order, as JSON. Observational — nothing
 * is re-executed. The file name is validated as a bare capture basename before it
 * is joined to the capture directory, so it cannot escape it (no traversal). */
int handle_audit_replay(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *file = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "file"));
   if (!file || !audit_replay_valid_basename(file))
      return send_and_free(conn, jo_err("file must be an audit capture file name"));

   /* Bound the response: /v1 replies share a 256 KB buffer, and a full stream can
    * be far larger. Default to the newest-friendly first page; cap the window so
    * the JSON always fits. The unbounded dump is the CLI (--audit-replay). */
   cJSON *jlim = cJSON_GetObjectItemCaseSensitive(req, "limit");
   cJSON *joff = cJSON_GetObjectItemCaseSensitive(req, "offset");
   long limit = cJSON_IsNumber(jlim) ? (long)jlim->valuedouble : 500;
   long offset = cJSON_IsNumber(joff) ? (long)joff->valuedouble : 0;
   if (limit <= 0 || limit > 1000)
      limit = 1000;
   if (offset < 0)
      offset = 0;

   char path[4096];
   snprintf(path, sizeof path, "%s/%s", config_default_dir(), file);
   cJSON *replay = audit_replay_to_json(path, offset, limit);
   if (!replay)
      return send_and_free(conn, jo_err("capture file not found"));

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "file", file);
   cJSON_AddItemToObject(resp, "replay", replay);
   return send_and_free(conn, resp);
}
