#ifndef SERVER_MCP_ROUNDTABLE_H
#define SERVER_MCP_ROUNDTABLE_H

#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>

/* Long-running roundtables use the server's asynchronous op-run lifecycle.
 * Submission returns a queued snapshot immediately; status reads that same
 * snapshot until it reaches completed, failed, or cancelled. */
cJSON *mcp_roundtable_submit(cJSON *args, uint32_t capabilities, char *err, size_t err_n);
cJSON *mcp_roundtable_status(cJSON *args, char *err, size_t err_n);

#endif
