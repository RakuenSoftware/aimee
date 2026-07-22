#ifndef AIMEE_WFE_ROUNDTABLE_PROXY_H
#define AIMEE_WFE_ROUNDTABLE_PROXY_H

#include "server.h"

/* Temporary resource-plane adapter: roundtable policy and execution live in
 * Go. C only forwards the already-authorized op payload over the private UDS. */
int wfe_roundtable_proxy(server_conn_t *conn, const cJSON *request);
int handle_roundtable_review_proxy(server_ctx_t *ctx, server_conn_t *conn, cJSON *request);

#endif
