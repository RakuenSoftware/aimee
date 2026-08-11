/* memory_commands.c: the memory module declares its commands to the core table.
 *
 * FIRST GROUP PORTED onto src/command_registry.c. One registration per command,
 * naming the verb once, and every surface routes from it.
 *
 * WHAT THIS REPLACES for this group. `memory` was spelled three different ways
 * depending on where you stood:
 *
 *   CLI          `aimee memory get`      (rpc_routes[]: group "memory", verb "get")
 *   RPC          "memory.get"
 *   MCP dispatch `memory_get`   ... but `memory.search` is `search_memory`, VERB
 *                FIRST, so a mechanical CLI->MCP mapping cannot even tell "absent"
 *                from "spelled backwards" -- check_command_surface_parity.py has to
 *                try both orders and still cannot be sure.
 *   MCP shown    memory_recall and search_memory only; memory_get is registered but
 *                NOT in MCP_CORE_TOOLS, so no external client is shown it.
 *
 * That last line is not a cosmetic inconsistency. Standing guidance told agents to
 * call memory_get; agents could not see it, and called memory_recall instead --
 * observed on CT 403, the model quoting the guidance back and then ignoring it.
 *
 * Here the verb is "get", once. The dotted and spaced spellings are derived, and
 * the MCP name follows one rule instead of two.
 *
 * ADDITIVE FOR NOW: registering does not yet unregister the old entries, because a
 * surface still reads its own array. Deleting those arrays is the next step and has
 * to be done per-surface -- flipping 162 routes at once is how routing breaks
 * silently for a subset, which is the whole failure this table exists to end.
 */
#include "command_registry.h"
#include "server_mcp_internal.h" /* tool_memory_get / tool_search_memory / ... */
#include "server.h"              /* memory_store_command: the RPC handler, split */
#include "cJSON.h"
#include "log.h"

/* The registry hands a handler (args, ud); the existing implementations take
 * (args) and already return an owned cJSON. The MCP layer's own handlers are the
 * same one-line shape -- mcph_memory_get(c) is `return tool_memory_get(c->jargs)`
 * -- so this adapts to the identical function rather than to a second copy. */
#define MEMORY_CMD_ADAPTER(fn_name, impl)                                                          \
   static cJSON *fn_name(const cJSON *args, void *ud)                                              \
   {                                                                                               \
      (void)ud;                                                                                    \
      return impl((cJSON *)args);                                                                  \
   }

MEMORY_CMD_ADAPTER(cmd_memory_get, tool_memory_get)
MEMORY_CMD_ADAPTER(cmd_memory_search, tool_search_memory)
MEMORY_CMD_ADAPTER(cmd_memory_briefing, tool_memory_briefing)
MEMORY_CMD_ADAPTER(cmd_memory_mutate, tool_memory_mutate)
MEMORY_CMD_ADAPTER(cmd_memory_store, memory_store_command)
MEMORY_CMD_ADAPTER(cmd_memory_list, memory_list_command)
MEMORY_CMD_ADAPTER(cmd_memory_delete, memory_delete_command)

int memory_commands_register(void)
{
   const aimee_command_t cmds[] = {
       {.group = "memory",
        .verb = "get",
        .summary = "Read one memory by id.",
        .surfaces = AIMEE_SURFACE_ALL,
        /* DISCOVERABLE, not PROMINENT: the shown surface is a per-session tax on
         * every client and `memory_recall` already covers the common case. What
         * must not happen again is guidance NAMING this while it is hidden --
         * check_guidance_tool_parity.py now fails the build on exactly that. */
        .mcp_visibility = AIMEE_MCP_DISCOVERABLE,
        .fn = cmd_memory_get,
        .module = "memory"},
       {.group = "memory",
        .verb = "search",
        .summary = "Search memories.",
        .surfaces = AIMEE_SURFACE_ALL,
        .mcp_visibility = AIMEE_MCP_PROMINENT, /* shown today as search_memory */
        .fn = cmd_memory_search,
        .module = "memory"},
       {.group = "memory",
        .verb = "briefing",
        .summary = "Assemble the memory briefing.",
        .surfaces = AIMEE_SURFACE_ALL,
        .mcp_visibility = AIMEE_MCP_DISCOVERABLE,
        .fn = cmd_memory_briefing,
        .module = "memory"},
       {.group = "memory",
        .verb = "list",
        .summary = "List memories by tier/kind.",
        /* CLI and RPC only, same reasoning as store: an agent asking "what do you
         * know" wants recall or search, which rank; an unranked page of rows is an
         * operator view. */
        .surfaces = AIMEE_SURFACE_CLI | AIMEE_SURFACE_RPC,
        .mcp_visibility = AIMEE_MCP_DISCOVERABLE,
        .fn = cmd_memory_list,
        .module = "memory"},
       {.group = "memory",
        .verb = "delete",
        .summary = "Delete a memory by id.",
        /* CLI and RPC only. Deleting is destructive and irreversible from the
         * agent's side; an agent that decides a memory is wrong should supersede
         * it, which keeps the history the curator reasons over. */
        .surfaces = AIMEE_SURFACE_CLI | AIMEE_SURFACE_RPC,
        .mcp_visibility = AIMEE_MCP_DISCOVERABLE,
        .fn = cmd_memory_delete,
        .module = "memory"},
       {.group = "memory",
        .verb = "store",
        .summary = "Store a memory.",
        /* CLI and RPC ONLY, and that is a decision rather than an oversight.
         * Writing to memory is not something an external agent should reach for
         * mid-turn: aimee decides what is worth keeping through the curator, and a
         * tool letting any client write what it likes turns the store into a
         * scratchpad. Flagged explicitly because absence from a list is exactly how
         * the previous four tables came to disagree with nobody deciding anything.
         *
         * This is also the first command registered from the RPC side: its only
         * implementation was handle_memory_store, which WRITES to a connection and
         * returns int, so there was no result for a table to hand to any other
         * surface. memory_store_command is that handler split in two -- the logic
         * returns a result, the handler writes it -- which is the shape every
         * surface needs and the change the other ~230 RPC handlers need too. */
        .surfaces = AIMEE_SURFACE_CLI | AIMEE_SURFACE_RPC,
        .mcp_visibility = AIMEE_MCP_DISCOVERABLE, /* unused while MCP is not a surface here */
        .fn = cmd_memory_store,
        .module = "memory"},
       {.group = "memory",
        .verb = "mutate",
        .summary = "Apply a memory mutation.",
        .surfaces = AIMEE_SURFACE_ALL,
        .mcp_visibility = AIMEE_MCP_DISCOVERABLE,
        .fn = cmd_memory_mutate,
        .module = "memory"},
   };
   int rc = aimee_command_register_many(cmds, sizeof cmds / sizeof cmds[0]);
   if (rc != 0)
      LOG_WARN("commands", "memory module failed to register its commands");
   return rc;
}
