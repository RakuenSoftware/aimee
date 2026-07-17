#pragma once
#include "server.h"

int handle_trigger_fire(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_trigger_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_trigger_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_trigger_cancel(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* Manual one-at-a-time proposal fire (defined in trigger_scheduler.c): file exactly the
 * named pending proposal as a WFE work item, bypassing the default-off auto scan.
 * Returns 0 and fills out_id[80] on success; -1 if not found/already filed/error. */
int trigger_proposals_file_one(const char *workspace, const char *pipeline, const char *event_dir,
                               const char *ref, const char *mode, const char *proposal_name,
                               char out_id[80]);
