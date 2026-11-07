#ifndef AIMEE_SCRIPT_TOOL_EXECUTION_H
#define AIMEE_SCRIPT_TOOL_EXECUTION_H
#include "cJSON.h"
/* Internal RPC entry adapter. Owns returned text; canonicalizes against the
 * current admitted definition before every permission/directive/policy gate.
 * attempt is issued by the host transport, never extracted from frame data. */
char *server_script_tool_execute(const cJSON *frame, const char *attempt, int *is_error);
#endif
