/* server_pipeline.h: server-side handlers for the `pipeline.*` method family —
 * the roundtable authoring pipeline control surface (proposal sections 1/3/5).
 * Distinct from the autopilot `pipelines` table and its `autopilot` handler. */
#ifndef DEC_SERVER_PIPELINE_H
#define DEC_SERVER_PIPELINE_H 1

#include "cJSON.h"
#include "server.h"

#ifdef __cplusplus
extern "C"
{
#endif

   int handle_pipeline_start(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
   int handle_pipeline_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
   int handle_pipeline_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
   int handle_pipeline_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
   int handle_pipeline_resume(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
   int handle_pipeline_advance(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
   int handle_pipeline_gate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

#ifdef __cplusplus
}
#endif

#endif /* DEC_SERVER_PIPELINE_H */
