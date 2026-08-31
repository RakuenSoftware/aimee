#ifndef AIMEE_ROUNDTABLE_REVIEW_BUS_H
#define AIMEE_ROUNDTABLE_REVIEW_BUS_H

#include "server.h"

/* The deadline rule and the panel resolver belong to the roundtable module; the
 * bus only carries the result. */
#include <aimee/roundtable/review_panel.h>

int handle_roundtable_review(server_ctx_t *ctx, server_conn_t *conn, cJSON *request);

#endif
