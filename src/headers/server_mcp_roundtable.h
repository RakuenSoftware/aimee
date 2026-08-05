#ifndef SERVER_MCP_ROUNDTABLE_H
#define SERVER_MCP_ROUNDTABLE_H

#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>

/* Runs a roundtable and returns its verdict. Blocks for as long as the review
 * takes: the caller waits on aimee-server, aimee-server waits on the bus, and
 * the roundtable waits on whichever model its panel is configured with. There
 * is no run id and no status call -- see server_mcp_roundtable.c for why the
 * asynchronous submit/poll pair this replaced was so expensive. */
cJSON *mcp_roundtable_review(cJSON *args, uint32_t capabilities, char *err, size_t err_n);

#endif
