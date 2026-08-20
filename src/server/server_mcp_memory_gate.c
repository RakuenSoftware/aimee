/* server_mcp_memory_gate.c: the authorization decisions for the MCP memory tools
 * that can destroy stored data. See server_mcp_memory_gate.h for why they live
 * here rather than inline in server_mcp_call_table.c. */
#include "server_mcp_memory_gate.h"
#include "server.h" /* CAP_MEMORY_ADMIN / CAP_MEMORY_WRITE */
#include "aimee.h"
#include "memory.h" /* MEMORY_MAINTENANCE_MODE_PRUNE / _MODES_DEFAULT */
#include <string.h>

const char *mcp_mutate_verb_method(const char *verb)
{
   if (!verb)
      return NULL;
   if (strcmp(verb, "store") == 0)
      return "memory.store";
   if (strcmp(verb, "update") == 0)
      return "memory.update";
   if (strcmp(verb, "supersede") == 0)
      return "memory.supersede";
   /* The destructive one, and the reason this table exists: the RPC method twin
    * requires CAP_MEMORY_ADMIN while the MCP tool required nothing. */
   if (strcmp(verb, "forget") == 0)
      return "memory.delete";
   if (strcmp(verb, "affirm") == 0)
      return "memory.touch";
   if (strcmp(verb, "reject") == 0)
      return "memory.reject";
   return NULL;
}

uint32_t mcp_memory_maintain_required_cap(unsigned int modes)
{
   unsigned int effective = modes ? modes : (unsigned int)MEMORY_MAINTENANCE_MODES_DEFAULT;
   return (effective & MEMORY_MAINTENANCE_MODE_PRUNE) ? (uint32_t)CAP_MEMORY_ADMIN
                                                      : (uint32_t)CAP_MEMORY_WRITE;
}
