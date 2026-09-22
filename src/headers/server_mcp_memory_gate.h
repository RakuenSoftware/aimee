#ifndef SERVER_MCP_MEMORY_GATE_H
#define SERVER_MCP_MEMORY_GATE_H
#include <stdint.h>
struct cJSON;

/* Mutation verb routing is still native pending its own migration. */
const char *mcp_mutate_verb_method(const char *verb);

/* Execute the shared Go owner's maintenance plan. Capabilities come only from
 * the authenticated host connection, never from tool arguments. */
struct cJSON *server_mcp_memory_maintain_command(uint32_t capabilities, const struct cJSON *args);
#endif
